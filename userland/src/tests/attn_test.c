/*
 * attn_test: why SpringBoard's AttentionAwareness clients get ENOTCONN (P24, automatic lock).
 *   - bootstrap_look_up of com.apple.AttentionAwareness: is the service there?
 *   - an XPC message with a reply on that service: does the peer answer or drop the connection?
 *   - AWAttentionAwarenessClient with a 5 s attention-lost timeout, as IdleTimerHosting configures
 *     one: the NSError of -resumeWithError:.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <mach/mach.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xpc/xpc.h>
#include <dispatch/dispatch.h>

extern mach_port_t bootstrap_port;
extern kern_return_t bootstrap_look_up(mach_port_t bp, const char *service_name, mach_port_t *sp);
/* (marked unavailable on iOS in the SDK header; the symbol is in libxpc all the same) */
extern xpc_connection_t attn_create_mach_service(const char *name, dispatch_queue_t queue, uint64_t flags)
    __asm__("_xpc_connection_create_mach_service");

static id
cls_new(const char *name)
{
	Class c = objc_getClass(name);
	return c ? ((id (*)(id, SEL))objc_msgSend)(((id (*)(Class, SEL))objc_msgSend)(c, sel_registerName("alloc")),
	    sel_registerName("init")) : nil;
}

static const char *
described(id o)
{
	id d = o ? ((id (*)(id, SEL))objc_msgSend)(o, sel_registerName("description")) : nil;
	return d ? ((const char *(*)(id, SEL))objc_msgSend)(d, sel_registerName("UTF8String")) : "(nil)";
}

int
main(int argc, char **argv)
{
	const char *tags = argc > 1 ? argv[1] : NULL;   /* "tags": timeout keys, "tagkeys": tag keys */
	mach_port_t port = MACH_PORT_NULL;
	kern_return_t kr = bootstrap_look_up(bootstrap_port, "com.apple.AttentionAwareness", &port);
	printf("attn_test: bootstrap_look_up %#x port %#x\n", kr, port);

	xpc_connection_t c = attn_create_mach_service("com.apple.AttentionAwareness", NULL, 0);
	xpc_connection_set_event_handler(c, ^(xpc_object_t e) {
		printf("attn_test: xpc event %s\n", xpc_get_type(e) == XPC_TYPE_ERROR ?
		    xpc_dictionary_get_string(e, XPC_ERROR_KEY_DESCRIPTION) : "message");
	});
	xpc_connection_resume(c);
	xpc_object_t msg = xpc_dictionary_create(NULL, NULL, 0);
	xpc_object_t reply = xpc_connection_send_message_with_reply_sync(c, msg);
	printf("attn_test: xpc reply %s\n", xpc_get_type(reply) == XPC_TYPE_ERROR ?
	    xpc_dictionary_get_string(reply, XPC_ERROR_KEY_DESCRIPTION) : "a dictionary");

	if (!dlopen("/System/Library/PrivateFrameworks/AttentionAwareness.framework/AttentionAwareness", RTLD_NOW)) {
		printf("attn_test: dlopen failed: %s\n", dlerror());
		return 1;
	}
	if (tags && strcmp(tags, "methods") == 0) {
		/* every method of the framework's classes whose name has a tag or a timeout in it, at its
		 * address in the shared cache as dsc.py names it (image base 0x1bdda3000 in 23G90) */
		const char *path = "/System/Library/PrivateFrameworks/AttentionAwareness.framework/AttentionAwareness";
		unsigned n = 0;
		const char **names = objc_copyClassNamesForImage(path, &n);
		Dl_info info = { 0 };
		dladdr((void *)objc_getClass("AWAttentionAwarenessClient"), &info);
		uintptr_t slide = (uintptr_t)info.dli_fbase - 0x1bdda3000ul;
		for (unsigned i = 0; i < n; i++) {
			for (int meta = 0; meta < 2; meta++) {
				Class c = objc_getClass(names[i]);
				if (meta) c = object_getClass((id)c);
				unsigned m = 0;
				Method *list = class_copyMethodList(c, &m);
				for (unsigned j = 0; j < m; j++) {
					const char *sel = sel_getName(method_getName(list[j]));
					if (strcmp(names[i], "AWAttentionAwareService") == 0 || strcmp(names[i], "AWScheduler") == 0 ||
					    strcasestr(names[i], "HID") || strcasestr(sel, "tag") || strcasestr(sel, "timeout") || strcasestr(sel, "config") || strcasestr(sel, "HID") || strcasestr(sel, "nterested") || strcasestr(sel, "UUID") || strcasestr(sel, "builtIn") || strcasestr(sel, "Coder"))
						printf("attn_test: %c[%s %s] %#lx %s\n", meta ? '+' : '-', names[i], sel,
						    (unsigned long)((uintptr_t)method_getImplementation(list[j]) - slide),
						    method_getTypeEncoding(list[j]));
				}
				free(list);
			}
		}
		free(names);
		return 0;
	}
	id config = cls_new("AWAttentionAwarenessConfiguration");
	id client = cls_new("AWAttentionAwarenessClient");
	if (!config || !client) {
		printf("attn_test: no AWAttentionAwarenessConfiguration or AWAttentionAwarenessClient\n");
		return 1;
	}
	if (tags && strcmp(tags, "coder") == 0) {
		/* the configuration through NSKeyedArchiver and back, as NSXPC carries it to backboardd */
		double timeouts[3] = { 40.0, 42.0, 60.0 };
		int tagv[3] = { 1, 2, 3 };
		CFNumberRef keys[3], values[3];
		for (int i = 0; i < 3; i++) {
			keys[i] = CFNumberCreate(NULL, kCFNumberDoubleType, &timeouts[i]);
			values[i] = CFNumberCreate(NULL, kCFNumberIntType, &tagv[i]);
		}
		CFDictionaryRef d = CFDictionaryCreate(NULL, (const void **)keys, (const void **)values, 3,
		    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
		((void (*)(id, SEL, id))objc_msgSend)(config, sel_registerName("setIdentifier:"), (id)CFSTR("com.annibale.coder"));
		((void (*)(id, SEL, id))objc_msgSend)(config, sel_registerName("setAttentionLostTimeoutDictionary:"), (id)d);
		id err = nil;
		id data = ((id (*)(Class, SEL, id, BOOL, id *))objc_msgSend)(objc_getClass("NSKeyedArchiver"),
		    sel_registerName("archivedDataWithRootObject:requiringSecureCoding:error:"), config, YES, &err);
		printf("attn_test: archived %s (%s)\n", data ? "ok" : "FAILED", described(err));
		id back = data ? ((id (*)(Class, SEL, Class, id, id *))objc_msgSend)(objc_getClass("NSKeyedUnarchiver"),
		    sel_registerName("unarchivedObjectOfClass:fromData:error:"), objc_getClass("AWAttentionAwarenessConfiguration"),
		    data, &err) : nil;
		printf("attn_test: unarchived %s (%s)\n", back ? "ok" : "FAILED", described(err));
		printf("attn_test: before: dictionary %s timeouts %s\n",
		    described(((id (*)(id, SEL))objc_msgSend)(config, sel_registerName("attentionLostTimeoutDictionary"))),
		    described(((id (*)(id, SEL))objc_msgSend)(config, sel_registerName("attentionLostTimeouts"))));
		printf("attn_test: after: dictionary %s timeouts %s\n",
		    back ? described(((id (*)(id, SEL))objc_msgSend)(back, sel_registerName("attentionLostTimeoutDictionary"))) : "-",
		    back ? described(((id (*)(id, SEL))objc_msgSend)(back, sel_registerName("attentionLostTimeouts"))) : "-");
		id copy = ((id (*)(id, SEL))objc_msgSend)(config, sel_registerName("copy"));
		printf("attn_test: copy: dictionary %s\n",
		    described(((id (*)(id, SEL))objc_msgSend)(copy, sel_registerName("attentionLostTimeoutDictionary"))));
		return 0;
	}
	CFStringRef ident = CFSTR("com.annibale.attn_test");
	((void (*)(id, SEL, id))objc_msgSend)(config, sel_registerName("setIdentifier:"), (id)ident);
	if (tags) {
		/* as IdleTimerHosting's ITAttentionAwarenessClient: several timeouts, each with a tag */
		double timeouts[2] = { 4.0, 6.0 };
		int tags[2] = { 1, 3 };
		CFNumberRef keys[2], values[2];
		for (int i = 0; i < 2; i++) {
			keys[i] = CFNumberCreate(NULL, kCFNumberDoubleType, &timeouts[i]);
			values[i] = CFNumberCreate(NULL, kCFNumberIntType, &tags[i]);
		}
		bool tag_keys = strcmp(tags, "tagkeys") == 0;
		CFDictionaryRef d = CFDictionaryCreate(NULL, (const void **)(tag_keys ? values : keys),
		    (const void **)(tag_keys ? keys : values), 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
		((void (*)(id, SEL, id))objc_msgSend)(config, sel_registerName("setAttentionLostTimeoutDictionary:"), (id)d);
		printf("attn_test: timeouts %s\n", described(((id (*)(id, SEL))objc_msgSend)(config,
		    sel_registerName("attentionLostTimeoutDictionary"))));
		/* the process's tag table, and how an event of each tag index reads back through it */
		id shared = ((id (*)(Class, SEL))objc_msgSend)(objc_getClass("AWAttentionAwarenessClientConfig"),
		    sel_registerName("sharedClientConfig"));
		printf("attn_test: config tagIndex %llu, timeouts %s\n",
		    ((unsigned long long (*)(id, SEL))objc_msgSend)(config, sel_registerName("tagIndex")),
		    described(((id (*)(id, SEL))objc_msgSend)(config, sel_registerName("attentionLostTimeouts"))));
		for (unsigned long long i = 0; i < 6 && shared; i++) {
			id tag = ((id (*)(id, SEL, unsigned long long))objc_msgSend)(shared, sel_registerName("tagForIndex:"), i);
			id ev = ((id (*)(id, SEL, unsigned long long, unsigned long long, double))objc_msgSend)(
			    ((id (*)(Class, SEL))objc_msgSend)(objc_getClass("AWAttentionLostEvent"), sel_registerName("alloc")),
			    sel_registerName("initWithTimestamp:tagIndex:attentionLostTimeout:"), 0, i, 4.0);
			((void (*)(id, SEL, id))objc_msgSend)(ev, sel_registerName("updateWithConfig:"), config);
			printf("attn_test: tagForIndex(%llu) %s; event %s\n", i, described(tag), described(ev));
		}
	} else {
		((void (*)(id, SEL, double))objc_msgSend)(config, sel_registerName("setAttentionLostTimeout:"), 5.0);
	}
	((void (*)(id, SEL, unsigned long long))objc_msgSend)(config, sel_registerName("setEventMask:"), 0xffffull);
	id error = nil;
	BOOL ok = ((BOOL (*)(id, SEL, id, id *))objc_msgSend)(client, sel_registerName("setConfiguration:shouldReset:error:"),
	    config, &error);
	printf("attn_test: setConfiguration %d %s\n", ok, described(error));
	void (^handler)(id) = ^(id event) {
		double timeout = 0;
		if (class_respondsToSelector(object_getClass(event), sel_registerName("attentionLostTimeout")))
			timeout = ((double (*)(id, SEL))objc_msgSend)(event, sel_registerName("attentionLostTimeout"));
		printf("attn_test: event %s; timeout %.17g, associatedObject %s\n", described(event), timeout,
		    described(((id (*)(id, SEL))objc_msgSend)(event, sel_registerName("associatedObject"))));
		id held = ((id (*)(id, SEL))objc_msgSend)(client, sel_registerName("configuration"));
		id dict = held ? ((id (*)(id, SEL))objc_msgSend)(held, sel_registerName("attentionLostTimeoutDictionary")) : nil;
		printf("attn_test: the client's configuration %p, its timeout dictionary %s\n", (void *)held, described(dict));
		CFNumberRef key = CFNumberCreate(NULL, kCFNumberDoubleType, &timeout);
		printf("attn_test: looked up by hand: %s\n", dict ? described((id)CFDictionaryGetValue((CFDictionaryRef)dict, key)) : "(no dictionary)");
		CFRelease(key);
		fflush(stdout);
	};
	((void (*)(id, SEL, dispatch_queue_t, id))objc_msgSend)(client, sel_registerName("setEventHandlerWithQueue:block:"),
	    dispatch_get_global_queue(0, 0), (id)handler);
	error = nil;
	ok = ((BOOL (*)(id, SEL, id *))objc_msgSend)(client, sel_registerName("resumeWithError:"), &error);
	printf("attn_test: resume %d %s; waiting 20 s for an attention-lost event\n", ok, described(error));
	fflush(stdout);
	/* what backboardd's PersistentDataManager keeps in its shared memory while this client is registered */
	sleep(1);
	int fd = shm_open("com.apple.AttentionAwareness", O_RDONLY, 0);
	const uint32_t *w = fd >= 0 ? mmap(NULL, 16384, PROT_READ, MAP_SHARED, fd, 0) : MAP_FAILED;
	if (w != MAP_FAILED) {
		unsigned nonzero = 0;
		for (unsigned i = 0; i < 4096; i++)
			nonzero += w[i] != 0;
		/* header: version, max clients, size, entry size (words 0, 2, 4, 6); entries from byte 32 */
		unsigned entry = w[6] ? w[6] : 0x58, max = w[2] ? w[2] : 32, used = 0;
		printf("attn_test: shm entries:");
		for (unsigned e = 0; e < max && 32 + (e + 1) * entry <= 16384; e++) {
			const uint32_t *p = w + (32 + e * entry) / 4;
			unsigned nz = 0;
			for (unsigned k = 0; k < entry / 4; k++)
				nz += p[k] != 0;
			used += nz != 0;
			printf(" %u:%u", e, nz);
		}
		printf("; %u of %u in use\n", used, max);
		printf("attn_test: shm %u nonzero words of 4096; first:", nonzero);
		for (unsigned i = 0; i < 4096 && nonzero; i++)
			if (w[i])
				printf(" [%u]=%#x", i, w[i]), nonzero = nonzero > 48 ? nonzero : nonzero;
		printf("\n");
	} else {
		printf("attn_test: shm not readable (fd %d)\n", fd);
	}
	fflush(stdout);
	sleep(20);
	return 0;
}
