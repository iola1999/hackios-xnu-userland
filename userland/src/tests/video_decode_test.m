/*
 * Decode a local video's compressed samples through AVAssetReader and VideoToolbox.
 * Usage: video_decode_test [--deadline SECONDS] [--frames COUNT]
 *                         [--require-hardware | --software] [--report-file PATH] FILE
 * Defaults: 12 seconds, 60 frames, hardware allowed. COUNT is limited to 10000.
 * Exit: 0 decoded every submitted frame; 1 input/decode/output failure; 2 arguments;
 *       124 whole-process deadline, including asset loading, draining and cleanup.
 * --require-hardware also requires the session's actual hardware property to be true.
 * The first video track is read; compressed buffers must contain one frame each.
 * Empty timing markers do not count as frames. A reset of an active decoder fails explicitly.
 * Pixel hashes cover active NV12 bytes, excluding row padding. Paths, error descriptions
 * and media bytes are omitted from logs. This probe does not present frames or play audio.
 * --report-file exclusively creates a file with every callback and a completion record.
 */
#import <AVFoundation/AVFoundation.h>
#import <VideoToolbox/VideoToolbox.h>
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

struct options {
	double deadline;
	unsigned frames;
	bool require_hardware, software;
	const char *path, *report_path;
};

struct decode_state {
	pthread_mutex_t lock;
	unsigned callbacks, frames, errors, dropped, empty, pixel_errors;
	uint64_t bytes;
	FILE *report;
};

static void
deadline_expired(int signal_number)
{
	static const char message[] = "video_decode_test: stage=deadline result=fail exit=124\n";
	(void)signal_number;
	(void)write(STDERR_FILENO, message, sizeof(message) - 1);
	_exit(124);
}

static void
report_error(const char *stage, NSError *error)
{
	const char *domain = "other";
	if ([error.domain isEqualToString:AVFoundationErrorDomain]) domain = "avfoundation";
	else if ([error.domain isEqualToString:NSOSStatusErrorDomain]) domain = "osstatus";
	else if ([error.domain isEqualToString:NSPOSIXErrorDomain]) domain = "posix";
	else if ([error.domain isEqualToString:NSCocoaErrorDomain]) domain = "cocoa";
	printf("video_decode_test: stage=%s error_domain=%s error=%ld\n", stage, domain, (long)error.code);
	NSError *underlying = error.userInfo[NSUnderlyingErrorKey];
	if ([underlying isKindOfClass:NSError.class]) {
		printf("video_decode_test: stage=%s underlying_error=%ld\n", stage, (long)underlying.code);
	}
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

static void
decoded_frame(void *context, void *source, OSStatus status, VTDecodeInfoFlags flags,
    CVImageBufferRef image, CMTime pts, CMTime duration)
{
	struct decode_state *state = context;
	uint64_t hash = 0, bytes = 0;
	unsigned low = 0, high = 0;
	int pixel_error = 0;
	if (status == noErr && image != NULL) {
		pixel_error = pixel_summary(image, &hash, &bytes, &low, &high);
	}
	pthread_mutex_lock(&state->lock);
	state->callbacks++;
	state->errors += status != noErr;
	state->dropped += (flags & kVTDecodeInfo_FrameDropped) != 0;
	state->empty += image == NULL;
	state->pixel_errors += pixel_error != 0;
	if (status == noErr && image != NULL && pixel_error == 0) {
		state->frames++;
		state->bytes += bytes;
	}
	FILE *outputs[] = { stdout, state->report };
	for (unsigned index = 0; index < 2; index++) {
		if (outputs[index] == NULL) continue;
		fprintf(outputs[index], "video_decode_test: stage=callback sample=%" PRIuPTR " status=%d flags=0x%x"
		    " pts=%" PRId64 "/%d pts_flags=0x%x duration=%" PRId64 "/%d"
		    " format=0x%08x width=%zu height=%zu pixel_error=%d bytes=%" PRIu64
		    " fnv1a64=%016" PRIx64 " min=%u max=%u\n",
		    (uintptr_t)source, (int)status, (unsigned)flags, pts.value, pts.timescale,
		    (unsigned)pts.flags, duration.value, duration.timescale,
		    image ? (unsigned)CVPixelBufferGetPixelFormatType(image) : 0,
		    image ? CVPixelBufferGetWidth(image) : 0, image ? CVPixelBufferGetHeight(image) : 0,
		    pixel_error, bytes, hash, low, high);
	}
	pthread_mutex_unlock(&state->lock);
}

static int
hardware_property(VTDecompressionSessionRef session, const char *phase)
{
	CFTypeRef value = NULL;
	OSStatus status = VTSessionCopyProperty(session,
	    kVTDecompressionPropertyKey_UsingHardwareAcceleratedVideoDecoder, NULL, &value);
	int hardware = -1;
	if (status == noErr && value != NULL && CFGetTypeID(value) == CFBooleanGetTypeID()) {
		hardware = CFBooleanGetValue(value) ? 1 : 0;
	}
	printf("video_decode_test: stage=hardware phase=%s status=%d hardware=%d\n", phase, (int)status, hardware);
	if (value != NULL) CFRelease(value);
	return hardware;
}

static int
decode(const struct options *options)
{
	struct decode_state state = { .lock = PTHREAD_MUTEX_INITIALIZER };
	VTDecompressionSessionRef session = NULL;
	AVAssetReader *reader = nil;
	unsigned submitted = 0, accepted = 0, markers = 0;
	bool failed = false, limit_reached = false;
	int hardware = -1;
	if (options->report_path != NULL) {
		state.report = fopen(options->report_path, "wx");
		if (state.report == NULL) {
			printf("video_decode_test: stage=report_open errno=%d\n", errno);
			pthread_mutex_destroy(&state.lock);
			return 1;
		}
	}
	@try {
		do {
			struct stat st;
			if (stat(options->path, &st) != 0) {
				printf("video_decode_test: stage=input errno=%d\n", errno);
				failed = true;
				break;
			}
			if (!S_ISREG(st.st_mode)) {
				printf("video_decode_test: stage=input error=not_regular_file\n");
				failed = true;
				break;
			}
			printf("video_decode_test: stage=input_ready bytes=%lld\n", (long long)st.st_size);
			NSURL *url = [NSURL fileURLWithPath:@(options->path)];
			AVURLAsset *asset = [AVURLAsset URLAssetWithURL:url options:nil];
			dispatch_semaphore_t loaded = dispatch_semaphore_create(0);
			__block NSArray<AVAssetTrack *> *tracks = nil;
			__block NSError *load_error = nil;
			printf("video_decode_test: stage=tracks_begin\n");
			[asset loadTracksWithMediaType:AVMediaTypeVideo completionHandler:^(NSArray<AVAssetTrack *> *value, NSError *error) {
				tracks = value;
				load_error = error;
				dispatch_semaphore_signal(loaded);
			}];
			dispatch_semaphore_wait(loaded, DISPATCH_TIME_FOREVER);
			printf("video_decode_test: stage=tracks_ready count=%lu\n", (unsigned long)tracks.count);
			if (load_error != nil || tracks.count == 0) {
				report_error("tracks", load_error);
				printf("video_decode_test: stage=tracks count=%lu\n", (unsigned long)tracks.count);
				failed = true;
				break;
			}
			NSError *error = nil;
			reader = [[AVAssetReader alloc] initWithAsset:asset error:&error];
			AVAssetReaderTrackOutput *output = [[AVAssetReaderTrackOutput alloc]
			    initWithTrack:tracks.firstObject outputSettings:nil];
			if (reader == nil || ![reader canAddOutput:output]) {
				report_error("reader_create", error);
				failed = true;
				break;
			}
			[reader addOutput:output];
			printf("video_decode_test: stage=reader_begin\n");
			if (![reader startReading]) {
				report_error("reader_start", reader.error);
				failed = true;
				break;
			}
			printf("video_decode_test: stage=reader_ready\n");
			while (submitted < options->frames) {
				CMSampleBufferRef sample = [output copyNextSampleBuffer];
				if (sample == NULL) {
					if (reader.status != AVAssetReaderStatusCompleted) {
						report_error("reader_sample", reader.error);
						failed = true;
					}
					break;
				}
				CMFormatDescriptionRef format = CMSampleBufferGetFormatDescription(sample);
				CMItemCount count = CMSampleBufferGetNumSamples(sample);
				CFTypeRef reset = CMGetAttachment(sample, kCMSampleBufferAttachmentKey_ResetDecoderBeforeDecoding, NULL);
				if (session != NULL && reset != NULL && CFEqual(reset, kCFBooleanTrue)) {
					printf("video_decode_test: stage=sample error=decoder_reset_marker\n");
					CFRelease(sample);
					failed = true;
					break;
				}
				/* AVAssetReader emits zero-sample edit-boundary and timing buffers. */
				if (count == 0 && CMSampleBufferGetTotalSampleSize(sample) == 0) {
					CFTypeRef drain = CMGetAttachment(sample, kCMSampleBufferAttachmentKey_DrainAfterDecoding, NULL);
					OSStatus status = noErr;
					if (session != NULL && drain != NULL && CFEqual(drain, kCFBooleanTrue)) {
						status = VTDecompressionSessionWaitForAsynchronousFrames(session);
					}
					markers++;
					printf("video_decode_test: stage=marker count=%u drain_status=%d\n", markers, (int)status);
					CFRelease(sample);
					if (status != noErr) { failed = true; break; }
					continue;
				}
				if (count != 1 || format == NULL || CMFormatDescriptionGetMediaType(format) != kCMMediaType_Video ||
				    CMSampleBufferGetDataBuffer(sample) == NULL || CMSampleBufferGetTotalSampleSize(sample) == 0) {
					printf("video_decode_test: stage=sample error=invalid_compressed_sample count=%ld\n", (long)count);
					CFRelease(sample);
					failed = true;
					break;
				}
				if (session == NULL) {
					NSDictionary *specification = @{
						(__bridge NSString *)kVTVideoDecoderSpecification_EnableHardwareAcceleratedVideoDecoder: @(!options->software),
						(__bridge NSString *)kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder: @(options->require_hardware)
					};
					NSDictionary *pixels = @{
						(__bridge NSString *)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange)
					};
					VTDecompressionOutputCallbackRecord callback = { decoded_frame, &state };
					printf("video_decode_test: stage=create_begin\n");
					OSStatus status = VTDecompressionSessionCreate(NULL, format, (__bridge CFDictionaryRef)specification,
					    (__bridge CFDictionaryRef)pixels, &callback, &session);
					CMVideoDimensions dimensions = CMVideoFormatDescriptionGetDimensions(format);
					printf("video_decode_test: stage=create status=%d codec=0x%08x width=%d height=%d\n",
					    (int)status, (unsigned)CMFormatDescriptionGetMediaSubType(format), dimensions.width, dimensions.height);
					if (status != noErr || session == NULL) {
						CFRelease(sample);
						failed = true;
						break;
					}
					hardware = hardware_property(session, "create");
				} else if (!VTDecompressionSessionCanAcceptFormatDescription(session, format)) {
					printf("video_decode_test: stage=format error=session_cannot_accept\n");
					CFRelease(sample);
					failed = true;
					break;
				}
				VTDecodeInfoFlags flags = 0;
				CMTime pts = CMSampleBufferGetPresentationTimeStamp(sample);
				submitted++;
				OSStatus status = VTDecompressionSessionDecodeFrame(session, sample,
				    kVTDecodeFrame_EnableAsynchronousDecompression, (void *)(uintptr_t)submitted, &flags);
				printf("video_decode_test: stage=submit sample=%u status=%d flags=0x%x pts=%" PRId64 "/%d bytes=%zu\n",
				    submitted, (int)status, (unsigned)flags, pts.value, pts.timescale, CMSampleBufferGetTotalSampleSize(sample));
				CFRelease(sample);
				if (status != noErr || (flags & kVTDecodeInfo_FrameDropped) != 0) {
					failed = true;
					break;
				}
				accepted++;
			}
			limit_reached = submitted == options->frames;
		} while (false);
	} @catch (NSException *exception) {
		(void)exception;
		printf("video_decode_test: stage=exception result=fail\n");
		failed = true;
	}
	if (session != NULL) {
		/* This also emits delayed frames. The process deadline bounds this blocking API. */
		OSStatus status = VTDecompressionSessionWaitForAsynchronousFrames(session);
		printf("video_decode_test: stage=drain status=%d\n", (int)status);
		failed |= status != noErr;
		hardware = hardware_property(session, "drain");
		VTDecompressionSessionInvalidate(session);
		CFRelease(session);
	}
	NSInteger reader_status = reader.status;
	[reader cancelReading];
	failed |= options->require_hardware && hardware != 1;
	/* The software decoder may omit this property; EnableHardware=false still applies. */
	failed |= options->software && hardware == 1;
	failed |= accepted == 0 || accepted != submitted || state.callbacks != accepted || state.frames != accepted ||
	    state.errors != 0 || state.dropped != 0 || state.empty != 0 || state.pixel_errors != 0;
	if (state.report != NULL) {
		/* Keep the report open through the session's callback cleanup. */
		bool report_failed = ferror(state.report) != 0;
		if (fprintf(state.report, "video_decode_test: stage=report_complete callbacks=%u frames=%u\n",
		    state.callbacks, state.frames) < 0) report_failed = true;
		if (fclose(state.report) != 0) report_failed = true;
		if (report_failed) printf("video_decode_test: stage=report_write result=fail\n");
		failed |= report_failed;
	}
	printf("video_decode_test: stage=summary result=%s submitted=%u accepted=%u callbacks=%u frames=%u"
	    " callback_errors=%u dropped=%u empty=%u pixel_errors=%u hardware=%d require_hardware=%d software=%d"
	    " limit_reached=%d reader_status=%ld markers=%u output_bytes=%" PRIu64 "\n",
	    failed ? "fail" : "pass", submitted, accepted, state.callbacks, state.frames, state.errors, state.dropped,
	    state.empty, state.pixel_errors, hardware, options->require_hardware, options->software,
	    limit_reached, (long)reader_status, markers, state.bytes);
	pthread_mutex_destroy(&state.lock);
	return failed ? 1 : 0;
}

int
main(int argc, char **argv)
{
	struct options options = { .deadline = 12, .frames = 60 };
	for (int i = 1; i < argc; i++) {
		char *end = NULL;
		if (!strcmp(argv[i], "--deadline") && i + 1 < argc) {
			errno = 0;
			options.deadline = strtod(argv[++i], &end);
			if (errno || end == argv[i] || *end || !isfinite(options.deadline) ||
			    options.deadline <= 0 || options.deadline > 3600) goto usage;
		} else if (!strcmp(argv[i], "--frames") && i + 1 < argc) {
			errno = 0;
			unsigned long count = strtoul(argv[++i], &end, 10);
			if (errno || end == argv[i] || *end || count == 0 || count > 10000) goto usage;
			options.frames = (unsigned)count;
		} else if (!strcmp(argv[i], "--require-hardware")) {
			options.require_hardware = true;
		} else if (!strcmp(argv[i], "--software")) {
			options.software = true;
		} else if (!strcmp(argv[i], "--report-file") && i + 1 < argc && options.report_path == NULL) {
			options.report_path = argv[++i];
		} else if (argv[i][0] != '-' && options.path == NULL) {
			options.path = argv[i];
		} else goto usage;
	}
	if (options.path == NULL || (options.require_hardware && options.software)) goto usage;
	setvbuf(stdout, NULL, _IOLBF, 0);
	struct sigaction action = { .sa_handler = deadline_expired };
	sigemptyset(&action.sa_mask);
	sigset_t alarm_mask;
	sigemptyset(&alarm_mask);
	sigaddset(&alarm_mask, SIGALRM);
	struct itimerval timer = { .it_value = {
		.tv_sec = (time_t)options.deadline,
		.tv_usec = (suseconds_t)((options.deadline - floor(options.deadline)) * 1000000)
	} };
	if (timer.it_value.tv_sec == 0 && timer.it_value.tv_usec == 0) timer.it_value.tv_usec = 1;
	if (sigaction(SIGALRM, &action, NULL) != 0 || sigprocmask(SIG_UNBLOCK, &alarm_mask, NULL) != 0 ||
	    setitimer(ITIMER_REAL, &timer, NULL) != 0) {
		printf("video_decode_test: stage=deadline_setup errno=%d\n", errno);
		return 1;
	}
	@autoreleasepool {
		return decode(&options);
	}
usage:
	fprintf(stderr, "usage: video_decode_test [--deadline SECONDS] [--frames COUNT] [--require-hardware | --software] [--report-file PATH] FILE\n");
	return 2;
}
