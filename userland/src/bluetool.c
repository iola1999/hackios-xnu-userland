/*
 * BlueTool replacement for the REDMI K90's WCN7850 Bluetooth controller (/redwood/bin/bluetool,
 * launchd job com.apple.BlueTool).
 *
 * bluetoothd asks BlueTool over XPC to run the "boot" and "init" scripts before opening the H4
 * transport: a dictionary with kMsgId = "RunBuiltinScript" (or "RunExternalScript", "RunCommand")
 * and kMsgArgs = { script = "boot" | "init" | "deepsleep", btaddress = ... }. Stock BlueTool only
 * knows Apple PCIe and Broadcom UART controllers. Here the kernel's annibale_bt_hci.c downloads
 * the WCN7850 TLV rampatch and NVM over SE14 and runs HCI Reset, then sets kern.redwood.bt_ready=1.
 *
 * Waiting for kern.redwood.bt_ready before answering holds bluetoothd until the controller and its
 * local-mac-address on the IOKit "bluetooth" service are ready. The reply carries result = 0,
 * manufacturer = 15 (Broadcom, whose vendor commands annibale_bt_ios.c answers locally), and a
 * firmware description string.
 */
#include <dispatch/dispatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>
#include <xpc/xpc.h>

extern xpc_connection_t listen_on(const char *name, dispatch_queue_t queue, uint64_t flags)
    __asm__("_xpc_connection_create_mach_service");

static int
wait_bt_ready(int *waited_ms)
{
	int ready = 0;
	int waited = 0;

	for (waited = 0; waited < 40000; waited += 50) {
		size_t len = sizeof(ready);

		if (sysctlbyname("kern.redwood.bt_ready", &ready, &len, NULL, 0) == 0 && ready != 0) {
			break;
		}
		usleep(50 * 1000);
	}
	if (waited_ms != NULL) {
		*waited_ms = waited;
	}
	return ready;
}

static void
answer(xpc_object_t message)
{
	const char *id = xpc_dictionary_get_string(message, "kMsgId");
	xpc_object_t args = xpc_dictionary_get_value(message, "kMsgArgs");
	const char *script = args != NULL && xpc_get_type(args) == XPC_TYPE_DICTIONARY ?
	    xpc_dictionary_get_string(args, "script") : NULL;
	xpc_object_t reply = xpc_dictionary_create_reply(message);
	xpc_connection_t peer = xpc_dictionary_get_remote_connection(message);
	int waited_ms = 0;
	int ready = wait_bt_ready(&waited_ms);

	fprintf(stderr, "bluetool: %s, script %s: done (bt_ready=%d, waited %d ms)\n",
	    id != NULL ? id : "(no id)", script != NULL ? script : "(none)", ready, waited_ms);
	if (reply == NULL || peer == NULL) {
		return;
	}
	xpc_dictionary_set_int64(reply, "result", 0);
	xpc_dictionary_set_int64(reply, "manufacturer", 15);
	xpc_dictionary_set_string(reply, "firmware", "Qualcomm WCN7850, brought up by the kernel");
	xpc_dictionary_set_string(reply, "output", "");
	xpc_connection_send_message(peer, reply);
	xpc_release(reply);
}

int
main(void)
{
	xpc_connection_t listener;
	int waited_ms = 0;
	int ready;

	setvbuf(stderr, NULL, _IOLBF, 0);
	ready = wait_bt_ready(&waited_ms);
	listener = listen_on("com.apple.BlueTool", NULL, XPC_CONNECTION_MACH_SERVICE_LISTENER);
	if (listener == NULL) {
		fprintf(stderr, "bluetool: no mach service com.apple.BlueTool\n");
		return 1;
	}
	xpc_connection_set_event_handler(listener, ^(xpc_object_t peer) {
		if (xpc_get_type(peer) != XPC_TYPE_CONNECTION) {
			return;
		}
		xpc_connection_set_event_handler((xpc_connection_t)peer, ^(xpc_object_t message) {
			if (xpc_get_type(message) == XPC_TYPE_DICTIONARY) {
				answer(message);
			}
		});
		xpc_connection_resume((xpc_connection_t)peer);
	});
	xpc_connection_resume(listener);
	fprintf(stderr, "bluetool: serving com.apple.BlueTool (bt_ready=%d, waited %d ms)\n", ready, waited_ms);
	dispatch_main();
}
