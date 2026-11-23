/*
 * micloop_test: record from the microphone while playing a speaker tone, and say what came.
 *
 * usage: micloop_test SECONDS tone AT
 *
 * Same session and recorder as record_test (play-and-record, WAV to /tmp). At AT seconds a 1000 Hz
 * sine at most of full scale starts on the speaker through AudioQueue and lasts while the recording
 * does. The file is then checked at 1000 Hz before and during the tone: if the microphone really
 * hears the room, the second figure is much louder than the first.
 */
#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PATH            "/tmp/micloop_test.wav"
#define TONE_HZ         1000.0
#define TONE_PEAK       0.9
#define BUFFER_FRAMES   4096
#define RATE            48000

/* ---- speaker tone, played on its own thread ----------------------------------------------- */

struct tone_state {
	uint64_t frame, wanted;
	atomic_uint_fast64_t accepted;
	atomic_int enqueue_error;
	atomic_bool stopping;
};

static void
tone_fill(void *context, AudioQueueRef queue, AudioQueueBufferRef buffer)
{
	struct tone_state *state = context;
	int16_t *samples = buffer->mAudioData;
	uint32_t count = 0;

	if (atomic_load(&state->stopping) || atomic_load(&state->enqueue_error)) {
		return;
	}
	for (; count < BUFFER_FRAMES && state->frame < state->wanted; count++, state->frame++) {
		double edge = state->frame < RATE / 100 ? state->frame / (RATE / 100.0) :
		    state->wanted - state->frame < RATE / 100 ? (state->wanted - state->frame) / (RATE / 100.0) : 1;
		int16_t v = (int16_t)(32767 * TONE_PEAK * edge * sin(2 * M_PI * TONE_HZ * state->frame / RATE));

		samples[2 * count] = v;
		samples[2 * count + 1] = v;
	}
	if (count == 0) {
		return;
	}
	buffer->mAudioDataByteSize = count * 4;
	OSStatus status = AudioQueueEnqueueBuffer(queue, buffer, 0, NULL);

	if (status) {
		atomic_store(&state->enqueue_error, status);
	} else {
		atomic_fetch_add(&state->accepted, count);
	}
}

static void *
tone_thread(void *arg)
{
	double seconds = *(double *)arg;
	AudioStreamBasicDescription format = {
		.mSampleRate = RATE, .mFormatID = kAudioFormatLinearPCM,
		.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked,
		.mBytesPerPacket = 4, .mFramesPerPacket = 1, .mBytesPerFrame = 4, .mChannelsPerFrame = 2, .mBitsPerChannel = 16,
	};
	struct tone_state state = { .wanted = (uint64_t)(seconds * RATE) };
	AudioQueueRef queue = NULL;
	OSStatus status;

	status = AudioQueueNewOutput(&format, tone_fill, &state, NULL, NULL, 0, &queue);
	if (status != 0) {
		printf("record_test: tone: AudioQueueNewOutput failed %d\n", (int)status);
		return NULL;
	}
	for (int i = 0; i < 3 && state.frame < state.wanted; i++) {
		AudioQueueBufferRef buffer;

		status = AudioQueueAllocateBuffer(queue, BUFFER_FRAMES * 4, &buffer);
		if (status != 0) {
			printf("record_test: tone: AudioQueueAllocateBuffer failed %d\n", (int)status);
			goto out;
		}
		tone_fill(&state, queue, buffer);
	}
	status = AudioQueueStart(queue, NULL);
	printf("record_test: tone at %.0f Hz started on the speaker (status %d)\n", TONE_HZ, (int)status);
	while (atomic_load(&state.accepted) < state.wanted && !atomic_load(&state.enqueue_error)) {
		usleep(10000);
	}
	AudioQueueStop(queue, true);
out:
	AudioQueueDispose(queue, true);
	return NULL;
}

static pthread_t tone_tid;
static double tone_seconds;

static void
tone_start(double seconds)
{
	tone_seconds = seconds;
	if (pthread_create(&tone_tid, NULL, tone_thread, &tone_seconds) != 0) {
		printf("record_test: tone thread: %s\n", strerror(errno));
		return;
	}
	pthread_detach(tone_tid);
}

static id
send(id object, const char *selector)
{
	return object ? ((id (*)(id, SEL))objc_msgSend)(object, sel_registerName(selector)) : NULL;
}

static const char *
text(id object)
{
	id description = object ? send(object, "description") : NULL;
	const char *utf8 = description ? ((const char *(*)(id, SEL))objc_msgSend)(description, sel_registerName("UTF8String")) : NULL;

	return utf8 ? utf8 : "(none)";
}

static unsigned long
count_of(id array)
{
	return array ? ((unsigned long (*)(id, SEL))objc_msgSend)(array, sel_registerName("count")) : 0;
}

static id
item(id array, unsigned long i)
{
	return ((id (*)(id, SEL, unsigned long))objc_msgSend)(array, sel_registerName("objectAtIndex:"), i);
}

/* a constant (NSString *) of a framework, by name */
static id
constant(const char *framework_path, const char *name)
{
	void *framework = dlopen(framework_path, RTLD_NOW);
	id *value = framework ? dlsym(framework, name) : NULL;

	return value ? *value : NULL;
}

#define AVFAUDIO        "/System/Library/Frameworks/AVFAudio.framework/AVFAudio"
#define COREHAPTICS     "/System/Library/Frameworks/CoreHaptics.framework/CoreHaptics"

static void
ports(const char *what, id list)
{
	unsigned long n = count_of(list);

	printf("record_test: %s: %lu\n", what, n);
	for (unsigned long i = 0; i < n; i++) {
		id port = item(list, i);

		printf("record_test:   %s \"%s\" (%s)\n", text(send(port, "portType")), text(send(port, "portName")),
		    text(send(port, "UID")));
	}
}

/* The audio session, set up for recording; false if it will not be. */
static bool
session_up(void)
{
	id session = send((id)objc_getClass("AVAudioSession"), "sharedInstance"), error = NULL;
	id category = constant(AVFAUDIO, "AVAudioSessionCategoryPlayAndRecord");
	BOOL ok;

	if (session == NULL || category == NULL) {
		printf("record_test: no AVAudioSession\n");
		return false;
	}
	if (((BOOL (*)(id, SEL, SEL))objc_msgSend)(session, sel_registerName("respondsToSelector:"),
	    sel_registerName("recordPermission"))) {
		unsigned long permission = ((unsigned long (*)(id, SEL))objc_msgSend)(session, sel_registerName("recordPermission"));

		printf("record_test: the permission to record: %c%c%c%c\n", (int)(permission >> 24 & 0xff), (int)(permission >> 16 & 0xff),
		    (int)(permission >> 8 & 0xff), (int)(permission & 0xff));
	}
	ok = ((BOOL (*)(id, SEL, id, id *))objc_msgSend)(session, sel_registerName("setCategory:error:"), category, &error);
	printf("record_test: the category play-and-record: %s%s\n", ok ? "set" : "NOT SET: ", ok ? "" : text(error));
	if (!ok) {
		return false;
	}
	ok = ((BOOL (*)(id, SEL, BOOL, id *))objc_msgSend)(session, sel_registerName("setAllowHapticsAndSystemSoundsDuringRecording:error:"),
	    YES, &error);
	printf("record_test: haptics while recording: %s%s\n", ok ? "allowed" : "NOT ALLOWED: ", ok ? "" : text(error));
	ok = ((BOOL (*)(id, SEL, BOOL, id *))objc_msgSend)(session, sel_registerName("setActive:error:"), YES, &error);
	printf("record_test: the session: %s%s\n", ok ? "active" : "NOT ACTIVE: ", ok ? "" : text(error));
	if (!ok) {
		return false;
	}
	printf("record_test: input available: %s\n", ((BOOL (*)(id, SEL))objc_msgSend)(session,
	    sel_registerName("isInputAvailable")) ? "yes" : "NO");
	ports("available inputs", send(session, "availableInputs"));
	ports("the route's inputs", send(send(session, "currentRoute"), "inputs"));
	ports("the route's outputs", send(send(session, "currentRoute"), "outputs"));
	return true;
}

static CFNumberRef
number(int value)
{
	return CFNumberCreate(NULL, kCFNumberIntType, &value);
}

/* The power of one frequency in samples [from, to): Goertzel, as dB of full scale (a full-scale sine is 0). */
static double
tone_db(const int16_t *samples, long from, long to, double hz)
{
	double w = 2 * M_PI * hz / RATE, c = 2 * cos(w), s1 = 0, s2 = 0, power;
	long n = to - from;

	if (n <= 0) {
		return -200;
	}
	for (long i = from; i < to; i++) {
		double window = 0.5 - 0.5 * cos(2 * M_PI * (i - from) / (n - 1)), s0 = samples[i] / 32768.0 * window + c * s1 - s2;

		s2 = s1;
		s1 = s0;
	}
	power = s1 * s1 + s2 * s2 - c * s1 * s2;
	/* (a Hann window halves the amplitude: a full-scale sine gives (n/4)^2) */
	return 10 * log10(power / ((n / 4.0) * (n / 4.0)) + 1e-20);
}

static double
rms_db(const int16_t *samples, long from, long to)
{
	double sum = 0;

	for (long i = from; i < to; i++) {
		sum += (double)samples[i] * samples[i];
	}
	return to > from ? 10 * log10(sum / (to - from) / (32768.0 * 32768.0) + 1e-20) : -200;
}

/* The file read back: the samples of its data chunk. */
static int
analyse(double seconds, double vibrate_at)
{
	FILE *file = fopen(PATH, "rb");
	unsigned char header[12], chunk[8];
	int16_t *samples = NULL;
	long count = 0, peak = 0;

	if (file == NULL || fread(header, 1, 12, file) != 12 || memcmp(header, "RIFF", 4) != 0 || memcmp(header + 8, "WAVE", 4) != 0) {
		printf("record_test: %s is not a WAV file\n", PATH);
		return 1;
	}
	while (fread(chunk, 1, 8, file) == 8) {
		uint32_t size = chunk[4] | chunk[5] << 8 | chunk[6] << 16 | (uint32_t)chunk[7] << 24;

		if (memcmp(chunk, "data", 4) == 0) {
			samples = malloc(size + 2);
			count = (long)fread(samples, 1, size, file) / 2;
			break;
		}
		fseek(file, (long)(size + (size & 1)), SEEK_CUR);
	}
	fclose(file);
	if (samples == NULL || count == 0) {
		printf("record_test: %s has no samples\n", PATH);
		return 1;
	}
	for (long i = 0; i < count; i++) {
		long v = samples[i] < 0 ? -(long)samples[i] : samples[i];

		peak = v > peak ? v : peak;
	}
	printf("record_test: the file: %ld samples (%.2f s of %.2f), peak %ld of 32768, %.1f dBFS RMS over all\n", count,
	    (double)count / RATE, seconds, peak, rms_db(samples, 0, count));
	printf("record_test: dBFS RMS of each tenth of a second:");
	for (long at = 0; at + RATE / 10 <= count; at += RATE / 10) {
		printf(" %.0f", rms_db(samples, at, at + RATE / 10));
	}
	printf("\n");
	if (vibrate_at > 0) {
		long quiet_from = (long)(0.5 * RATE), quiet_to = (long)(vibrate_at * RATE) - RATE / 10;
		long loud_from = (long)((vibrate_at + 0.15) * RATE), loud_to = (long)(count - RATE / 10);

		loud_to = loud_to < count ? loud_to : count;
		printf("record_test: before the tone %.1f dBFS RMS, %.1f dBFS at %.0f Hz; while it lasts %.1f dBFS RMS, %.1f dBFS at %.0f "
		    "Hz\n", rms_db(samples, quiet_from, quiet_to), tone_db(samples, quiet_from, quiet_to, TONE_HZ), TONE_HZ,
		    rms_db(samples, loud_from, loud_to), tone_db(samples, loud_from, loud_to, TONE_HZ), TONE_HZ);
	}
	free(samples);
	return 0;
}

static int
record_raw(double seconds, double tone_at)
{
	const char *dev = "/dev/annibale-mic";
	int fd = open(dev, O_RDONLY);
	if (fd < 0) {
		dev = "/dev/annibale-audio";
		fd = open(dev, O_RDONLY);
	}
	if (fd < 0) {
		printf("record_test: raw: cannot open %s: %s\n", dev, strerror(errno));
		return 1;
	}
	long target = (long)(seconds * RATE);
	int16_t *samples = calloc((size_t)target, sizeof(int16_t));
	if (samples == NULL) {
		close(fd);
		return 1;
	}
	long got_samples = 0;
	bool toned = false;
	while (got_samples < target) {
		long chunk = target - got_samples;
		if (chunk > 960) {
			chunk = 960;
		}
		if (tone_at > 0 && !toned && got_samples >= (long)(tone_at * RATE)) {
			tone_start(seconds - tone_at);
			toned = true;
		}
		ssize_t n = read(fd, samples + got_samples, (size_t)chunk * sizeof(int16_t));
		if (n <= 0) {
			break;
		}
		got_samples += n / 2;
	}
	close(fd);
	long peak = 0;
	for (long i = 0; i < got_samples; i++) {
		long v = samples[i] < 0 ? -(long)samples[i] : samples[i];
		if (v > peak) {
			peak = v;
		}
	}
	printf("record_test: raw (%s): %ld samples (%.2f s of %.2f), peak %ld of 32768, %.1f dBFS RMS\n",
	    dev, got_samples, (double)got_samples / RATE, seconds, peak, rms_db(samples, 0, got_samples));
	if (tone_at > 0 && got_samples > (long)((tone_at + 0.5) * RATE)) {
		long quiet_from = (long)(0.3 * RATE), quiet_to = (long)(tone_at * RATE) - RATE / 10;
		long loud_from = (long)((tone_at + 0.2) * RATE), loud_to = got_samples - RATE / 10;

		printf("record_test: before the tone %.1f dBFS RMS, %.1f dBFS at %.0f Hz; while it lasts %.1f dBFS RMS, %.1f dBFS at %.0f Hz\n",
		    rms_db(samples, quiet_from, quiet_to), tone_db(samples, quiet_from, quiet_to, TONE_HZ), TONE_HZ,
		    rms_db(samples, loud_from, loud_to), tone_db(samples, loud_from, loud_to, TONE_HZ), TONE_HZ);
	}
	free(samples);
	return got_samples > 0 ? 0 : 1;
}

int
main(int argc, char **argv)
{
	double seconds = argc > 1 ? atof(argv[1]) : 5, vibrate_at = 0;
	const void *keys[6], *values[6];
	CFDictionaryRef settings;
	CFURLRef url;
	id recorder, error = NULL;
	BOOL ok, vibrated = NO;
	int tenths;

	setvbuf(stdout, NULL, _IONBF, 0);
	seconds = seconds < 1 ? 1 : seconds > 60 ? 60 : seconds;
	if (argc > 2 && strcmp(argv[2], "raw") == 0) {
		double tone_at = argc > 4 && strcmp(argv[3], "tone") == 0 ? atof(argv[4]) : 0;
		if (tone_at > 0 && !session_up()) {
			return 1;
		}
		return record_raw(seconds, tone_at);
	}
	if (argc > 3 && strcmp(argv[2], "tone") == 0) {
		vibrate_at = atof(argv[3]);
	}
	if (!session_up()) {
		return 1;
	}
	keys[0] = CFSTR("AVFormatIDKey");
	values[0] = number('lpcm');
	keys[1] = CFSTR("AVSampleRateKey");
	values[1] = number(RATE);
	keys[2] = CFSTR("AVNumberOfChannelsKey");
	values[2] = number(1);
	keys[3] = CFSTR("AVLinearPCMBitDepthKey");
	values[3] = number(16);
	keys[4] = CFSTR("AVLinearPCMIsFloatKey");
	values[4] = kCFBooleanFalse;
	keys[5] = CFSTR("AVLinearPCMIsBigEndianKey");
	values[5] = kCFBooleanFalse;
	settings = CFDictionaryCreate(NULL, keys, values, 6, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	unlink(PATH);
	url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)PATH, (CFIndex)strlen(PATH), false);
	recorder = ((id (*)(id, SEL, id, id, id *))objc_msgSend)(send((id)objc_getClass("AVAudioRecorder"), "alloc"),
	    sel_registerName("initWithURL:settings:error:"), (id)url, (id)settings, &error);
	if (recorder == NULL) {
		printf("record_test: no AVAudioRecorder: %s\n", text(error));
		return 1;
	}
	((void (*)(id, SEL, BOOL))objc_msgSend)(recorder, sel_registerName("setMeteringEnabled:"), YES);
	ok = ((BOOL (*)(id, SEL))objc_msgSend)(recorder, sel_registerName("prepareToRecord")) &&
	    ((BOOL (*)(id, SEL, double))objc_msgSend)(recorder, sel_registerName("recordForDuration:"), seconds);
	printf("record_test: recording %.1f s into %s%s: %s\n", seconds, PATH, vibrate_at > 0 ? ", the tone at" : "",
	    ok ? "started" : "DOES NOT START");
	if (!ok) {
		return 1;
	}
	printf("record_test: meters (average/peak dB) every half second:");
	for (tenths = 0; tenths < (int)(seconds * 10) + 5 && ((BOOL (*)(id, SEL))objc_msgSend)(recorder, sel_registerName("isRecording"));
	    tenths++) {
		if (vibrate_at > 0 && !vibrated && tenths >= (int)(vibrate_at * 10)) {
			printf("\n");
			tone_start(seconds - vibrate_at);
			vibrated = YES;
		}
		usleep(100000);
		send(recorder, "updateMeters");
		if (tenths % 5 == 4) {
			printf(" %.0f/%.0f", ((float (*)(id, SEL, unsigned long))objc_msgSend)(recorder, sel_registerName("averagePowerForChannel:"), 0),
			    ((float (*)(id, SEL, unsigned long))objc_msgSend)(recorder, sel_registerName("peakPowerForChannel:"), 0));
		}
	}
	printf("\n");
	send(recorder, "stop");
	usleep(300000);
	return analyse(seconds, vibrate_at);
}
