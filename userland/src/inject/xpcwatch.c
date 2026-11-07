/*
 * libredwood-xpcwatch.dylib: which named services a process connects to, and how often.
 *
 * Several processes of the desktop keep two or three processors busy minutes after boot
 * (SpringBoard, appprotectiond, com.apple.MapKit.SnapshotService; kernel-12377-cmd23-state1), and
 * their samples sit in XPC connection setup and teardown, with launchd answering. A client that
 * reconnects in a loop to a service that cannot answer looks like that. This library is inserted
 * into such processes (etc/inject.conf) and interposes the calls that name a service, so the names
 * and their counts reach the kernel log (kern.redwood.say, which any process may write, because
 * the standard error of an XPC service goes nowhere): the first connection to each name, then at
 * every power of two, "xpcwatch NAME N".
 */
#include <dispatch/dispatch.h>
#include <mach/mach.h>
#include <os/lock.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/sysctl.h>
#include <xpc/xpc.h>

extern kern_return_t bootstrap_look_up(mach_port_t bp, const char *service_name, mach_port_t *sp);
/* The iOS SDK marks this one unavailable; the library exports it all the same. */
extern xpc_connection_t real_create_mach_service(const char *name, dispatch_queue_t queue, uint64_t flags)
    __asm__("_xpc_connection_create_mach_service");

#define XPCWATCH_NAMES 128

static struct {
	char name[96];
	uint32_t count;
} names[XPCWATCH_NAMES];
static os_unfair_lock names_lock = OS_UNFAIR_LOCK_INIT;

static void
say(const char *text)
{
	(void)sysctlbyname("kern.redwood.say", NULL, NULL, (void *)text, strlen(text) + 1);
}

static void
count(const char *kind, const char *name, int result)
{
	uint32_t n = 0;
	char line[200];

	if (name == NULL)
		return;
	os_unfair_lock_lock(&names_lock);
	for (unsigned i = 0; i < XPCWATCH_NAMES; i++) {
		if (names[i].count == 0) {
			strlcpy(names[i].name, name, sizeof(names[i].name));
			n = names[i].count = 1;
			break;
		}
		if (strncmp(names[i].name, name, sizeof(names[i].name) - 1) == 0) {
			n = ++names[i].count;
			break;
		}
	}
	os_unfair_lock_unlock(&names_lock);
	if (n == 0 || (n & (n - 1)) != 0)
		return;
	if (result != 0)
		snprintf(line, sizeof(line), "xpcwatch %s %s %u (last %#x)", kind, name, n, result);
	else
		snprintf(line, sizeof(line), "xpcwatch %s %s %u", kind, name, n);
	say(line);
}

static xpc_connection_t
watch_create_mach_service(const char *name, dispatch_queue_t queue, uint64_t flags)
{
	count("mach", name, 0);
	return real_create_mach_service(name, queue, flags);
}

static xpc_connection_t
watch_create(const char *name, dispatch_queue_t queue)
{
	count("service", name, 0);
	return xpc_connection_create(name, queue);
}

static kern_return_t
watch_look_up(mach_port_t bp, const char *name, mach_port_t *sp)
{
	kern_return_t kr = bootstrap_look_up(bp, name, sp);

	count("lookup", name, kr);
	return kr;
}

__attribute__((used, section("__DATA,__interpose"))) static const struct {
	const void *replacement, *original;
} interposers[] = {
	{ (const void *)watch_create_mach_service, (const void *)real_create_mach_service },
	{ (const void *)watch_create, (const void *)xpc_connection_create },
	{ (const void *)watch_look_up, (const void *)bootstrap_look_up },
};
