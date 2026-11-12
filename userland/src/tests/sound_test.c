/*
 * sound_test: make iOS play something, the way a program would.
 *
 * usage: sound_test [--deadline SECONDS] STEP...
 *   --deadline SECONDS                 whole-process limit (0 < SECONDS <= 3600), including cleanup; exit 124
 *                                      on expiry, 2 for invalid arguments, 1 for a playback/API failure
 *   wait SECONDS
 *   springboard                        wait until SpringBoard is up and has said that the ringer is on (at
 *                                      most ten minutes): until then system sounds are "played" in silence,
 *                                      as on an iPhone with its switch on silent
 *   tone SECONDS [LEFT_HZ RIGHT_HZ]    a sine wave in each channel (default 1000 and 440 Hz) at half of
 *                                      full scale, through an audio queue: what a music player does
 *   level PEAK                         the tones after this one at PEAK of full scale (0 to 1) instead
 *                                      (before iOS's volume, which scales the samples itself)
 *   raw SECONDS                        the same tone written straight to /dev/annibale-audio, past iOS's own
 *                                      processing and volume (which keep a tone more than 12 dB under full
 *                                      scale even at the top): to try what the kernel does with full scale
 *   system ID                          a system sound by its number (1100 is the lock sound,
 *                                      1104 a keyboard click), as SpringBoard plays them
 *   file PATH                          a sound file, played as a system sound
 *   activate                           activate an AVAudioSession with the playback category
 *   route                              where iOS says its sound goes: the audio session's outputs and volume,
 *                                      and the devices of the system's output context
 *   haptic [SECONDS]                   haptics the way an application (and UIKit's feedback generators) plays
 *                                      them, through CoreHaptics: a tap, then a vibration of SECONDS (default
 *                                      0.3, at most 3: the kernel cuts a vibration at 1 s) at half strength, then
 *                                      three taps of falling strength
 *   restart                            kill the audio server (launchd starts it again when it is next asked for):
 *                                      what every program then sees as "media services were reset"
 *
 * The samples come out of /dev/annibale-audio (see sys/annibale_audio.h); a tone is something one
 * can recognize there without ears: two peaks in the spectrum, where they were put.
 */
#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <mach/mach_time.h>
#include <math.h>
#include <notify.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <signal.h>
#include <stdatomic.h>
#include <sys/time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define RATE            48000
#define BUFFER_FRAMES   4096

static double frequency[2] = { 1000, 440 }, peak = 0.5;
static mach_timebase_info_data_t timebase;

static uint64_t
monotonic_us(void)
{
	return (uint64_t)((__uint128_t)mach_absolute_time() * timebase.numer / timebase.denom / 1000);
}

static void
phase(const char *name, const char *event, int status)
{
	printf("sound_test: phase=%s event=%s us=%llu status=%d\n", name, event, monotonic_us(), status);
}

static void
deadline_expired(__unused int signal_number)
{
	static const char message[] = "sound_test: phase=deadline result=fail exit=124\n";
	write(STDERR_FILENO, message, sizeof(message) - 1);
	_exit(124);
}

struct tone_state {
	uint64_t frame, wanted;
	atomic_uint_fast64_t accepted;
	atomic_int enqueue_error;
	atomic_bool stopping;
};

static void
fill(void *context, AudioQueueRef queue, AudioQueueBufferRef buffer)
{
	struct tone_state *state = context;
	int16_t *samples = buffer->mAudioData;
	uint32_t count = 0;
	if (atomic_load(&state->stopping) || atomic_load(&state->enqueue_error)) {
		return;
	}
	for (; count < BUFFER_FRAMES && state->frame < state->wanted; count++, state->frame++) {
		/* A hundredth of a second at each edge prevents a discontinuity. */
		double edge = state->frame < RATE / 100 ? state->frame / (RATE / 100.0) :
		    state->wanted - state->frame < RATE / 100 ? (state->wanted - state->frame) / (RATE / 100.0) : 1;
		for (int channel = 0; channel < 2; channel++) {
			samples[2 * count + channel] = (int16_t)(32767 * peak * edge * sin(2 * M_PI * frequency[channel] * state->frame / RATE));
		}
	}
	if (count == 0) {
		return;
	}
	buffer->mAudioDataByteSize = count * 4;
	phase("queue-enqueue", "begin", 0);
	OSStatus status = AudioQueueEnqueueBuffer(queue, buffer, 0, NULL);
	if (status) {
		atomic_store(&state->enqueue_error, status);
	} else {
		atomic_fetch_add(&state->accepted, count);
	}
	printf("sound_test: enqueue us=%llu status=%d frames=%u accepted=%llu\n", monotonic_us(),
	    (int)status, count, (unsigned long long)atomic_load(&state->accepted));
}

/* The tone to the kernel's device itself, 10 ms at a time and as fast as it plays (48000 frames a second). */
static int
raw(double seconds)
{
	int device = open("/dev/annibale-audio", O_WRONLY);
	int16_t block[2 * 480];
	uint64_t total = (uint64_t)(seconds * RATE), written = 0, start;
	mach_timebase_info_data_t timebase;

	if (device < 0) {
		printf("sound_test: /dev/annibale-audio: %s\n", strerror(errno));
		return 1;
	}
	mach_timebase_info(&timebase);
	printf("sound_test: a tone of %.0f and %.0f Hz at %.2f of full scale for %.1f s, straight to /dev/annibale-audio\n",
	    frequency[0], frequency[1], peak, seconds);
	fflush(stdout);
	start = mach_absolute_time();
	while (written < total) {
		uint64_t due;
		uint32_t count = total - written < 480 ? (uint32_t)(total - written) : 480;

		for (uint32_t f = 0; f < count; f++) {
			uint64_t n = written + f;
			double edge = n < RATE / 100 ? n / (RATE / 100.0) : total - n < RATE / 100 ? (total - n) / (RATE / 100.0) : 1;

			for (int channel = 0; channel < 2; channel++) {
				block[2 * f + channel] = (int16_t)(32767 * peak * edge * sin(2 * M_PI * frequency[channel] * n / RATE));
			}
		}
		if (write(device, block, count * 4) != (ssize_t)(count * 4)) {
			printf("sound_test: /dev/annibale-audio: a write failed: %s\n", strerror(errno));
			break;
		}
		written += count;
		/* stay 100 ms ahead of the clock, not more: the kernel's ring holds 0.68 s */
		due = start + (written > RATE / 10 ? written - RATE / 10 : 0) * 1000000000ull / RATE * timebase.denom / timebase.numer;
		if (mach_absolute_time() < due) {
			mach_wait_until(due);
		}
	}
	close(device);
	printf("sound_test: %llu frames written straight to the device\n", written);
	return written < total;
}

static int
tone(double seconds)
{
	AudioStreamBasicDescription format = {
		.mSampleRate = RATE, .mFormatID = kAudioFormatLinearPCM,
		.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked,
		.mBytesPerPacket = 4, .mFramesPerPacket = 1, .mBytesPerFrame = 4, .mChannelsPerFrame = 2, .mBitsPerChannel = 16,
	};
	if (!isfinite(seconds) || seconds <= 0 || seconds >= (double)UINT64_MAX / RATE) {
		return 1;
	}
	struct tone_state state = { .wanted = (uint64_t)(seconds * RATE) };
	AudioQueueRef queue = NULL;
	OSStatus status;
	bool stopped = false;
	double until;

	/* The callback owns frame after Start; accepted and errors are shared with this thread. */
	phase("queue-create", "begin", 0);
	status = AudioQueueNewOutput(&format, fill, &state, NULL, NULL, 0, &queue);
	phase("queue-create", "end", status);
	if (status != 0) {
		return 1;
	}
	for (int i = 0; i < 3 && state.frame < state.wanted; i++) {
		AudioQueueBufferRef buffer;
		phase("queue-allocate", "begin", 0);
		status = AudioQueueAllocateBuffer(queue, BUFFER_FRAMES * 4, &buffer);
		phase("queue-allocate", "end", status);
		if (status != 0) {
			goto dispose;
		}
		fill(&state, queue, buffer);
		if ((status = atomic_load(&state.enqueue_error)) != 0) {
			goto dispose;
		}
	}
	phase("queue-start", "begin", 0);
	status = AudioQueueStart(queue, NULL);
	phase("queue-start", "end", status);
	printf("sound_test: tone hz=%.0f/%.0f peak=%.2f seconds=%.3f wanted=%llu\n",
	    frequency[0], frequency[1], peak, seconds, state.wanted);
	if (status != 0) {
		goto dispose;
	}
	until = monotonic_us() / 1000000.0 + 3 * seconds + 10;
	while (atomic_load(&state.accepted) < state.wanted && !atomic_load(&state.enqueue_error) && monotonic_us() / 1000000.0 < until) {
		usleep(10000);
	}
	status = atomic_load(&state.enqueue_error);
	if (status != 0 || atomic_load(&state.accepted) != state.wanted) {
		goto dispose;
	}
	phase("queue-stop", "begin", 0);
	status = AudioQueueStop(queue, false);
	phase("queue-stop", "end", status);
	while (status == 0 && monotonic_us() / 1000000.0 < until) {
		UInt32 running = 1, size = sizeof(running);
		status = AudioQueueGetProperty(queue, kAudioQueueProperty_IsRunning, &running, &size);
		if (status != 0 || size != sizeof(running)) {
			phase("queue-running", "end", status ? status : -1);
			break;
		}
		if (!running) {
			stopped = true;
			phase("queue-stopped", "end", 0);
			break;
		}
		usleep(10000);
	}
dispose:
	atomic_store(&state.stopping, true);
	phase("queue-dispose", "begin", 0);
	OSStatus disposed = AudioQueueDispose(queue, true);
	phase("queue-dispose", "end", disposed);
	printf("sound_test: tone-complete accepted=%llu wanted=%llu stopped=%d status=%d dispose=%d\n",
	    (unsigned long long)atomic_load(&state.accepted), state.wanted, stopped, (int)status, (int)disposed);
	/* A failed Dispose does not establish that callbacks have released this stack context. */
	if (disposed != 0) exit(1);
	return status != 0 || !stopped || atomic_load(&state.accepted) != state.wanted;
}

static void
played(SystemSoundID sound, __unused void *context)
{
	printf("sound_test: system sound %u has been played\n", (unsigned)sound);
	CFRunLoopStop(CFRunLoopGetCurrent());
}

static int
system_sound(SystemSoundID sound)
{
	AudioServicesAddSystemSoundCompletion(sound, CFRunLoopGetCurrent(), kCFRunLoopCommonModes, played, NULL);
	printf("sound_test: system sound %u\n", (unsigned)sound);
	AudioServicesPlaySystemSound(sound);
	if (CFRunLoopRunInMode(kCFRunLoopDefaultMode, 15, false) == kCFRunLoopRunTimedOut) {
		printf("sound_test: system sound %u: no word of its end in 15 s\n", (unsigned)sound);
		return 1;
	}
	return 0;
}

/* SpringBoard keeps the state of the ringer switch (1: ring, 0: silent) in a notification of this name. */
static void
wait_for_ringer(void)
{
	uint64_t state = 0;
	int token, seconds;

	if (notify_register_check("com.apple.springboard.ringerstate", &token) != NOTIFY_STATUS_OK) {
		return;
	}
	for (seconds = 0; seconds < 600 && (notify_get_state(token, &state) != NOTIFY_STATUS_OK || state == 0); seconds++) {
		sleep(1);
	}
	printf("sound_test: the ringer is %s after %d s\n", state ? "on" : "still off", seconds);
	notify_cancel(token);
}

/* [object selector] for methods without arguments that return an object. */
static id
send(id object, const char *selector)
{
	return object ? ((id (*)(id, SEL))objc_msgSend)(object, sel_registerName(selector)) : NULL;
}

static const char *
text(id string)
{
	const char *utf8 = string ? ((const char *(*)(id, SEL))objc_msgSend)(string, sel_registerName("UTF8String")) : NULL;

	return utf8 ? utf8 : "(none)";
}

/* Explicit session activation separates session errors from AudioQueue startup errors. */
static int
activate_playback(void)
{
	id session = send((id)objc_getClass("AVAudioSession"), "sharedInstance");
	id *category = dlsym(RTLD_DEFAULT, "AVAudioSessionCategoryPlayback");
	id error = NULL;
	if (session == NULL || category == NULL) {
		printf("sound_test: session: playback API unavailable\n");
		return 1;
	}
	phase("session-category", "begin", 0);
	BOOL ok = ((BOOL (*)(id, SEL, id, id *))objc_msgSend)(session,
	    sel_registerName("setCategory:error:"), *category, &error);
	phase("session-category", "end", ok ? 0 : 1);
	printf("sound_test: session: category=%d error=%ld\n", ok,
	    error ? ((long (*)(id, SEL))objc_msgSend)(error, sel_registerName("code")) : 0);
	if (!ok) {
		return 1;
	}
	error = NULL;
	phase("session-active", "begin", 0);
	ok = ((BOOL (*)(id, SEL, BOOL, id *))objc_msgSend)(session,
	    sel_registerName("setActive:error:"), YES, &error);
	phase("session-active", "end", ok ? 0 : 1);
	printf("sound_test: session: active=%d error=%ld\n", ok,
	    error ? ((long (*)(id, SEL))objc_msgSend)(error, sel_registerName("code")) : 0);
	return !ok;
}

/*
 * What the audio session of this program says of the route (AVAudioSession, as an application would
 * ask), and what the system's output context holds (AVOutputContext: what Control Center goes by).
 */
static void
route(void)
{
	id session = send((id)objc_getClass("AVAudioSession"), "sharedInstance");
	id outputs = send(send(session, "currentRoute"), "outputs");
	id context = send((id)objc_getClass("AVOutputContext"), "sharedSystemAudioContext");
	id devices = send(context, "outputDevices");
	unsigned long count = outputs ? ((unsigned long (*)(id, SEL))objc_msgSend)(outputs, sel_registerName("count")) : 0;

	printf("sound_test: route: %lu output%s, volume %.2f\n", count, count == 1 ? "" : "s",
	    session ? ((float (*)(id, SEL))objc_msgSend)(session, sel_registerName("outputVolume")) : -1.0f);
	for (unsigned long i = 0; i < count; i++) {
		id port = ((id (*)(id, SEL, unsigned long))objc_msgSend)(outputs, sel_registerName("objectAtIndex:"), i);

		printf("sound_test: route:   %s \"%s\" (%s)\n", text(send(port, "portType")), text(send(port, "portName")),
		    text(send(port, "UID")));
	}
	count = devices ? ((unsigned long (*)(id, SEL))objc_msgSend)(devices, sel_registerName("count")) : 0;
	printf("sound_test: route: the system's output context %s, %lu device%s\n", context ? "is there" : "is missing", count,
	    count == 1 ? "" : "s");
	for (unsigned long i = 0; i < count; i++) {
		id device = ((id (*)(id, SEL, unsigned long))objc_msgSend)(devices, sel_registerName("objectAtIndex:"), i);

		printf("sound_test: route:   \"%s\" (%s)\n", text(send(device, "name")), text(send(device, "ID")));
	}
}

/* CoreHaptics' constants (NSString *) by name, the framework opened when first asked for */
static id
haptics_constant(const char *name)
{
	static void *framework;
	id *constant;

	if (framework == NULL) {
		framework = dlopen("/System/Library/Frameworks/CoreHaptics.framework/CoreHaptics", RTLD_NOW);
	}
	constant = framework ? dlsym(framework, name) : NULL;
	return constant ? *constant : NULL;
}

static id
haptic_parameter(const char *name, float value)
{
	id parameter = send((id)objc_getClass("CHHapticEventParameter"), "alloc");

	return ((id (*)(id, SEL, id, float))objc_msgSend)(parameter, sel_registerName("initWithParameterID:value:"),
	           haptics_constant(name), value);
}

/* A haptic event: transient (a tap) when `duration' is 0, otherwise continuous for that long. */
static id
haptic_event(double at, double duration, float intensity, float sharpness)
{
	const void *parameters[2] = {
		haptic_parameter("CHHapticEventParameterIDHapticIntensity", intensity),
		haptic_parameter("CHHapticEventParameterIDHapticSharpness", sharpness),
	};
	CFArrayRef list = CFArrayCreate(NULL, parameters, 2, &kCFTypeArrayCallBacks);
	id event = send((id)objc_getClass("CHHapticEvent"), "alloc");

	if (duration > 0) {
		event = ((id (*)(id, SEL, id, id, double, double))objc_msgSend)(event,
		    sel_registerName("initWithEventType:parameters:relativeTime:duration:"),
		    haptics_constant("CHHapticEventTypeHapticContinuous"), (id)list, at, duration);
	} else {
		event = ((id (*)(id, SEL, id, id, double))objc_msgSend)(event, sel_registerName("initWithEventType:parameters:relativeTime:"),
		    haptics_constant("CHHapticEventTypeHapticTransient"), (id)list, at);
	}
	CFRelease(list);
	return event;
}

static int
haptic(double seconds)
{
	id error = NULL, engine, pattern, player;
	const void *events[5];
	CFArrayRef list, none = CFArrayCreate(NULL, NULL, 0, &kCFTypeArrayCallBacks);
	BOOL started;

	if (haptics_constant("CHHapticEventTypeHapticTransient") == NULL) {
		printf("sound_test: haptic: no CoreHaptics\n");
		return 1;
	}
	{
		/* what an application asks first, and what UIKit's feedback generators go by */
		id capabilities = send((id)objc_getClass("CHHapticEngine"), "capabilitiesForHardware");
		SEL first = sel_registerName("supports1stPartyHaptics");
		BOOL answers = capabilities && ((BOOL (*)(id, SEL, SEL))objc_msgSend)(capabilities,
		    sel_registerName("respondsToSelector:"), first);

		printf("sound_test: haptic: the hardware %s haptics%s\n", capabilities && ((BOOL (*)(id, SEL))objc_msgSend)(capabilities,
		    sel_registerName("supportsHaptics")) ? "supports" : "DOES NOT SUPPORT", !answers ? "" :
		    ((BOOL (*)(id, SEL))objc_msgSend)(capabilities, first) ? " (also the system's own, as UIKit's feedbacks)" :
		    " (but NOT the system's own, as UIKit's feedbacks)");
	}
	engine = ((id (*)(id, SEL, id *))objc_msgSend)(send((id)objc_getClass("CHHapticEngine"), "alloc"),
	    sel_registerName("initAndReturnError:"), &error);
	if (engine == NULL) {
		printf("sound_test: haptic: no engine: %s\n", text(send(error, "description")));
		return 1;
	}
	started = ((BOOL (*)(id, SEL, id *))objc_msgSend)(engine, sel_registerName("startAndReturnError:"), &error);
	printf("sound_test: haptic: the engine %s%s\n", started ? "has started" : "DOES NOT START: ",
	    started ? "" : text(send(error, "description")));
	if (!started) {
		return 1;
	}
	events[0] = haptic_event(0, 0, 1.0f, 0.5f);
	events[1] = haptic_event(0.3, seconds, 0.5f, 0.3f);
	events[2] = haptic_event(0.6 + seconds, 0, 1.0f, 0.8f);
	events[3] = haptic_event(0.8 + seconds, 0, 0.6f, 0.8f);
	events[4] = haptic_event(1.0 + seconds, 0, 0.3f, 0.8f);
	list = CFArrayCreate(NULL, events, 5, &kCFTypeArrayCallBacks);
	pattern = ((id (*)(id, SEL, id, id, id *))objc_msgSend)(send((id)objc_getClass("CHHapticPattern"), "alloc"),
	    sel_registerName("initWithEvents:parameters:error:"), (id)list, (id)none, &error);
	player = pattern ? ((id (*)(id, SEL, id, id *))objc_msgSend)(engine, sel_registerName("createPlayerWithPattern:error:"),
	    pattern, &error) : NULL;
	if (player == NULL ||
	    !((BOOL (*)(id, SEL, double, id *))objc_msgSend)(player, sel_registerName("startAtTime:error:"), 0.0, &error)) {
		printf("sound_test: haptic: the pattern does not play: %s\n", text(send(error, "description")));
		return 1;
	}
	printf("sound_test: haptic: playing a tap, %.2f s of vibration at half strength, three taps\n", seconds);
	usleep((useconds_t)((1.5 + seconds) * 1e6));
	((void (*)(id, SEL, id))objc_msgSend)(engine, sel_registerName("stopWithCompletionHandler:"), NULL);
	printf("sound_test: haptic: done\n");
	return 0;
}

extern int proc_listallpids(void *buffer, int size);
extern int proc_name(int pid, void *buffer, uint32_t size);

static void
restart(void)
{
	static int pids[2048];
	int count = proc_listallpids(pids, sizeof(pids));
	char name[64];
	int killed = 0;

	for (int i = 0; i < count; i++) {
		if (proc_name(pids[i], name, sizeof(name)) > 0 &&
		    (strcmp(name, "audiomxd") == 0 || strcmp(name, "mediaserverd") == 0)) {
			printf("sound_test: restart: killing %s[%d]: %s\n", name, pids[i],
			    kill(pids[i], SIGKILL) == 0 ? "done" : strerror(errno));
			killed++;
		}
	}
	if (killed == 0) {
		printf("sound_test: restart: neither audiomxd nor mediaserverd is running\n");
	}
}

int
main(int argc, char **argv)
{
	int failed = 0;

	setvbuf(stdout, NULL, _IONBF, 0);
	mach_timebase_info(&timebase);
	printf("sound_test: pid=%d start_us=%llu\n", getpid(), monotonic_us());
	int first = 1;
	if (argc > 1 && strcmp(argv[1], "--deadline") == 0) {
		char *end;
		if (argc < 4) return 2;
		errno = 0;
		double seconds = strtod(argv[2], &end);
		if (errno || end == argv[2] || *end || !isfinite(seconds) || seconds <= 0 || seconds > 3600) return 2;
		struct sigaction action = { .sa_handler = deadline_expired };
		sigemptyset(&action.sa_mask);
		sigset_t alarm_mask;
		sigemptyset(&alarm_mask);
		sigaddset(&alarm_mask, SIGALRM);
		struct itimerval timer = { .it_value = {
			.tv_sec = (time_t)seconds,
			.tv_usec = (suseconds_t)((seconds - floor(seconds)) * 1000000),
		} };
		if (!timer.it_value.tv_sec && !timer.it_value.tv_usec) return 2;
		if (sigaction(SIGALRM, &action, NULL) || sigprocmask(SIG_UNBLOCK, &alarm_mask, NULL) ||
		    setitimer(ITIMER_REAL, &timer, NULL)) {
			printf("sound_test: deadline-setup errno=%d\n", errno);
			return 1;
		}
		first = 3;
	}
	for (int i = first; i < argc; i++) {
		const char *step = argv[i];
		phase(step, "begin", 0);
		if (strcmp(argv[i], "wait") == 0 && i + 1 < argc) {
			usleep((useconds_t)(atof(argv[++i]) * 1e6));
		} else if (strcmp(argv[i], "springboard") == 0) {
			wait_for_ringer();
		} else if (strcmp(argv[i], "route") == 0) {
			route();
		} else if (strcmp(argv[i], "activate") == 0) {
			if (activate_playback()) {
				phase(step, "end", 1);
				return 1;
			}
		} else if (strcmp(argv[i], "restart") == 0) {
			restart();
		} else if (strcmp(argv[i], "haptic") == 0) {
			double seconds = i + 1 < argc && atof(argv[i + 1]) > 0 ? atof(argv[++i]) : 0.3;

			failed |= haptic(seconds > 3 ? 3 : seconds);
		} else if (strcmp(argv[i], "tone") == 0 && i + 1 < argc) {
			double seconds = atof(argv[++i]);

			if (i + 2 < argc && atof(argv[i + 1]) > 0 && atof(argv[i + 2]) > 0) {
				frequency[0] = atof(argv[++i]);
				frequency[1] = atof(argv[++i]);
			}
			failed |= tone(seconds);
		} else if (strcmp(argv[i], "raw") == 0 && i + 1 < argc) {
			failed |= raw(atof(argv[++i]));
		} else if (strcmp(argv[i], "level") == 0 && i + 1 < argc) {
			peak = atof(argv[++i]);
			peak = peak < 0 ? 0 : peak > 1 ? 1 : peak;
			printf("sound_test: the tones from now on at %.2f of full scale\n", peak);
		} else if (strcmp(argv[i], "system") == 0 && i + 1 < argc) {
			failed |= system_sound((SystemSoundID)atoi(argv[++i]));
		} else if (strcmp(argv[i], "file") == 0 && i + 1 < argc) {
			CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)argv[i + 1],
			    (CFIndex)strlen(argv[i + 1]), false);
			SystemSoundID sound = 0;
			OSStatus status = AudioServicesCreateSystemSoundID(url, &sound);

			i++;
			if (status != 0) {
				printf("sound_test: %s: AudioServicesCreateSystemSoundID: %d\n", argv[i], (int)status);
				failed = 1;
			} else {
				failed |= system_sound(sound);
			}
		} else {
			printf("sound_test: what is \"%s\"?\n", argv[i]);
			return 2;
		}
		phase(step, "end", failed);
	}
	return failed;
}
