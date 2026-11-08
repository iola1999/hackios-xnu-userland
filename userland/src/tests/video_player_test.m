/*
 * Play a file or HTTP(S) video through AVPlayer and inspect its NV12 output frames.
 * Usage: video_player_test [--deadline SECONDS] FILE | --url HTTP_URL
 * Default deadline: 150 seconds, including cold startup, playback and cleanup.
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
#include <time.h>
#include <unistd.h>

#define SUMMARY_LINE_SIZE 1024
#define FRAME_LOG_EVERY 60

struct summary_snapshot {
	bool ready, playing, advanced, ended, failed, finished;
	unsigned frames, frame_logs, pixel_errors, empty, stalls;
	uint64_t bytes, elapsed_ms, first_frame_ms;
	CMTime maximum;
};

static char summary_lines[2][SUMMARY_LINE_SIZE];
static volatile sig_atomic_t summary_lengths[2];
static volatile sig_atomic_t summary_index;
static uint64_t started_monotonic_ms;

static uint64_t
monotonic_milliseconds(void)
{
	struct timespec now;
	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
	return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
}

static uint64_t
elapsed_milliseconds(void)
{
	uint64_t now = monotonic_milliseconds();
	return started_monotonic_ms != 0 && now >= started_monotonic_ms ? now - started_monotonic_ms : 0;
}

static bool
summary_passed(const struct summary_snapshot *snapshot)
{
	return snapshot->finished && !snapshot->failed && snapshot->ready && snapshot->playing && snapshot->advanced &&
	    snapshot->ended && snapshot->frames != 0 && snapshot->pixel_errors == 0;
}

static int
format_summary(char *line, size_t size, const struct summary_snapshot *snapshot)
{
	return snprintf(line, size, "video_player_test: stage=summary result=%s ready=%d playing=%d advanced=%d ended=%d"
	    " frames=%u frame_logs=%u pixel_errors=%u empty=%u stalls=%u output_bytes=%" PRIu64
	    " elapsed_ms=%" PRIu64 " first_frame_ms=%" PRIu64 " max_time=%" PRId64 "/%d\n",
	    summary_passed(snapshot) ? "pass" : "fail", snapshot->ready, snapshot->playing, snapshot->advanced,
	    snapshot->ended, snapshot->frames, snapshot->frame_logs, snapshot->pixel_errors, snapshot->empty,
	    snapshot->stalls, snapshot->bytes, snapshot->elapsed_ms, snapshot->first_frame_ms,
	    snapshot->maximum.value, snapshot->maximum.timescale);
}

/* Publish complete buffers so the alarm handler never formats text or calls Objective-C. */
static void
publish_summary(struct summary_snapshot *snapshot)
{
	snapshot->elapsed_ms = elapsed_milliseconds();
	int next = summary_index ^ 1;
	int length = format_summary(summary_lines[next], sizeof(summary_lines[next]), snapshot);
	if (length < 0) return;
	if ((size_t)length >= sizeof(summary_lines[next])) length = sizeof(summary_lines[next]) - 1;
	summary_lengths[next] = length;
	__atomic_store_n(&summary_index, next, __ATOMIC_RELEASE);
}

static int
complete_summary(struct summary_snapshot *snapshot)
{
	struct itimerval disabled = { 0 };
	(void)setitimer(ITIMER_REAL, &disabled, NULL);
	snapshot->finished = true;
	publish_summary(snapshot);
	char line[SUMMARY_LINE_SIZE];
	int length = format_summary(line, sizeof(line), snapshot);
	if (length > 0) fwrite(line, 1, (size_t)length, stdout);
	return summary_passed(snapshot) ? 0 : 1;
}

static void
deadline_expired(int signal_number)
{
	static const char message[] = "video_player_test: stage=deadline result=fail exit=124\n";
	int current = __atomic_load_n(&summary_index, __ATOMIC_ACQUIRE) & 1;
	int length = summary_lengths[current];
	(void)signal_number;
	if (length > 0 && length <= SUMMARY_LINE_SIZE) (void)write(STDOUT_FILENO, summary_lines[current], (size_t)length);
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

static const char *
waiting_reason(AVPlayer *player)
{
	NSString *reason = player.reasonForWaitingToPlay;
	if (reason == nil) return "none";
	if ([reason isEqualToString:AVPlayerWaitingToMinimizeStallsReason]) return "to_minimize_stalls";
	if ([reason isEqualToString:AVPlayerWaitingWhileEvaluatingBufferingRateReason]) return "evaluating_buffering_rate";
	if ([reason isEqualToString:AVPlayerWaitingWithNoItemToPlayReason]) return "no_item_to_play";
	if ([reason isEqualToString:AVPlayerWaitingForCoordinatedPlaybackReason]) return "coordinated_playback";
	return "other";
}

static void
print_loaded_ranges(AVPlayerItem *item)
{
	NSArray<NSValue *> *ranges = item.loadedTimeRanges;
	if (ranges.count == 0) {
		printf("none");
		return;
	}
	NSUInteger shown = ranges.count < 8 ? ranges.count : 8;
	for (NSUInteger index = 0; index < shown; index++) {
		CMTimeRange range = ranges[index].CMTimeRangeValue;
		double start = CMTimeGetSeconds(range.start), duration = CMTimeGetSeconds(range.duration);
		printf("%s", index == 0 ? "" : ",");
		if (isfinite(start) && isfinite(duration)) printf("%.3f+%.3f", start, duration);
		else printf("invalid");
	}
	if (shown < ranges.count) printf(",+%lu", (unsigned long)(ranges.count - shown));
}

static void
report_player_state(AVPlayer *player, AVPlayerItem *item, AVPlayerItemVideoOutput *output, unsigned stalls)
{
	double host = CMTimeGetSeconds(CMClockGetTime(CMClockGetHostTimeClock()));
	CMTime request = [output itemTimeForHostTime:host];
	CMTime current = player.currentTime;
	printf("video_player_test: stage=state elapsed_ms=%" PRIu64 " player_status=%ld item_status=%ld"
	    " time_control=%ld rate=%.3f waiting_reason=%s buffer_empty=%d likely_to_keep_up=%d stalls=%u"
	    " current_time=%" PRId64 "/%d output_time=%" PRId64 "/%d has_new=%d loaded=",
	    elapsed_milliseconds(), (long)player.status, (long)item.status, (long)player.timeControlStatus, player.rate,
	    waiting_reason(player), item.playbackBufferEmpty, item.playbackLikelyToKeepUp, stalls,
	    current.value, current.timescale, request.value, request.timescale,
	    [output hasNewPixelBufferForItemTime:request]);
	print_loaded_ranges(item);
	printf("\n");
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
play(const char *path, bool remote)
{
	__block struct summary_snapshot summary = { .maximum = kCMTimeZero };
	started_monotonic_ms = monotonic_milliseconds();
	publish_summary(&summary);
	struct timeval wall;
	uint64_t epoch_us = gettimeofday(&wall, NULL) == 0 ? (uint64_t)wall.tv_sec * 1000000 + (uint64_t)wall.tv_usec : 0;
	printf("video_player_test: stage=start epoch_us=%" PRIu64 "\n", epoch_us);
	struct stat st;
	if (!remote && stat(path, &st) != 0) {
		printf("video_player_test: stage=input errno=%d\n", errno);
		summary.failed = true;
		return complete_summary(&summary);
	}
	if (!remote && !S_ISREG(st.st_mode)) {
		printf("video_player_test: stage=input error=not_regular_file\n");
		summary.failed = true;
		return complete_summary(&summary);
	}
	AVPlayer *player = nil;
	AVPlayerItem *item = nil;
	AVPlayerItemVideoOutput *output = nil;
	NSNotificationCenter *center = NSNotificationCenter.defaultCenter;
	NSMutableArray *observers = [NSMutableArray array];
	CMTime previous = kCMTimeInvalid;
	@try {
		NSURL *url = remote ? [NSURL URLWithString:@(path)] : [NSURL fileURLWithPath:@(path)];
		if (remote && (!url.host.length ||
		    (![url.scheme isEqualToString:@"http"] && ![url.scheme isEqualToString:@"https"]))) {
			printf("video_player_test: stage=input error=invalid_http_url\n");
			summary.failed = true;
			return complete_summary(&summary);
		}
		item = [AVPlayerItem playerItemWithURL:url];
		printf("video_player_test: stage=item_created elapsed_ms=%" PRIu64 "\n", elapsed_milliseconds());
		output = [[AVPlayerItemVideoOutput alloc] initWithPixelBufferAttributes:@{
			(__bridge NSString *)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange)
		}];
		[item addOutput:output];
		printf("video_player_test: stage=output_added elapsed_ms=%" PRIu64 "\n", elapsed_milliseconds());
		player = [AVPlayer playerWithPlayerItem:item];
		printf("video_player_test: stage=player_created elapsed_ms=%" PRIu64 "\n", elapsed_milliseconds());
		[observers addObject:[center addObserverForName:AVPlayerItemDidPlayToEndTimeNotification object:item
		    queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *notification) {
			(void)notification;
			summary.ended = true;
			printf("video_player_test: stage=end received=1 elapsed_ms=%" PRIu64 "\n", elapsed_milliseconds());
		}]];
		[observers addObject:[center addObserverForName:AVPlayerItemFailedToPlayToEndTimeNotification object:item
		    queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *notification) {
			summary.failed = true;
			report_error("failed_to_end", notification.userInfo[AVPlayerItemFailedToPlayToEndTimeErrorKey]);
		}]];
		[observers addObject:[center addObserverForName:AVPlayerItemPlaybackStalledNotification object:item
		    queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *notification) {
			(void)notification;
			summary.stalls++;
			printf("video_player_test: stage=stalled count=%u elapsed_ms=%" PRIu64 "\n",
			    summary.stalls, elapsed_milliseconds());
			report_player_state(player, item, output, summary.stalls);
		}]];
		printf("video_player_test: stage=play elapsed_ms=%" PRIu64 "\n", elapsed_milliseconds());
		[player play];
		printf("video_player_test: stage=play_returned elapsed_ms=%" PRIu64 "\n", elapsed_milliseconds());
		NSInteger last_player = -1, last_item = -1, last_control = -1;
		double next_clock = 0, next_state = 0;
		while (!summary.failed) {
			@autoreleasepool {
				NSInteger ps = player.status, is = item.status, control = player.timeControlStatus;
				double host = CMTimeGetSeconds(CMClockGetTime(CMClockGetHostTimeClock()));
				if (ps != last_player || is != last_item || control != last_control || host >= next_state) {
					report_player_state(player, item, output, summary.stalls);
					last_player = ps; last_item = is; last_control = control;
					next_state = host + 1.0;
				}
				summary.ready |= ps == AVPlayerStatusReadyToPlay && is == AVPlayerItemStatusReadyToPlay;
				summary.playing |= control == AVPlayerTimeControlStatusPlaying;
				if (ps == AVPlayerStatusFailed || is == AVPlayerItemStatusFailed) {
					report_error("player", player.error);
					report_error("item", item.error);
					summary.failed = true;
					break;
				}
				CMTime current = player.currentTime;
				if (CMTIME_IS_NUMERIC(current)) {
					summary.advanced |= CMTIME_IS_NUMERIC(previous) && CMTimeCompare(current, previous) > 0;
					if (CMTimeCompare(current, summary.maximum) > 0) summary.maximum = current;
					previous = current;
				}
				if (host >= next_clock || summary.ended) {
					printf("video_player_test: stage=clock elapsed_ms=%" PRIu64 " time=%" PRId64 "/%d flags=0x%x\n",
					    elapsed_milliseconds(), current.value, current.timescale, (unsigned)current.flags);
					next_clock = host + 1.0;
				}
				CMTime request = [output itemTimeForHostTime:host];
				if ([output hasNewPixelBufferForItemTime:request]) {
					CMTime pts = kCMTimeInvalid;
					CVPixelBufferRef image = [output copyPixelBufferForItemTime:request itemTimeForDisplay:&pts];
					if (image != NULL) {
						uint64_t hash = 0, frame_bytes = 0;
						unsigned low = 0, high = 0;
						int error = pixel_summary(image, &hash, &frame_bytes, &low, &high);
						summary.pixel_errors += error != 0;
						summary.frames += error == 0;
						summary.bytes += error == 0 ? frame_bytes : 0;
						unsigned index = summary.frames + summary.pixel_errors;
						if (error == 0 && summary.frames == 1) {
							summary.first_frame_ms = elapsed_milliseconds();
							printf("video_player_test: stage=first_frame elapsed_ms=%" PRIu64 "\n", summary.first_frame_ms);
						}
						if (error != 0 || index <= 3 || index % FRAME_LOG_EVERY == 0) {
							printf("video_player_test: stage=frame index=%u elapsed_ms=%" PRIu64
							    " pts=%" PRId64 "/%d pts_flags=0x%x format=0x%08x width=%zu height=%zu"
							    " pixel_error=%d bytes=%" PRIu64 " fnv1a64=%016" PRIx64 " min=%u max=%u\n",
							    index, elapsed_milliseconds(), pts.value, pts.timescale, (unsigned)pts.flags,
							    (unsigned)CVPixelBufferGetPixelFormatType(image), CVPixelBufferGetWidth(image),
							    CVPixelBufferGetHeight(image), error, frame_bytes, hash, low, high);
							summary.frame_logs++;
						}
						CVPixelBufferRelease(image);
						if (error != 0) { summary.failed = true; break; }
					} else {
						summary.empty++;
						if (summary.empty == 1 || summary.empty % FRAME_LOG_EVERY == 0) {
							printf("video_player_test: stage=empty count=%u elapsed_ms=%" PRIu64 "\n",
							    summary.empty, elapsed_milliseconds());
						}
					}
				}
				if (summary.ended) break;
				[NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.005]];
				publish_summary(&summary);
			}
		}
	} @catch (NSException *exception) {
		(void)exception;
		printf("video_player_test: stage=exception result=fail\n");
		summary.failed = true;
	}
	publish_summary(&summary);
	for (id observer in observers) [center removeObserver:observer];
	[player pause];
	if (output != nil) [item removeOutput:output];
	[player replaceCurrentItemWithPlayerItem:nil];
	summary.failed |= !summary.ready || !summary.playing || !summary.advanced || !summary.ended ||
	    summary.frames == 0 || summary.pixel_errors != 0;
	return complete_summary(&summary);
}

int
main(int argc, char **argv)
{
	double deadline = 150;
	const char *path = NULL;
	bool remote = false;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--deadline") && i + 1 < argc) {
			char *end = NULL;
			errno = 0;
			deadline = strtod(argv[++i], &end);
			if (errno || end == argv[i] || *end || !isfinite(deadline) || deadline <= 0 || deadline > 3600) goto usage;
		} else if (!strcmp(argv[i], "--url") && i + 1 < argc && path == NULL) {
			remote = true;
			path = argv[++i];
		} else if (argv[i][0] != '-' && path == NULL) {
			path = argv[i];
		} else goto usage;
	}
	if (path == NULL) goto usage;
	setvbuf(stdout, NULL, _IOLBF, 0);
	struct summary_snapshot initial = { .maximum = kCMTimeZero };
	publish_summary(&initial);
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
		return play(path, remote);
	}
usage:
	fprintf(stderr, "usage: video_player_test [--deadline SECONDS] FILE | --url HTTP_URL\n");
	return 2;
}
