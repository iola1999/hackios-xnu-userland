/*
 * Optional diagnostics for fairplayd.H2; injection is selected by the test configuration.
 * iOS 26 build 23G90 imports IORegistryEntryFromPath, IORegistryEntryCreateCFProperty and IOConnectCallStructMethod directly. Observe those entry points and each caller-requested IOIteratorNext without consuming or resetting an iterator in the probe.
 * Matching names, user-client types, selectors, buffer lengths and return values identify the native IOKit contract. Authentication buffers, property values, filesystem paths and argument contents stay out of the log.
 * The 23G90 matching class, Open type and selectors remain unverified. Earlier-system observations of an early return from main do not establish this build's behavior.
 * Interposing observes calls across image boundaries. Calls within libSystem require kernel tracing; zero hook calls alone cannot establish that no syscall occurred.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <crt_externs.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <dispatch/dispatch.h>

/* (the SDK here has no xpc/xpc.h; only this one call is wanted, and only its name is read) */
typedef void *xpc_connection_t;
extern xpc_connection_t xpc_connection_create_mach_service(const char *name, dispatch_queue_t queue, uint64_t flags);
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <mach/thread_act.h>
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ucontext.h>
#include <dlfcn.h>
#include <unistd.h>

extern const void *_dyld_get_shared_cache_range(size_t *length);

/* MobileGestalt and the entitlement reader, declared here because the SDK keeps no headers for them */
extern CFTypeRef MGCopyAnswer(CFStringRef key);
extern CFTypeRef SecTaskCopyValueForEntitlement(void *task, CFStringRef entitlement, CFErrorRef *error);

#define CACHE_BASE 0x180000000UL

static uintptr_t slide;
static bool ours;
static uintptr_t own_image_base, own_text_start, own_text_size;

static void
say(const char *format, ...)
{
	char line[512];
	va_list ap;
	int n, saved_errno = errno;

	va_start(ap, format);
	n = vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	if (n > (int)sizeof(line) - 1) {
		n = sizeof(line) - 1;
	}
	if (n > 0) {
		write(2, line, (size_t)n);
	}
	errno = saved_errno;
}

/* up the frame pointers, unslid, for "ipsw dyld a2s" */
static void
frames_from(uintptr_t fp, char *text, size_t size)
{
	size_t used = 0;

	text[0] = 0;
	for (unsigned i = 0; i < 10 && fp != 0 && (fp & 7) == 0 && used + 20 < size; i++) {
		uintptr_t next = ((uintptr_t *)fp)[0], ret = ((uintptr_t *)fp)[1];

		if (ret == 0) {
			break;
		}
		used += (size_t)snprintf(text + used, size - used, " %#lx", ret - slide);
		fp = next > fp ? next : 0;
	}
}

static void
say_the_fault(int signal_number, siginfo_t *info, void *context_in)
{
	ucontext_t *context = context_in;
	uintptr_t pc = 0, fp = 0;
	char frames[10 * 20 + 1] = "";

	if (context != NULL && context->uc_mcontext != NULL) {
		pc = (uintptr_t)__darwin_arm_thread_state64_get_pc(context->uc_mcontext->__ss);
		fp = (uintptr_t)__darwin_arm_thread_state64_get_fp(context->uc_mcontext->__ss);
	}
	frames_from(fp, frames, sizeof(frames));
	say("redwood: fairplayd: signal %d at %p, pc %#lx, called from%s (unslid)\n", signal_number,
	    info != NULL ? info->si_addr : NULL, pc - slide, frames);
	signal(signal_number, SIG_DFL);
	raise(signal_number);
}

/* IOKit calls retain their original arguments, results and object ownership. */

static uintptr_t
caller_offset(uintptr_t address)
{
	if (own_image_base != 0 && address >= own_image_base && address >= own_text_start &&
	    address - own_text_start < own_text_size) {
		return address - own_image_base;
	}
	return UINTPTR_MAX;
}

static io_registry_entry_t
watched_IORegistryEntryFromPath(mach_port_t port, const char *path)
{
	io_registry_entry_t entry = IORegistryEntryFromPath(port, path);
	int saved_errno = errno;

	if (ours) {
		const char *kind = path == NULL ? "null" : "other";
		uint64_t hash = 0;
		size_t length = 0;
		bool topology = false;
		uintptr_t offset = caller_offset((uintptr_t)__builtin_extract_return_addr(__builtin_return_address(0)));

		if (path != NULL && strncmp(path, "IODeviceTree:/", sizeof("IODeviceTree:/") - 1) == 0) {
			kind = "IODeviceTree";
			topology = true;
		} else if (path != NULL && strncmp(path, "IOService:/", sizeof("IOService:/") - 1) == 0) {
			kind = "IOService";
			topology = true;
		}
		/* Hash only recognized registry topology; path text stays out of the log. */
		if (topology) {
			hash = UINT64_C(14695981039346656037);
			while (length < sizeof(io_string_t) && path[length] != 0) {
				hash = (hash ^ (unsigned char)path[length++]) * UINT64_C(1099511628211);
			}
		}
		say("redwood: fairplayd: IORegistryEntryFromPath path_kind=%s path_hash=%016llx "
		    "path_bytes=%zu path_complete=%u entry_present=%u errno_after=%d caller_main=%u caller_offset=%#lx\n",
		    kind, (unsigned long long)hash, length, topology && length < sizeof(io_string_t),
		    entry != IO_OBJECT_NULL, saved_errno, offset != UINTPTR_MAX, offset != UINTPTR_MAX ? offset : 0);
	}
	errno = saved_errno;
	return entry;
}

static CFTypeRef
watched_IORegistryEntryCreateCFProperty(io_registry_entry_t entry, CFStringRef key,
    CFAllocatorRef allocator, IOOptionBits options)
{
	CFTypeRef value = IORegistryEntryCreateCFProperty(entry, key, allocator, options);
	int saved_errno = errno;

	if (ours) {
		char name[96] = "(unreadable)";
		const char *kind = value != NULL ? "other" : "null";
		const char *units = "none";
		CFTypeID type = value != NULL ? CFGetTypeID(value) : 0;
		CFIndex length = -1;
		uintptr_t offset = caller_offset((uintptr_t)__builtin_extract_return_addr(__builtin_return_address(0)));

		if (key != NULL && CFGetTypeID(key) == CFStringGetTypeID() &&
		    CFStringGetCString(key, name, sizeof(name), kCFStringEncodingUTF8)) {
			for (size_t i = 0; name[i] != 0; i++) {
				unsigned char c = (unsigned char)name[i];
				if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
				    (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) {
					strlcpy(name, "(unreadable)", sizeof(name));
					break;
				}
			}
		} else {
			strlcpy(name, "(unreadable)", sizeof(name));
		}
		/* Read size metadata only; the returned Create reference belongs to the caller. */
		if (value != NULL && type == CFDataGetTypeID()) {
			kind = "data";
			length = CFDataGetLength(value);
			units = "bytes";
		} else if (value != NULL && type == CFStringGetTypeID()) {
			kind = "string";
			length = CFStringGetLength(value);
			units = "utf16";
		} else if (value != NULL && type == CFArrayGetTypeID()) {
			kind = "array";
			length = CFArrayGetCount(value);
			units = "elements";
		} else if (value != NULL && type == CFDictionaryGetTypeID()) {
			kind = "dictionary";
			length = CFDictionaryGetCount(value);
			units = "entries";
		} else if (value != NULL && type == CFNumberGetTypeID()) {
			kind = "number";
		} else if (value != NULL && type == CFBooleanGetTypeID()) {
			kind = "boolean";
		}
		say("redwood: fairplayd: IORegistryEntryCreateCFProperty key=%s value_present=%u "
		    "type=%s type_id=%lu length=%ld units=%s errno_after=%d caller_main=%u caller_offset=%#lx\n",
		    name, value != NULL, kind, (unsigned long)type, (long)length, units, saved_errno,
		    offset != UINTPTR_MAX, offset != UINTPTR_MAX ? offset : 0);
	}
	errno = saved_errno;
	return value;
}

static CFMutableDictionaryRef
watched_IOServiceMatching(const char *name)
{
	CFMutableDictionaryRef matching = IOServiceMatching(name);

	if (ours) {
		say("redwood: fairplayd: IOServiceMatching(\"%s\") -> %s\n", name != NULL ? name : "(null)",
		    matching != NULL ? "a dictionary" : "nothing");
	}
	return matching;
}

static kern_return_t
watched_IOServiceGetMatchingServices(mach_port_t port, CFDictionaryRef matching, io_iterator_t *existing)
{
	kern_return_t result = IOServiceGetMatchingServices(port, matching, existing);

	if (ours) {
		say("redwood: fairplayd: IOServiceGetMatchingServices -> %#x, iterator %#x\n", result,
		    result == KERN_SUCCESS && existing != NULL ? *existing : IO_OBJECT_NULL);
	}
	return result;
}

static io_object_t
watched_IOIteratorNext(io_iterator_t iterator)
{
	io_object_t object = IOIteratorNext(iterator);

	if (ours) {
		say("redwood: fairplayd: IOIteratorNext(%#x) -> %#x\n", iterator, object);
	}
	return object;
}

static io_service_t
watched_IOServiceGetMatchingService(mach_port_t port, CFDictionaryRef matching)
{
	io_service_t service = IOServiceGetMatchingService(port, matching);

	if (ours) {
		say("redwood: fairplayd: IOServiceGetMatchingService -> %s\n", service != 0 ? "a service" : "NOTHING");
	}
	return service;
}

static kern_return_t
watched_IOServiceOpen(io_service_t service, task_port_t task, uint32_t type, io_connect_t *connect)
{
	kern_return_t result = IOServiceOpen(service, task, type, connect);

	if (ours) {
		say("redwood: fairplayd: IOServiceOpen(type %u) -> %#x\n", type, result);
	}
	return result;
}

static kern_return_t
watched_IOConnectCallMethod(mach_port_t connection, uint32_t selector, const uint64_t *input, uint32_t input_count,
    const void *input_struct, size_t input_struct_count, uint64_t *output, uint32_t *output_count,
    void *output_struct, size_t *output_struct_count)
{
	kern_return_t result = IOConnectCallMethod(connection, selector, input, input_count, input_struct,
	    input_struct_count, output, output_count, output_struct, output_struct_count);

	if (ours) {
		say("redwood: fairplayd: IOConnectCallMethod selector %u -> %#x\n", selector, result);
	}
	return result;
}

static kern_return_t
watched_IOConnectCallStructMethod(mach_port_t connection, uint32_t selector, const void *input_struct,
    size_t input_struct_count, void *output_struct, size_t *output_struct_count)
{
	size_t output_capacity = output_struct_count != NULL ? *output_struct_count : 0;
	kern_return_t result = IOConnectCallStructMethod(connection, selector, input_struct,
	    input_struct_count, output_struct, output_struct_count);

	if (ours) {
		say("redwood: fairplayd: IOConnectCallStructMethod selector=%u input_bytes=%zu "
		    "output_capacity=%zu output_bytes=%zu output_size_present=%u result=%#x\n",
		    selector, input_struct_count, output_capacity,
		    output_struct_count != NULL ? *output_struct_count : 0, output_struct_count != NULL, result);
	}
	return result;
}

static int
watched_open(const char *path, int flags, ...)
{
	mode_t mode = 0;

	if ((flags & O_CREAT) != 0) {
		va_list ap;

		va_start(ap, flags);
		mode = (mode_t)va_arg(ap, int);
		va_end(ap);
	}
	int fd = open(path, flags, mode);

	if (ours) {
		say("redwood: fairplayd: open flags=%#x -> %d errno=%d\n", flags, fd, fd < 0 ? errno : 0);
	}
	return fd;
}

/* Only the numeric MIB and return code are logged; sysctl buffers remain private. */
static int
watched_sysctl(int *mib, unsigned count, void *out, size_t *out_size, void *in, size_t in_size)
{
	int result = sysctl(mib, count, out, out_size, in, in_size);

	if (ours && mib != NULL && count >= 1) {
		say("redwood: fairplayd: sysctl [%d %d %d %d] (%u deep) -> %d\n", mib[0], count > 1 ? mib[1] : -1,
		    count > 2 ? mib[2] : -1, count > 3 ? mib[3] : -1, count, result);
	}
	return result;
}

/* Image-local libSystem calls may bypass both public and private interpose entries. */
extern int __sysctl(int *mib, unsigned count, void *out, size_t *out_size, void *in, size_t in_size);

static int
watched___sysctl(int *mib, unsigned count, void *out, size_t *out_size, void *in, size_t in_size)
{
	int result = __sysctl(mib, count, out, out_size, in, in_size);

	if (ours && mib != NULL && count >= 1) {
		say("redwood: fairplayd: sysctl [%d %d %d %d] (%u deep) -> %d\n", mib[0], count > 1 ? mib[1] : -1,
		    count > 2 ? mib[2] : -1, count > 3 ? mib[3] : -1, count, result);
	}
	return result;
}

/* Log the MAC policy and request number without reading the request payload. */
extern int __mac_syscall(const char *policy, int call, void *arg);

static int
watched___mac_syscall(const char *policy, int call, void *arg)
{
	int result = __mac_syscall(policy, call, arg);

	if (ours) {
		say("redwood: fairplayd: __mac_syscall(\"%s\", %d) -> %d\n", policy != NULL ? policy : "(null)",
		    call, result);
	}
	return result;
}

static int
watched_sysctlbyname(const char *name, void *out, size_t *out_size, void *in, size_t in_size)
{
	int result = sysctlbyname(name, out, out_size, in, in_size);

	if (ours) {
		say("redwood: fairplayd: sysctlbyname(\"%s\") -> %d\n", name != NULL ? name : "(null)", result);
	}
	return result;
}

static void *
watched_dlopen(const char *path, int mode)
{
	void *library = dlopen(path, mode);

	if (ours) {
		say("redwood: fairplayd: dlopen mode=%#x -> %s\n", mode, library != NULL ? "loaded" : "failed");
	}
	return library;
}

static xpc_connection_t
watched_xpc_connection_create_mach_service(const char *name, dispatch_queue_t queue, uint64_t flags)
{
	xpc_connection_t connection = xpc_connection_create_mach_service(name, queue, flags);

	if (ours) {
		say("redwood: fairplayd: xpc_connection_create_mach_service(\"%s\") -> %s\n",
		    name != NULL ? name : "(null)", connection != NULL ? "a connection" : "nothing");
	}
	return connection;
}

static int
watched_stat(const char *path, struct stat *out)
{
	int result = stat(path, out);

	if (ours) {
		say("redwood: fairplayd: stat -> %d errno=%d\n", result, result < 0 ? errno : 0);
	}
	return result;
}

static int
watched_access(const char *path, int mode)
{
	int result = access(path, mode);

	if (ours) {
		say("redwood: fairplayd: access -> %d errno=%d\n", result, result < 0 ? errno : 0);
	}
	return result;
}

static int
watched_mkdir(const char *path, mode_t mode)
{
	int result = mkdir(path, mode);

	if (ours) {
		say("redwood: fairplayd: mkdir -> %d errno=%d\n", result, result < 0 ? errno : 0);
	}
	return result;
}

/* MobileGestalt and entitlement logs contain keys and presence only. */
static CFTypeRef
watched_MGCopyAnswer(CFStringRef key)
{
	CFTypeRef answer = MGCopyAnswer(key);
	int saved_errno = errno;

	if (ours) {
		char text[96] = "";

		if (key == NULL || !CFStringGetCString(key, text, sizeof(text), kCFStringEncodingUTF8)) {
			strlcpy(text, "(unreadable)", sizeof(text));
		}
		say("redwood: fairplayd: MGCopyAnswer(\"%s\") -> %s\n", text, answer != NULL ? "an answer" : "NOTHING");
	}
	errno = saved_errno;
	return answer;
}

static CFTypeRef
watched_SecTaskCopyValueForEntitlement(void *task, CFStringRef entitlement, CFErrorRef *error)
{
	CFTypeRef value = SecTaskCopyValueForEntitlement(task, entitlement, error);
	int saved_errno = errno;

	if (ours) {
		char text[96] = "";

		if (entitlement == NULL || !CFStringGetCString(entitlement, text, sizeof(text), kCFStringEncodingUTF8)) {
			strlcpy(text, "(unreadable)", sizeof(text));
		}
		say("redwood: fairplayd: entitlement \"%s\" -> %s\n", text, value != NULL ? "held" : "NOT HELD");
	}
	errno = saved_errno;
	return value;
}

static void
watched__exit(int status)
{
	char frames[10 * 20 + 1] = "";

	if (ours) {
		frames_from((uintptr_t)__builtin_frame_address(0), frames, sizeof(frames));
		say("redwood: fairplayd: LEAVING, _exit(%d), called from%s (unslid)\n", status, frames);
	}
	_exit(status);
}

static void
watched_abort(void)
{
	char frames[10 * 20 + 1] = "";

	if (ours) {
		frames_from((uintptr_t)__builtin_frame_address(0), frames, sizeof(frames));
		say("redwood: fairplayd: LEAVING, abort(), called from%s (unslid)\n", frames);
	}
	abort();
}

/* Bounded main-thread PC sampling also covers execution within the obfuscated binary. */
#define SAMPLES 2048
static uintptr_t samples[SAMPLES];
static unsigned sample_count;
static mach_port_t main_thread;
static uintptr_t own_slide;

static void *
sampler(__unused void *ignored)
{
	while (sample_count < SAMPLES) {
		arm_thread_state64_t state;
		mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;

		if (thread_get_state(main_thread, ARM_THREAD_STATE64, (thread_state_t)&state, &count) != KERN_SUCCESS) {
			break;
		}
		samples[sample_count++] = (uintptr_t)arm_thread_state64_get_pc(state);
		usleep(500);
	}
	return NULL;
}

static void
at_the_end(void)
{
	unsigned shown = 0;

	if (!ours) {
		return;
	}
	say("redwood: fairplayd: atexit samples=%u\n",
	    sample_count);
	/* Print at most 40 distinct samples, newest first. */
	for (unsigned i = sample_count; i > 0 && shown < 40; i--) {
		uintptr_t pc = samples[i - 1];
		bool seen = false;

		for (unsigned j = i; j < sample_count; j++) {
			seen |= samples[j] == pc;
		}
		if (seen) {
			continue;
		}
		shown++;
		say("redwood: fairplayd: was at %#lx (cache %#lx, own %#lx)\n", pc, pc - slide, pc - own_slide);
	}
}

static void
watched_exit(int status)
{
	char frames[10 * 20 + 1] = "";

	if (ours) {
		frames_from((uintptr_t)__builtin_frame_address(0), frames, sizeof(frames));
		say("redwood: fairplayd: LEAVING, exit(%d), called from%s (unslid)\n", status, frames);
	}
	exit(status);
}

__attribute__((used)) static struct {
	const void *ours, *theirs;
} interposed[] __attribute__((section("__DATA,__interpose"))) = {
	{ (const void *)watched_IORegistryEntryFromPath, (const void *)IORegistryEntryFromPath },
	{ (const void *)watched_IORegistryEntryCreateCFProperty, (const void *)IORegistryEntryCreateCFProperty },
	{ (const void *)watched_IOServiceMatching, (const void *)IOServiceMatching },
	{ (const void *)watched_IOServiceGetMatchingServices, (const void *)IOServiceGetMatchingServices },
	{ (const void *)watched_IOServiceGetMatchingService, (const void *)IOServiceGetMatchingService },
	{ (const void *)watched_IOIteratorNext, (const void *)IOIteratorNext },
	{ (const void *)watched_IOServiceOpen, (const void *)IOServiceOpen },
	{ (const void *)watched_IOConnectCallMethod, (const void *)IOConnectCallMethod },
	{ (const void *)watched_IOConnectCallStructMethod, (const void *)IOConnectCallStructMethod },
	{ (const void *)watched_exit, (const void *)exit },
	{ (const void *)watched__exit, (const void *)_exit },
	{ (const void *)watched_abort, (const void *)abort },
	{ (const void *)watched_open, (const void *)open },
	{ (const void *)watched_sysctlbyname, (const void *)sysctlbyname },
	{ (const void *)watched_sysctl, (const void *)sysctl },
	{ (const void *)watched___sysctl, (const void *)__sysctl },
	{ (const void *)watched___mac_syscall, (const void *)__mac_syscall },
	{ (const void *)watched_MGCopyAnswer, (const void *)MGCopyAnswer },
	{ (const void *)watched_SecTaskCopyValueForEntitlement, (const void *)SecTaskCopyValueForEntitlement },
	{ (const void *)watched_dlopen, (const void *)dlopen },
	{ (const void *)watched_xpc_connection_create_mach_service, (const void *)xpc_connection_create_mach_service },
	{ (const void *)watched_stat, (const void *)stat },
	{ (const void *)watched_access, (const void *)access },
	{ (const void *)watched_mkdir, (const void *)mkdir },
};

__attribute__((constructor))
static void
redwood_fairplay_init(void)
{
	const char *name = getprogname();
	size_t length = 0;
	struct sigaction action;

	ours = name != NULL && strncmp(name, "fairplayd", 9) == 0;
	if (!ours) {
		return;
	}
	slide = (uintptr_t)_dyld_get_shared_cache_range(&length) - CACHE_BASE;
	memset(&action, 0, sizeof(action));
	action.sa_sigaction = say_the_fault;
	action.sa_flags = SA_SIGINFO | SA_RESETHAND;
	sigaction(SIGSEGV, &action, NULL);
	sigaction(SIGBUS, &action, NULL);
	sigaction(SIGABRT, &action, NULL);
	sigaction(SIGILL, &action, NULL);
	sigaction(SIGTRAP, &action, NULL);
	main_thread = mach_thread_self();
	own_slide = (uintptr_t)_dyld_get_image_vmaddr_slide(0);
	{
		const struct mach_header_64 *header = (const struct mach_header_64 *)_dyld_get_image_header(0);
		unsigned long text_size = 0;

		own_image_base = (uintptr_t)header;
		if (header != NULL) {
			own_text_start = (uintptr_t)getsegmentdata(header, "__TEXT", &text_size);
			own_text_size = text_size;
		}
	}
	atexit(at_the_end);
	{
		pthread_t thread;

		if (pthread_create(&thread, NULL, sampler, NULL) == 0) {
			pthread_detach(thread);
		}
	}
	{
		int *argc = _NSGetArgc();

		say("redwood: fairplayd: probe loaded argc=%d\n", argc != NULL ? *argc : -1);
	}
}
