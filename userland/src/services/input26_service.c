/* iOS 26 input service. launchd owns restart after HID reset or transport failure.
 * Run one instance per system: /dev/annibale-touch has one shared input queue. */
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
#include "annibale_input.h"

static void say(const char *format, ...)
{
    char line[512];
    va_list args;
    va_start(args, format);
    int n = vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    if (n > 0) write(STDERR_FILENO, line, (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1);
}

_Static_assert(sizeof(struct annibale_touch) == 24, "annibale touch record size");
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
extern IOHIDEventRef IOHIDEventCreateAmbientLightSensorEvent(CFAllocatorRef allocator, uint64_t time, uint32_t level,
    uint32_t options);
extern IOHIDEventRef IOHIDEventCreateProximtyEvent(CFAllocatorRef allocator, uint64_t time, uint32_t detection_mask,
    uint32_t options);
extern void IOHIDEventAppendEvent(IOHIDEventRef parent, IOHIDEventRef child, uint32_t options);
extern void IOHIDEventSetIntegerValue(IOHIDEventRef event, uint32_t field, CFIndex value);

#define HID_CLIENT_SIMPLE               4       /* a client that only sends */
#define HID_PAGE_DIGITIZER              0x0d
#define HID_USAGE_TOUCH_SCREEN          0x04
#define HID_PAGE_CONSUMER               0x0c
#define HID_USAGE_CONSUMER_CONTROL      0x01
#define HID_PAGE_APPLE_VENDOR           0xff00  /* kHIDPage_AppleVendor */
#define HID_USAGE_APPLE_ALS             0x0004  /* kHIDUsage_AppleVendor_AmbientLightSensor */
#define HID_USAGE_APPLE_PROXIMITY       0x0005  /* kHIDUsage_AppleVendor_ProximitySensor */
#define HID_EVENT_TYPE_ALS              12      /* kIOHIDEventTypeAmbientLightSensor */
#define HID_EVENT_TYPE_PROXIMITY        14      /* kIOHIDEventTypeProximity */
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
#define FIELD_ALS_RAW_CH0               0xc0001
#define FIELD_ALS_RAW_CH1               0xc0002
#define FIELD_ALS_BRIGHTNESS_CHANGED    0xc0005
#define FIELD_ALS_ILLUMINANCE           0xc000a
#define PROXIMITY_MASK_NEAR             0x41

#define ANNIBALE_SENSORS_DEVICE          "/dev/annibale-sensors"

struct annibale_sensor_sample {
	uint64_t timestamp;
	uint32_t als_lux;
	uint32_t clear_raw;
	uint32_t ir_raw;
	uint32_t prox_raw;
	uint16_t prox_distance_mm;
	uint8_t  prox_near;
	uint8_t  source_flags;
};

_Static_assert(sizeof(struct annibale_sensor_sample) == 32, "annibale sensor sample size");

#define MAX_FINGERS     10

/* Screen-edge gestures require the edge flag. Infer it from the initial
 * contact position because the input record has no contact-shape data. */
#define EDGE_SIDE       (16.0 / 375)
#define EDGE_TOP        (16.0 / 812)
#define EDGE_BOTTOM     (34.0 / 812)    /* the strip below the applications' content, where the home bar is */

enum ann_finger_state {
	ANN_FINGER_IDLE = 0,
	ANN_FINGER_WAKE_SWALLOWED = 1,
	ANN_FINGER_ACTIVE_INNER = 2,
	ANN_FINGER_ACTIVE_EDGE = 3,
};

static struct {
	enum ann_finger_state state;
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
static _Atomic int prox_detection_mode;
static _Atomic uint32_t cached_als_lux = 150;
static _Atomic uint32_t cached_clear_raw = 600;
static _Atomic uint32_t cached_ir_raw = 120;
static _Atomic uint32_t cached_prox_near = 0;

static IOHIDEventRef
create_als_event(uint64_t ts, uint32_t lux, uint32_t clear_raw, uint32_t ir_raw)
{
	IOHIDEventRef event = IOHIDEventCreateAmbientLightSensorEvent(NULL, ts ? ts : mach_absolute_time(), lux, 0);
	if (!event) {
		return NULL;
	}
	IOHIDEventSetIntegerValue(event, FIELD_IS_BUILT_IN, 1);
	IOHIDEventSetIntegerValue(event, FIELD_ALS_RAW_CH0, (CFIndex)clear_raw);
	IOHIDEventSetIntegerValue(event, FIELD_ALS_RAW_CH1, (CFIndex)ir_raw);
	IOHIDEventSetIntegerValue(event, FIELD_ALS_BRIGHTNESS_CHANGED, 0);
	IOHIDEventSetIntegerValue(event, FIELD_ALS_ILLUMINANCE, (CFIndex)lux);
	return event;
}

static IOHIDEventRef
create_prox_event(uint64_t ts, bool near)
{
	IOHIDEventRef event = IOHIDEventCreateProximtyEvent(NULL, ts ? ts : mach_absolute_time(),
	    near ? PROXIMITY_MASK_NEAR : 0, 0);
	if (!event) {
		return NULL;
	}
	IOHIDEventSetIntegerValue(event, FIELD_IS_BUILT_IN, 1);
	return event;
}

static IOHIDEventRef
service_copy_event(__unused void *target, __unused void *refcon, __unused IOHIDServiceClientRef service,
    uint32_t type, __unused IOHIDEventRef matching, __unused uint32_t options)
{
	if (type == HID_EVENT_TYPE_ALS) {
		return create_als_event(mach_absolute_time(), cached_als_lux, cached_clear_raw, cached_ir_raw);
	}
	if (type == HID_EVENT_TYPE_PROXIMITY) {
		return create_prox_event(mach_absolute_time(), cached_prox_near != 0);
	}
	return NULL;
}

static Boolean
service_set_property(__unused void *target, void *refcon, __unused IOHIDServiceClientRef service, CFStringRef key,
    CFTypeRef value)
{
    struct service *srv = (struct service *)refcon;
    if (key && value && srv && srv->properties) {
        CFDictionarySetValue(srv->properties, key, value);
    }
    if (key && value && CFEqual(key, CFSTR("DeviceOpenedByEventSystem")) &&
        CFGetTypeID(value) == CFBooleanGetTypeID() && CFBooleanGetValue(value)) {
        srv->opened = true;
        say("input26: DeviceOpenedByEventSystem service=%p\n", (void *)service);
    }
    if (key && value && CFEqual(key, CFSTR("TouchDetectionMode")) && CFGetTypeID(value) == CFNumberGetTypeID()) {
        int mode = 0;
        CFNumberGetValue(value, kCFNumberIntType, &mode);
        if (mode != touch_mode) say("input26: touch detection mode %#x\n", mode);
        touch_mode = mode;
    }
    if (key && value && CFEqual(key, CFSTR("DetectionMode")) && CFGetTypeID(value) == CFNumberGetTypeID()) {
        int mode = 0;
        CFNumberGetValue(value, kCFNumberIntType, &mode);
        if (mode != prox_detection_mode) say("input26: proximity detection mode %#x\n", mode);
        prox_detection_mode = mode;
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
	.copyProperty = service_copy_property, .copyEvent = service_copy_event,
};

/* Each service gets one registration attempt and its own callback state.
 * On failure the process exits, so pending callbacks retain valid refcon data. */
static IOHIDServiceClientRef
make_service_ext(IOHIDEventSystemClientRef client, const char *product, int page, int usage, bool display, bool require_open)
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
	CFDictionarySetValue(properties, CFSTR("Manufacturer"), CFSTR("annibale"));
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
    int max_polls = require_open ? 20 : 5;
    for (int tenths = 0; tenths < max_polls && !made->opened; tenths++) {
        usleep(100000);
    }
    if (!made->opened && require_open) {
        say("input26: event server did not open %s after %d polls\n", product, max_polls);
        return NULL;
    }
    return service;
}

static IOHIDServiceClientRef
make_service(IOHIDEventSystemClientRef client, const char *product, int page, int usage, bool display)
{
    return make_service_ext(client, product, page, usage, display, true);
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
send_touch(IOHIDServiceClientRef screen, const struct annibale_touch *t, uint64_t read_at)
{
	uint32_t type = t->type;
	unsigned f = t->finger;

	if (f >= MAX_FINGERS) {
		return;
	}
	bool was_active = fingers[f].state >= ANN_FINGER_ACTIVE_INNER;
	bool was_edge = fingers[f].state == ANN_FINGER_ACTIVE_EDGE;
	if (!was_active) {
		if (type == ANNIBALE_TOUCH_MOVE) {
			type = ANNIBALE_TOUCH_DOWN;
		} else if (type != ANNIBALE_TOUCH_DOWN) {
			return;
		}
	}
	uint32_t change = type == ANNIBALE_TOUCH_MOVE ? DIGITIZER_POSITION : DIGITIZER_RANGE | DIGITIZER_TOUCH;
	IOHIDEventRef hand, finger;
	bool any = false;
    struct touch_timing timing, *sample = NULL;

    if (touch_latency && latency_samples < 128) {
        latency_samples++;
        sample = &timing;
        touch_cpu_snapshot(&sample->cpu[0]);
    }
	fingers[f].x = t->x / 65535.0;
	fingers[f].y = t->y / 65535.0;
	if (type == ANNIBALE_TOUCH_UP) {
		fingers[f].state = ANN_FINGER_IDLE;
	} else if (type == ANNIBALE_TOUCH_DOWN) {
		bool edge = fingers[f].x < EDGE_SIDE || fingers[f].x > 1 - EDGE_SIDE || fingers[f].y < EDGE_TOP ||
		    fingers[f].y > 1 - EDGE_BOTTOM;
		fingers[f].state = edge ? ANN_FINGER_ACTIVE_EDGE : ANN_FINGER_ACTIVE_INNER;
		was_edge = edge;
	}
	for (unsigned i = 0; i < MAX_FINGERS; i++) {
		any |= fingers[i].state >= ANN_FINGER_ACTIVE_INNER;
	}

	hand = IOHIDEventCreateDigitizerEvent(NULL, t->time, DIGITIZER_HAND, 0, 0, change & ~DIGITIZER_RANGE, 0, fingers[f].x,
	    fingers[f].y, 0, 0, 0, any, any, 0);
	if (!hand) _exit(1);
	IOHIDEventSetIntegerValue(hand, FIELD_DIGITIZER_DISPLAY_INTEGRATED, 1);
	IOHIDEventSetIntegerValue(hand, FIELD_IS_BUILT_IN, 1);
	for (unsigned i = 0; i < MAX_FINGERS; i++) {
		bool touching = fingers[i].state >= ANN_FINGER_ACTIVE_INNER;
		bool edge = i == f ? was_edge : fingers[i].state == ANN_FINGER_ACTIVE_EDGE;
		if (!touching && i != f) {
			continue;
		}
		finger = IOHIDEventCreateDigitizerFingerEvent(NULL, t->time, i + 1, i + 2,
		    (i == f ? change : 0) | (edge ? DIGITIZER_FROM_EDGE_TIP : 0), fingers[i].x, fingers[i].y, 0,
		    touching ? 1.0 : 0.0, 0, touching, touching, 0);
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
send_button(IOHIDServiceClientRef buttons, const struct annibale_touch *t)
{
	uint32_t usage = t->finger == ANNIBALE_BUTTON_LOCK ? HID_USAGE_POWER : t->finger == ANNIBALE_BUTTON_HOME ? HID_USAGE_MENU :
	    t->finger == ANNIBALE_BUTTON_VOLUME_UP ? HID_USAGE_VOLUME_UP : t->finger == ANNIBALE_BUTTON_VOLUME_DOWN ?
	    HID_USAGE_VOLUME_DOWN : 0;
	IOHIDEventRef event;

	if (usage == 0) {
		return;
	}
	event = IOHIDEventCreateKeyboardEvent(NULL, t->time, HID_PAGE_CONSUMER, usage, t->type == ANNIBALE_BUTTON_DOWN, 0);
	if (!event) _exit(1);
	IOHIDEventSetIntegerValue(event, FIELD_IS_BUILT_IN, 1);
	dispatch_input_event(buttons, event, NULL);
	CFRelease(event);
}

static void
start_sensors_poller(dispatch_queue_t queue, IOHIDServiceClientRef als_service, IOHIDServiceClientRef prox_service)
{
	if (!als_service && !prox_service) {
		return;
	}
	dispatch_source_t timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, queue);
	if (!timer) {
		return;
	}
	static int sensors_fd = -1;
	static uint32_t last_als_lux = UINT32_MAX;
	static uint32_t last_prox_near = UINT32_MAX;
	static int last_prox_mode = 0;

	dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, 250 * NSEC_PER_MSEC),
	    250 * NSEC_PER_MSEC, 25 * NSEC_PER_MSEC);
	dispatch_source_set_event_handler(timer, ^{
		struct annibale_sensor_sample sample = { 0 };
		bool have_sample = false;

		if (sensors_fd < 0) {
			sensors_fd = open(ANNIBALE_SENSORS_DEVICE, O_RDONLY | O_NONBLOCK);
		}
		if (sensors_fd >= 0) {
			ssize_t n = pread(sensors_fd, &sample, sizeof(sample), 0);
			if (n == (ssize_t)sizeof(sample)) {
				have_sample = true;
			}
		}
		if (!have_sample) {
			int lux_val = 150, prox_val = 0;
			size_t sz = sizeof(int);
			if (sysctlbyname("kern.annibale.als_lux", &lux_val, &sz, NULL, 0) == 0 && lux_val >= 0) {
				sample.als_lux = (uint32_t)lux_val;
				sample.clear_raw = sample.als_lux * 4;
				sample.ir_raw = sample.als_lux;
				have_sample = true;
			}
			sz = sizeof(int);
			if (sysctlbyname("kern.annibale.prox_near", &prox_val, &sz, NULL, 0) == 0) {
				sample.prox_near = prox_val ? 1 : 0;
				have_sample = true;
			}
			sample.timestamp = mach_absolute_time();
		}
		if (!have_sample) {
			return;
		}
		cached_als_lux = sample.als_lux;
		cached_clear_raw = sample.clear_raw;
		cached_ir_raw = sample.ir_raw;
		cached_prox_near = sample.prox_near;

		if (als_service && sample.als_lux != last_als_lux) {
			IOHIDEventRef ev = create_als_event(sample.timestamp, sample.als_lux,
			    sample.clear_raw, sample.ir_raw);
			if (ev) {
				if (IOHIDVirtualServiceClientDispatchEvent(als_service, ev)) {
					if (last_als_lux == UINT32_MAX) {
						say("input26: dispatched initial ALS lux=%u\n", sample.als_lux);
					}
					last_als_lux = sample.als_lux;
				}
				CFRelease(ev);
			}
		}
		int cur_prox_mode = prox_detection_mode;
		if (prox_service && (sample.prox_near != last_prox_near || (cur_prox_mode != 0 && last_prox_mode == 0))) {
			IOHIDEventRef ev = create_prox_event(sample.timestamp, sample.prox_near != 0);
			if (ev) {
				if (IOHIDVirtualServiceClientDispatchEvent(prox_service, ev)) {
					say("input26: dispatched proximity near=%u (mode=%#x)\n",
					    sample.prox_near, cur_prox_mode);
					last_prox_near = sample.prox_near;
				}
				CFRelease(ev);
			}
		}
		last_prox_mode = cur_prox_mode;
	});
	dispatch_resume(timer);
}

static int
input_run(void)
{
	IOHIDEventSystemClientRef client = NULL;
	IOHIDServiceClientRef screen = NULL, buttons = NULL, als_service = NULL, prox_service = NULL;
	struct annibale_touch t;
	unsigned count = 0;
	int device;

	pthread_setname_np("annibale input");
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
    dispatch_queue_t queue = dispatch_queue_create("annibale.input", NULL);
    if (!queue) return 1;
    IOHIDEventSystemClientScheduleWithDispatchQueue(client, queue);
    screen = make_service(client, "Annibale touch screen", HID_PAGE_DIGITIZER, HID_USAGE_TOUCH_SCREEN, true);
    if (!screen) return 1;
    buttons = make_service(client, "Annibale buttons", HID_PAGE_CONSUMER, HID_USAGE_CONSUMER_CONTROL, false);
    if (!buttons) return 1;
    als_service = make_service_ext(client, "Annibale ambient light sensor", HID_PAGE_APPLE_VENDOR, HID_USAGE_APPLE_ALS, true, false);
    prox_service = make_service_ext(client, "Annibale proximity sensor", HID_PAGE_APPLE_VENDOR, HID_USAGE_APPLE_PROXIMITY, false, false);
    start_sensors_poller(queue, als_service, prox_service);
    say("input26: services registered (als=%p prox=%p); reading %s\n", (void *)als_service, (void *)prox_service, ANNIBALE_TOUCH_DEVICE);
    device = open(ANNIBALE_TOUCH_DEVICE, O_RDONLY);
    if (device < 0) {
        say("input26: %s: %s\n", ANNIBALE_TOUCH_DEVICE, strerror(errno));
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
			say("annibale: input: sample type %u finger %u at %u %u\n", t.type, t.finger, t.x, t.y);
		}
		if (t.type > ANNIBALE_TOUCH_UP) {
			send_button(buttons, &t);
			continue;
		}
		if (t.finger >= MAX_FINGERS) {
			continue;
		}
		int mode_low = touch_mode & 0xff;
		if (mode_low == TOUCH_MODE_WAKE || mode_low == 0xff) {
			for (unsigned i = 0; i < MAX_FINGERS; i++) {
				if (fingers[i].state >= ANN_FINGER_ACTIVE_INNER) {
					fingers[i].state = ANN_FINGER_IDLE;
				}
			}
			if (mode_low == TOUCH_MODE_WAKE && t.type == ANNIBALE_TOUCH_DOWN) {
				struct annibale_touch press = {
					.time = t.time, .type = ANNIBALE_BUTTON_DOWN, .finger = ANNIBALE_BUTTON_LOCK
				};
				send_button(buttons, &press);
				press.type = ANNIBALE_BUTTON_UP;
				send_button(buttons, &press);
				fingers[t.finger].state = ANN_FINGER_WAKE_SWALLOWED;
			} else if (t.type == ANNIBALE_TOUCH_UP) {
				fingers[t.finger].state = ANN_FINGER_IDLE;
			}
		} else if (fingers[t.finger].state == ANN_FINGER_WAKE_SWALLOWED && t.type != ANNIBALE_TOUCH_DOWN) {
			if (t.type == ANNIBALE_TOUCH_UP) {
				fingers[t.finger].state = ANN_FINGER_IDLE;
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
    say("input26: standalone pid=%d device=%s callbacks=2\n", getpid(), ANNIBALE_TOUCH_DEVICE);
    return input_run();
}
