/*
 * attn_test: why SpringBoard's AttentionAwareness clients get ENOTCONN (P24, automatic lock).
 *   - bootstrap_look_up of com.apple.AttentionAwareness: is the service there?
 *   - an XPC message with a reply on that service: does the peer answer or drop the connection?
 *   - AWAttentionAwarenessClient with a 5 s attention-lost timeout, as IdleTimerHosting configures
 *     one: the NSError of -resumeWithError:.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
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
	id config = cls_new("AWAttentionAwarenessConfiguration");
	id client = cls_new("AWAttentionAwarenessClient");
	if (!config || !client) {
		printf("attn_test: no AWAttentionAwarenessConfiguration or AWAttentionAwarenessClient\n");
		return 1;
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
	} else {
		((void (*)(id, SEL, double))objc_msgSend)(config, sel_registerName("setAttentionLostTimeout:"), 5.0);
	}
	((void (*)(id, SEL, unsigned long long))objc_msgSend)(config, sel_registerName("setEventMask:"), 0xffffull);
	id error = nil;
	BOOL ok = ((BOOL (*)(id, SEL, id, id *))objc_msgSend)(client, sel_registerName("setConfiguration:shouldReset:error:"),
	    config, &error);
	printf("attn_test: setConfiguration %d %s\n", ok, described(error));
	void (^handler)(id) = ^(id event) {
		printf("attn_test: event %s\n", described(event));
		fflush(stdout);
	};
	((void (*)(id, SEL, dispatch_queue_t, id))objc_msgSend)(client, sel_registerName("setEventHandlerWithQueue:block:"),
	    dispatch_get_global_queue(0, 0), (id)handler);
	error = nil;
	ok = ((BOOL (*)(id, SEL, id *))objc_msgSend)(client, sel_registerName("resumeWithError:"), &error);
	printf("attn_test: resume %d %s; waiting 20 s for an attention-lost event\n", ok, described(error));
	fflush(stdout);
	sleep(20);
	return 0;
}
