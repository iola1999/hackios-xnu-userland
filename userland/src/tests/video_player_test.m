/*
 * Play a local video through AVPlayer and inspect AVPlayerItemVideoOutput NV12 frames.
 * Usage: video_player_test [--deadline SECONDS] FILE
 * Default deadline: 12 seconds, including startup, playback and cleanup.
 * Exit: 0 ready, playing, advancing clock, valid pixels and end notification observed;
 *       1 input/player/pixel failure; 2 arguments; 124 whole-process deadline.
 * The player retains its default decoder, buffering and audio-session behavior.
 * Pixel hashes cover active NV12 bytes, excluding row padding. Logs omit paths,
 * NSError descriptions and userInfo. This probe does not present frames.
 */
#import <AVFoundation/AVFoundation.h>
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

static void
deadline_expired(int signal_number)
{
	static const char message[] = "video_player_test: stage=deadline result=fail exit=124\n";
	(void)signal_number;
	(void)write(STDERR_FILENO, message, sizeof(message) - 1);
	_exit(124);
}

static void
report_error(const char *stage, NSError *error)
{
	const char *domain = error == nil ? "none" : "other";
	if ([error.domain isEqualToString:AVFoundationErrorDomain]) domain = "avfoundation";
	else if ([error.domain isEqualToString:NSOSStatusErrorDomain]) domain = "osstatus";
	else if ([error.domain isEqualToString:NSPOSIXErrorDomain]) domain = "posix";
	else if ([error.domain isEqualToString:NSCocoaErrorDomain]) domain = "cocoa";
	else if ([error.domain isEqualToString:NSURLErrorDomain]) domain = "url";
	printf("video_player_test: stage=%s error_domain=%s error=%ld\n", stage, domain, (long)error.code);
}

/* Read every active byte while the callback owns a valid, read-only image buffer. */
static int
pixel_summary(CVPixelBufferRef image, uint64_t *hash, uint64_t *bytes, unsigned *low, unsigned *high)
{
	OSType format = CVPixelBufferGetPixelFormatType(image);
	if ((format != kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange &&
	    format != kCVPixelFormatType_420YpCbCr8BiPlanarFullRange) ||
	    CVPixelBufferGetPlaneCount(image) != 2) return EINVAL;
	CVReturn status = CVPixelBufferLockBaseAddress(image, kCVPixelBufferLock_ReadOnly);
	if (status != kCVReturnSuccess) return status;
	int error = 0;
	*hash = UINT64_C(14695981039346656037);
	*bytes = 0;
	*low = 255;
	*high = 0;
	for (size_t plane = 0; plane < 2; plane++) {
		size_t width = CVPixelBufferGetWidthOfPlane(image, plane);
		size_t height = CVPixelBufferGetHeightOfPlane(image, plane);
		size_t stride = CVPixelBufferGetBytesPerRowOfPlane(image, plane);
		const uint8_t *base = CVPixelBufferGetBaseAddressOfPlane(image, plane);
		size_t components = plane == 0 ? 1 : 2;
		if (width == 0 || height == 0 || width > SIZE_MAX / components ||
		    stride < width * components || height > SIZE_MAX / stride || base == NULL) {
			error = EINVAL;
			break;
		}
		for (size_t row = 0; row < height; row++) {
			for (size_t column = 0; column < width * components; column++) {
				unsigned value = base[row * stride + column];
				*hash = (*hash ^ value) * UINT64_C(1099511628211);
				if (value < *low) *low = value;
				if (value > *high) *high = value;
			}
		}
		*bytes += width * components * height;
	}
	status = CVPixelBufferUnlockBaseAddress(image, kCVPixelBufferLock_ReadOnly);
	return error ? error : status;
}

static int
play(const char *path)
{
	struct stat st;
	if (stat(path, &st) != 0) {
		printf("video_player_test: stage=input errno=%d\n", errno);
		return 1;
	}
	if (!S_ISREG(st.st_mode)) {
		printf("video_player_test: stage=input error=not_regular_file\n");
		return 1;
	}
	AVPlayer *player = nil;
	AVPlayerItem *item = nil;
	AVPlayerItemVideoOutput *output = nil;
	NSNotificationCenter *center = NSNotificationCenter.defaultCenter;
	NSMutableArray *observers = [NSMutableArray array];
	__block bool ended = false, failed = false;
	__block unsigned stalls = 0;
	bool ready = false, playing = false, advanced = false;
	unsigned frames = 0, pixel_errors = 0, empty = 0;
	uint64_t bytes = 0;
	CMTime previous = kCMTimeInvalid, maximum = kCMTimeZero;
	@try {
		NSURL *url = [NSURL fileURLWithPath:@(path)];
		item = [AVPlayerItem playerItemWithURL:url];
		output = [[AVPlayerItemVideoOutput alloc] initWithPixelBufferAttributes:@{
			(__bridge NSString *)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange)
		}];
		[item addOutput:output];
		player = [AVPlayer playerWithPlayerItem:item];
		[observers addObject:[center addObserverForName:AVPlayerItemDidPlayToEndTimeNotification object:item
		    queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *notification) {
			(void)notification;
			ended = true;
			printf("video_player_test: stage=end received=1\n");
		}]];
		[observers addObject:[center addObserverForName:AVPlayerItemFailedToPlayToEndTimeNotification object:item
		    queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *notification) {
			failed = true;
			report_error("failed_to_end", notification.userInfo[AVPlayerItemFailedToPlayToEndTimeErrorKey]);
		}]];
		[observers addObject:[center addObserverForName:AVPlayerItemPlaybackStalledNotification object:item
		    queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *notification) {
			(void)notification;
			stalls++;
			printf("video_player_test: stage=stalled count=%u\n", stalls);
		}]];
		printf("video_player_test: stage=start\n");
		[player play];
		NSInteger last_player = -1, last_item = -1, last_control = -1;
		double next_clock = 0;
		while (!failed) {
			@autoreleasepool {
				NSInteger ps = player.status, is = item.status, control = player.timeControlStatus;
				if (ps != last_player || is != last_item || control != last_control) {
					printf("video_player_test: stage=state player_status=%ld item_status=%ld time_control=%ld rate=%.3f\n",
					    (long)ps, (long)is, (long)control, player.rate);
					last_player = ps; last_item = is; last_control = control;
				}
				ready |= ps == AVPlayerStatusReadyToPlay && is == AVPlayerItemStatusReadyToPlay;
				playing |= control == AVPlayerTimeControlStatusPlaying;
				if (ps == AVPlayerStatusFailed || is == AVPlayerItemStatusFailed) {
					report_error("player", player.error);
					report_error("item", item.error);
					failed = true;
					break;
				}
				CMTime current = player.currentTime;
				if (CMTIME_IS_NUMERIC(current)) {
					advanced |= CMTIME_IS_NUMERIC(previous) && CMTimeCompare(current, previous) > 0;
					if (CMTimeCompare(current, maximum) > 0) maximum = current;
					previous = current;
				}
				double host = CMTimeGetSeconds(CMClockGetTime(CMClockGetHostTimeClock()));
				if (host >= next_clock || ended) {
					printf("video_player_test: stage=clock time=%" PRId64 "/%d flags=0x%x\n",
					    current.value, current.timescale, (unsigned)current.flags);
					next_clock = host + 0.1;
				}
				CMTime request = [output itemTimeForHostTime:host];
				if ([output hasNewPixelBufferForItemTime:request]) {
					CMTime pts = kCMTimeInvalid;
					CVPixelBufferRef image = [output copyPixelBufferForItemTime:request itemTimeForDisplay:&pts];
					if (image != NULL) {
						uint64_t hash = 0, frame_bytes = 0;
						unsigned low = 0, high = 0;
						int error = pixel_summary(image, &hash, &frame_bytes, &low, &high);
						pixel_errors += error != 0;
						frames += error == 0;
						bytes += error == 0 ? frame_bytes : 0;
						printf("video_player_test: stage=frame index=%u pts=%" PRId64 "/%d pts_flags=0x%x"
						    " format=0x%08x width=%zu height=%zu pixel_error=%d bytes=%" PRIu64
						    " fnv1a64=%016" PRIx64 " min=%u max=%u\n", frames, pts.value, pts.timescale,
						    (unsigned)pts.flags, (unsigned)CVPixelBufferGetPixelFormatType(image),
						    CVPixelBufferGetWidth(image), CVPixelBufferGetHeight(image), error, frame_bytes, hash, low, high);
						CVPixelBufferRelease(image);
						if (error != 0) { failed = true; break; }
					} else {
						empty++;
						printf("video_player_test: stage=empty count=%u\n", empty);
					}
				}
				if (ended) break;
				[NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.005]];
			}
		}
	} @catch (NSException *exception) {
		(void)exception;
		printf("video_player_test: stage=exception result=fail\n");
		failed = true;
	}
	for (id observer in observers) [center removeObserver:observer];
	[player pause];
	if (output != nil) [item removeOutput:output];
	[player replaceCurrentItemWithPlayerItem:nil];
	failed |= !ready || !playing || !advanced || !ended || frames == 0 || pixel_errors != 0;
	printf("video_player_test: stage=summary result=%s ready=%d playing=%d advanced=%d ended=%d"
	    " frames=%u pixel_errors=%u empty=%u stalls=%u output_bytes=%" PRIu64 " max_time=%" PRId64 "/%d\n",
	    failed ? "fail" : "pass", ready, playing, advanced, ended, frames, pixel_errors, empty, stalls,
	    bytes, maximum.value, maximum.timescale);
	return failed ? 1 : 0;
}

int
main(int argc, char **argv)
{
	double deadline = 12;
	const char *path = NULL;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--deadline") && i + 1 < argc) {
			char *end = NULL;
			errno = 0;
			deadline = strtod(argv[++i], &end);
			if (errno || end == argv[i] || *end || !isfinite(deadline) || deadline <= 0 || deadline > 3600) goto usage;
		} else if (argv[i][0] != '-' && path == NULL) {
			path = argv[i];
		} else goto usage;
	}
	if (path == NULL) goto usage;
	setvbuf(stdout, NULL, _IOLBF, 0);
	struct sigaction action = { .sa_handler = deadline_expired };
	sigemptyset(&action.sa_mask);
	sigset_t alarm_mask;
	sigemptyset(&alarm_mask);
	sigaddset(&alarm_mask, SIGALRM);
	struct itimerval timer = { .it_value = {
		.tv_sec = (time_t)deadline,
		.tv_usec = (suseconds_t)((deadline - floor(deadline)) * 1000000)
	} };
	if (timer.it_value.tv_sec == 0 && timer.it_value.tv_usec == 0) timer.it_value.tv_usec = 1;
	if (sigaction(SIGALRM, &action, NULL) != 0 || sigprocmask(SIG_UNBLOCK, &alarm_mask, NULL) != 0 ||
	    setitimer(ITIMER_REAL, &timer, NULL) != 0) {
		printf("video_player_test: stage=deadline_setup errno=%d\n", errno);
		return 1;
	}
	@autoreleasepool {
		return play(path);
	}
usage:
	fprintf(stderr, "usage: video_player_test [--deadline SECONDS] FILE\n");
	return 2;
}
