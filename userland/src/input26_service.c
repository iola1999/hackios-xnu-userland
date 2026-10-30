/* iOS 26 input service. launchd owns restart after HID reset or transport failure.
 * Run one instance per system: /dev/redwood-touch has one shared input queue. */
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <errno.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>
#include "redwood_input.h"

static void say(const char *format, ...)
{
    char line[512];
    va_list args;
    va_start(args, format);
    int n = vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    if (n > 0) write(STDERR_FILENO, line, (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1);
}

_Static_assert(sizeof(struct redwood_touch) == 24, "redwood touch record size");
/* HID APIs and callback layout checked against the local 23G90 IOKit cache. */
typedef struct __IOHIDEvent *IOHIDEventRef;
typedef struct __IOHIDEventSystemClient *IOHIDEventSystemClientRef;
typedef struct __IOHIDServiceClient *IOHIDServiceClientRef;

/* IOHIDVirtualServiceClientCallbacksV2 */
struct virtual_service_callbacks {
	uint64_t version;               /* 2 */
	void (*notification)(void *target, void *refcon, IOHIDServiceClientRef service, uint32_t type, CFDictionaryRef property);
	Boolean (*setProperty)(void *target, void *refcon, IOHIDServiceClientRef service, CFStringRef key, CFTypeRef value);
	CFTypeRef (*copyProperty)(void *target, void *refcon, IOHIDServiceClientRef service, CFStringRef key);
	IOHIDEventRef (*copyEvent)(void *target, void *refcon, IOHIDServiceClientRef service, uint32_t type, IOHIDEventRef matching, uint32_t options);
	int (*setOutputEvent)(void *target, void *refcon, IOHIDServiceClientRef service, IOHIDEventRef event);
	CFTypeRef (*copyMatchingEvent)(void *target, void *refcon, IOHIDServiceClientRef service, CFDictionaryRef matching);
};

extern IOHIDEventSystemClientRef IOHIDEventSystemClientCreateWithType(CFAllocatorRef allocator, int type, CFDictionaryRef attributes);
extern void IOHIDEventSystemClientScheduleWithDispatchQueue(IOHIDEventSystemClientRef client, dispatch_queue_t queue);
extern IOHIDServiceClientRef IOHIDVirtualServiceClientCreateWithCallbacks(IOHIDEventSystemClientRef client, CFDictionaryRef attributes,
    const struct virtual_service_callbacks *callbacks, void *target, void *refcon);
extern Boolean IOHIDVirtualServiceClientDispatchEvent(IOHIDServiceClientRef service, IOHIDEventRef event);
/* 23G90 takes five doubles and 32-bit range/touch values. In the hand call,
 * touch and options occupy adjacent 32-bit stack slots; CF Boolean is one byte. */
extern IOHIDEventRef IOHIDEventCreateDigitizerEvent(CFAllocatorRef allocator, uint64_t time, uint32_t transducer, uint32_t index,
    uint32_t identity, uint32_t event_mask, uint32_t button_mask, double x, double y, double z, double tip_pressure,
    double twist, boolean_t range, boolean_t touch, uint32_t options);
extern IOHIDEventRef IOHIDEventCreateDigitizerFingerEvent(CFAllocatorRef allocator, uint64_t time, uint32_t index, uint32_t identity,
    uint32_t event_mask, double x, double y, double z, double tip_pressure, double twist, boolean_t range, boolean_t touch,
    uint32_t options);
extern IOHIDEventRef IOHIDEventCreateKeyboardEvent(CFAllocatorRef allocator, uint64_t time, uint32_t usage_page, uint32_t usage,
    Boolean down, uint32_t options);
extern void IOHIDEventAppendEvent(IOHIDEventRef parent, IOHIDEventRef child, uint32_t options);
extern void IOHIDEventSetIntegerValue(IOHIDEventRef event, uint32_t field, CFIndex value);

#define HID_CLIENT_SIMPLE               4       /* a client that only sends */
#define HID_PAGE_DIGITIZER              0x0d
#define HID_USAGE_TOUCH_SCREEN          0x04
#define HID_PAGE_CONSUMER               0x0c
#define HID_USAGE_CONSUMER_CONTROL      0x01
#define HID_USAGE_POWER                 0x30    /* the lock button */
#define HID_USAGE_MENU                  0x40    /* the home button */
#define HID_USAGE_VOLUME_UP             0xe9
#define HID_USAGE_VOLUME_DOWN           0xea
#define DIGITIZER_HAND                  3       /* kIOHIDDigitizerTransducerTypeHand */
#define DIGITIZER_RANGE                 0x1     /* event mask: what has changed */
#define DIGITIZER_TOUCH                 0x2
#define DIGITIZER_POSITION              0x4
#define DIGITIZER_FROM_EDGE_TIP         0x800   /* not a change: this finger came in over an edge of the screen */
#define FIELD_IS_BUILT_IN               0x4
#define FIELD_DIGITIZER_DISPLAY_INTEGRATED 0xb0019

#define MAX_FINGERS     10

/* Screen-edge gestures require the edge flag. Infer it from the initial
 * contact position because the input record has no contact-shape data. */
#define EDGE_SIDE       (16.0 / 375)
#define EDGE_TOP        (16.0 / 812)
#define EDGE_BOTTOM     (34.0 / 812)    /* the strip below the applications' content, where the home bar is */

static struct {
	bool touching, edge;
	double x, y;
} fingers[MAX_FINGERS];

static CFNumberRef
number(int value)
{
	return CFNumberCreate(NULL, kCFNumberIntType, &value);
}

/* Callback state remains allocated until process exit. */
struct service {
	CFMutableDictionaryRef properties;
	_Atomic bool opened;
};

static CFTypeRef
service_copy_property(__unused void *target, void *refcon, __unused IOHIDServiceClientRef service, CFStringRef key)
{
    CFTypeRef value = key ? CFDictionaryGetValue(((struct service *)refcon)->properties, key) : NULL;
    return value ? CFRetain(value) : NULL;
}

/*
 * What the window server last asked of the touch screen ("TouchDetectionMode"), in the low
 * byte: 1 = report touches; TOUCH_MODE_WAKE while the display is off and a tap is to wake it
 * -- which Apple's multitouch controller then does on its own; 0xff = nothing at
 * all. (Seen: 0xff and 0xfe during the boot, 0x1 with the display on, 0x1fe after the lock
 * button.)
 */
#define TOUCH_MODE_WAKE         0xfe
static _Atomic int touch_mode;

static Boolean
service_set_property(__unused void *target, void *refcon, __unused IOHIDServiceClientRef service, CFStringRef key,
    CFTypeRef value)
{
    if (key && value && CFEqual(key, CFSTR("DeviceOpenedByEventSystem")) &&
        CFGetTypeID(value) == CFBooleanGetTypeID() && CFBooleanGetValue(value)) {
        ((struct service *)refcon)->opened = true;
        say("input26: DeviceOpenedByEventSystem service=%p\n", (void *)service);
    }
    if (key && value && CFEqual(key, CFSTR("TouchDetectionMode")) && CFGetTypeID(value) == CFNumberGetTypeID()) {
        int mode = 0;
        CFNumberGetValue(value, kCFNumberIntType, &mode);
        if (mode != touch_mode) say("input26: touch detection mode %#x\n", mode);
        touch_mode = mode;
    }
    return true;
}

static void
service_notification(__unused void *target, __unused void *refcon, __unused IOHIDServiceClientRef service,
    uint32_t type, __unused CFDictionaryRef property)
{
    if (type == 5) {
        /* 23G90 sends reset while holding its client mutex. Exit directly;
         * Create/Remove/Dispatch here would reenter that mutex. */
        static const char message[] = "input26: HID service reset; exiting for restart\n";
        write(STDERR_FILENO, message, sizeof(message) - 1);
        _exit(1);
    }
}

static const struct virtual_service_callbacks service_callbacks = {
	.version = 2, .notification = service_notification, .setProperty = service_set_property,
	.copyProperty = service_copy_property,
};

/* Each service gets one registration attempt and its own callback state.
 * On failure the process exits, so pending callbacks retain valid refcon data. */
static IOHIDServiceClientRef
make_service(IOHIDEventSystemClientRef client, const char *product, int page, int usage, bool display)
{
	struct service *made = calloc(1, sizeof(*made));
	IOHIDServiceClientRef service = NULL;
	CFMutableDictionaryRef properties = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks);
	CFMutableDictionaryRef pair = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks);
	CFStringRef name = CFStringCreateWithCString(NULL, product, kCFStringEncodingUTF8);
	CFNumberRef page_number = number(page), usage_number = number(usage), zero = number(0);
	CFArrayRef pairs;

    if (!made || !properties || !pair || !name || !page_number || !usage_number || !zero) {
        say("input26: cannot allocate service properties for %s\n", product);
        return NULL;
    }
	CFDictionarySetValue(pair, CFSTR("DeviceUsagePage"), page_number);
	CFDictionarySetValue(pair, CFSTR("DeviceUsage"), usage_number);
	pairs = CFArrayCreate(NULL, (const void **)&pair, 1, &kCFTypeArrayCallBacks);
    if (!pairs) return NULL;
	CFDictionarySetValue(properties, CFSTR("PrimaryUsagePage"), page_number);
	CFDictionarySetValue(properties, CFSTR("PrimaryUsage"), usage_number);
	CFDictionarySetValue(properties, CFSTR("DeviceUsagePage"), page_number);
	CFDictionarySetValue(properties, CFSTR("DeviceUsage"), usage_number);
	CFDictionarySetValue(properties, CFSTR("DeviceUsagePairs"), pairs);
	CFDictionarySetValue(properties, CFSTR("Built-In"), kCFBooleanTrue);
	CFDictionarySetValue(properties, CFSTR("Transport"), CFSTR("SPI"));
	CFDictionarySetValue(properties, CFSTR("Manufacturer"), CFSTR("redwood"));
	CFDictionarySetValue(properties, CFSTR("Product"), name);
	CFDictionarySetValue(properties, CFSTR("VendorID"), zero);
	CFDictionarySetValue(properties, CFSTR("ProductID"), zero);
	if (display) {
		CFDictionarySetValue(properties, CFSTR("DisplayIntegrated"), kCFBooleanTrue);
	}
    made->properties = properties;
    CFRelease(pair);
    CFRelease(pairs);
    CFRelease(name);
    CFRelease(page_number);
    CFRelease(usage_number);
    CFRelease(zero);
    service = IOHIDVirtualServiceClientCreateWithCallbacks(client, properties, &service_callbacks, NULL, made);
    if (service == NULL) {
        say("input26: registration failed for %s\n", product);
        return NULL;
    }
    for (int tenths = 0; tenths < 20 && !made->opened; tenths++) {
        usleep(100000);
    }
    if (!made->opened) {
        say("input26: event server did not open %s after 20 polls\n", product);
        return NULL;
    }
    return service;
}

/* touch_lat=1 samples at most 128 dispatched touch records. CPU values are
 * cumulative times of this input thread, not HID callbacks or backboardd.
 * Query wall times are measured separately; CPU deltas still include portions
 * of the adjacent queries. The next sample reports the previous logging cost. */
static bool touch_latency;
static unsigned latency_samples;
static thread_t latency_thread;
static mach_timebase_info_data_t timebase;
static uint64_t latency_log_previous;

static uint64_t
micros(uint64_t absolute)
{
    return absolute * timebase.numer / timebase.denom / 1000;
}

struct touch_cpu_sample {
    int64_t user_us, system_us;
    uint64_t begin, end;
    kern_return_t result;
};

struct touch_timing {
    struct touch_cpu_sample cpu[3];
    uint64_t dispatch_begin, dispatch_end;
};

static void
touch_cpu_snapshot(struct touch_cpu_sample *sample)
{
    thread_basic_info_data_t info;
    mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
    sample->begin = mach_absolute_time();
    sample->result = thread_info(latency_thread, THREAD_BASIC_INFO, (thread_info_t)&info, &count);
    sample->end = mach_absolute_time();
    if (sample->result == KERN_SUCCESS && count < THREAD_BASIC_INFO_COUNT)
        sample->result = KERN_FAILURE;
    sample->user_us = sample->system_us = -1;
    if (sample->result == KERN_SUCCESS) {
        sample->user_us = (int64_t)info.user_time.seconds * 1000000 + info.user_time.microseconds;
        sample->system_us = (int64_t)info.system_time.seconds * 1000000 + info.system_time.microseconds;
    }
}

static Boolean
dispatch_input_event(IOHIDServiceClientRef service, IOHIDEventRef event, struct touch_timing *timing)
{
    static unsigned sent;
    if (timing) {
        touch_cpu_snapshot(&timing->cpu[1]);
        timing->dispatch_begin = mach_absolute_time();
    }
    Boolean result = IOHIDVirtualServiceClientDispatchEvent(service, event);
    if (timing) {
        timing->dispatch_end = mach_absolute_time();
        touch_cpu_snapshot(&timing->cpu[2]);
    }
    if (sent++ < 4 || !result) {
        say("input26: dispatch #%u MIG_success=%u\n", sent, result);
    }
    if (!result && !timing) _exit(1);
    return result;
}

/* One frame of the touch screen: the hand, and under it every finger that is down or has just lifted. */
static void
send_touch(IOHIDServiceClientRef screen, const struct redwood_touch *t, uint64_t read_at)
{
	uint32_t change = t->type == REDWOOD_TOUCH_MOVE ? DIGITIZER_POSITION : DIGITIZER_RANGE | DIGITIZER_TOUCH;
	IOHIDEventRef hand, finger;
	bool any = false;
	unsigned f = t->finger;
    struct touch_timing timing, *sample = NULL;

	if (f >= MAX_FINGERS || (t->type != REDWOOD_TOUCH_DOWN && !fingers[f].touching)) {
		return;
	}
    if (touch_latency && latency_samples < 128) {
        latency_samples++;
        sample = &timing;
        touch_cpu_snapshot(&sample->cpu[0]);
    }
	fingers[f].touching = t->type != REDWOOD_TOUCH_UP;
	fingers[f].x = t->x / 65535.0;
	fingers[f].y = t->y / 65535.0;
	if (t->type == REDWOOD_TOUCH_DOWN) {
		fingers[f].edge = fingers[f].x < EDGE_SIDE || fingers[f].x > 1 - EDGE_SIDE || fingers[f].y < EDGE_TOP ||
		    fingers[f].y > 1 - EDGE_BOTTOM;
	}
	for (unsigned i = 0; i < MAX_FINGERS; i++) {
		any |= fingers[i].touching;
	}

	hand = IOHIDEventCreateDigitizerEvent(NULL, t->time, DIGITIZER_HAND, 0, 0, change & ~DIGITIZER_RANGE, 0, fingers[f].x,
	    fingers[f].y, 0, 0, 0, any, any, 0);
	if (!hand) _exit(1);
	IOHIDEventSetIntegerValue(hand, FIELD_DIGITIZER_DISPLAY_INTEGRATED, 1);
	IOHIDEventSetIntegerValue(hand, FIELD_IS_BUILT_IN, 1);
	for (unsigned i = 0; i < MAX_FINGERS; i++) {
		if (!fingers[i].touching && i != f) {
			continue;
		}
		finger = IOHIDEventCreateDigitizerFingerEvent(NULL, t->time, i + 1, i + 2,
		    (i == f ? change : 0) | (fingers[i].edge ? DIGITIZER_FROM_EDGE_TIP : 0), fingers[i].x, fingers[i].y, 0,
		    fingers[i].touching ? 1.0 : 0.0, 0, fingers[i].touching, fingers[i].touching, 0);
		if (!finger) _exit(1);
		IOHIDEventSetIntegerValue(finger, FIELD_DIGITIZER_DISPLAY_INTEGRATED, 1);
		IOHIDEventAppendEvent(hand, finger, 0);
		CFRelease(finger);
	}
    Boolean dispatched = dispatch_input_event(screen, hand, sample);
	CFRelease(hand);
    if (sample) {
        bool build_valid = sample->cpu[0].result == KERN_SUCCESS && sample->cpu[1].result == KERN_SUCCESS;
        bool dispatch_valid = sample->cpu[1].result == KERN_SUCCESS && sample->cpu[2].result == KERN_SUCCESS;
        uint64_t query0 = sample->cpu[0].end - sample->cpu[0].begin;
        uint64_t query1 = sample->cpu[1].end - sample->cpu[1].begin;
        uint64_t log_begin = mach_absolute_time();
        /* Keep the tl:i format; sent is the dispatch return, before the last
         * CPU query, event release and diagnostic output. */
        say("tl: i %u posted %llu read %llu sent %llu\n", t->type, micros(t->time), micros(read_at),
            micros(sample->dispatch_end));
        say("tl: split n %u type %u ok %u build_us %llu dispatch_us %llu total_us %llu pre_us %llu "
            "cpu_build_us %lld/%lld cpu_dispatch_us %lld/%lld query_us %llu/%llu/%llu "
            "kr %d/%d/%d log_prev_us %llu\n",
            latency_samples, t->type, dispatched, micros(sample->dispatch_begin - read_at - query0 - query1),
            micros(sample->dispatch_end - sample->dispatch_begin), micros(sample->dispatch_end - read_at),
            micros(sample->cpu[0].begin - read_at),
            build_valid ? sample->cpu[1].user_us - sample->cpu[0].user_us : -1,
            build_valid ? sample->cpu[1].system_us - sample->cpu[0].system_us : -1,
            dispatch_valid ? sample->cpu[2].user_us - sample->cpu[1].user_us : -1,
            dispatch_valid ? sample->cpu[2].system_us - sample->cpu[1].system_us : -1,
            micros(query0), micros(query1),
            micros(sample->cpu[2].end - sample->cpu[2].begin),
            sample->cpu[0].result, sample->cpu[1].result, sample->cpu[2].result,
            micros(latency_log_previous));
        latency_log_previous = mach_absolute_time() - log_begin;
    }
    if (!dispatched) _exit(1);
}

static void
send_button(IOHIDServiceClientRef buttons, const struct redwood_touch *t)
{
	uint32_t usage = t->finger == REDWOOD_BUTTON_LOCK ? HID_USAGE_POWER : t->finger == REDWOOD_BUTTON_HOME ? HID_USAGE_MENU :
	    t->finger == REDWOOD_BUTTON_VOLUME_UP ? HID_USAGE_VOLUME_UP : t->finger == REDWOOD_BUTTON_VOLUME_DOWN ?
	    HID_USAGE_VOLUME_DOWN : 0;
	IOHIDEventRef event;

	if (usage == 0) {
		return;
	}
	event = IOHIDEventCreateKeyboardEvent(NULL, t->time, HID_PAGE_CONSUMER, usage, t->type == REDWOOD_BUTTON_DOWN, 0);
	if (!event) _exit(1);
	IOHIDEventSetIntegerValue(event, FIELD_IS_BUILT_IN, 1);
	dispatch_input_event(buttons, event, NULL);
	CFRelease(event);
}

static int
input_run(void)
{
	IOHIDEventSystemClientRef client = NULL;
	IOHIDServiceClientRef screen = NULL, buttons = NULL;
	struct redwood_touch t;
	unsigned count = 0, waking = 0;         /* waking: the fingers whose touch went into waking the display */
	int device;

	pthread_setname_np("redwood input");
    int qos_set = pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    qos_class_t thread_qos = QOS_CLASS_UNSPECIFIED;
    int thread_relative = 0;
    int qos_get = pthread_get_qos_class_np(pthread_self(), &thread_qos, &thread_relative);
    say("input26: qos thread requested=%u set=%d get=%d class=%u relative=%d\n",
        (unsigned)QOS_CLASS_USER_INTERACTIVE, qos_set, qos_get,
        (unsigned)thread_qos, thread_relative);
	{
		char arguments[1024] = "";
		size_t size = sizeof(arguments) - 1;

		if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) == 0) {
			touch_latency = strstr(arguments, "touch_lat=1") != NULL;
		}
		mach_timebase_info(&timebase);
	}
    if (touch_latency) {
        uint64_t began = mach_absolute_time();
        latency_thread = mach_thread_self();
        uint64_t ended = mach_absolute_time();
        say("tl: init thread_self_us %llu sample_limit 128\n", micros(ended - began));
    }
    sleep(3);                       /* allow the HID event server to start */
    client = IOHIDEventSystemClientCreateWithType(NULL, HID_CLIENT_SIMPLE, NULL);
    if (!client) {
        say("input26: cannot create HID client\n");
        return 1;
    }
    dispatch_queue_t queue = dispatch_queue_create("redwood.input", NULL);
    if (!queue) return 1;
    IOHIDEventSystemClientScheduleWithDispatchQueue(client, queue);
    screen = make_service(client, "Redwood touch screen", HID_PAGE_DIGITIZER, HID_USAGE_TOUCH_SCREEN, true);
    if (!screen) return 1;
    buttons = make_service(client, "Redwood buttons", HID_PAGE_CONSUMER, HID_USAGE_CONSUMER_CONTROL, false);
    if (!buttons) return 1;
    say("input26: both services opened; reading %s\n", REDWOOD_TOUCH_DEVICE);
    device = open(REDWOOD_TOUCH_DEVICE, O_RDONLY);
    if (device < 0) {
        say("input26: %s: %s\n", REDWOOD_TOUCH_DEVICE, strerror(errno));
        return 1;
    }

    for (;;) {
        ssize_t bytes = read(device, &t, sizeof(t));
        if (bytes < 0 && errno == EINTR) continue;
        if (bytes != (ssize_t)sizeof(t)) {
            if (bytes < 0) say("input26: read: %s\n", strerror(errno));
            else say("input26: short read %zd, expected %zu\n", bytes, sizeof(t));
            close(device);
            return 1;
        }
		uint64_t read_at = mach_absolute_time();

		if (count++ < 4) {
			say("redwood: input: sample type %u finger %u at %u %u\n", t.type, t.finger, t.x, t.y);
		}
		if (t.type > REDWOOD_TOUCH_UP) {
			send_button(buttons, &t);
		} else if (t.type == REDWOOD_TOUCH_DOWN && (touch_mode & 0xff) == TOUCH_MODE_WAKE) {
			/*
			 * The display is off and a tap is to wake it. The lock button is what wakes it here:
			 * press it for the finger, and keep the rest of this touch to ourselves.
			 */
			struct redwood_touch press = { .time = t.time, .type = REDWOOD_BUTTON_DOWN, .finger = REDWOOD_BUTTON_LOCK };

			send_button(buttons, &press);
			press.type = REDWOOD_BUTTON_UP;
			send_button(buttons, &press);
			waking |= 1u << (t.finger & 31);
		} else if (waking & (1u << (t.finger & 31))) {
			if (t.type == REDWOOD_TOUCH_UP) {
				waking &= ~(1u << (t.finger & 31));
			}
		} else {
			send_touch(screen, &t, read_at);
		}
	}
}

_Static_assert(sizeof(struct virtual_service_callbacks) == 0x38, "23G90 callbacks v2 size");
_Static_assert(offsetof(struct virtual_service_callbacks, setProperty) == 0x10, "setProperty offset");
_Static_assert(offsetof(struct virtual_service_callbacks, copyProperty) == 0x18, "copyProperty offset");
_Static_assert(offsetof(struct virtual_service_callbacks, copyMatchingEvent) == 0x30, "copyMatchingEvent offset");

int main(void)
{
    say("input26: standalone pid=%d device=%s callbacks=2\n", getpid(), REDWOOD_TOUCH_DEVICE);
    return input_run();
}
