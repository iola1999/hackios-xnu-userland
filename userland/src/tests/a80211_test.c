/*
 * a80211_test: what userland sees through IO80211.framework's Apple80211 API, printed as the API
 * gives it (the kernel log only says "answered/error"). "dump" opens the interface and asks for
 * SSID, BSSID, STATE and CURRENT_NETWORK as Apple80211Get and Apple80211CopyCurrentNetwork return
 * them; "wifilog" writes com.apple.wifid's logging defaults so wifid's WFLog messages reach the
 * log (a restart of wifid reads them again); "power 0|1" asks for the radio off or on the way
 * WiFiManagerClientSetPower does, an Apple80211Set of type 19.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void *handle;

static int (*a80211_open)(void **);
static int (*a80211_bind)(void *, CFStringRef);
static int (*a80211_close)(void *);
static int (*a80211_get)(void *, int32_t, uint64_t, void *, uint32_t);
static int (*a80211_set)(void *, int32_t, void *, const void *, uint32_t);
static int (*a80211_current)(void *, CFDictionaryRef *);

static void
show(const char *tag, CFTypeRef what)
{
	CFStringRef text = what == NULL ? NULL : CFCopyDescription(what);

	if (text != NULL) {
		char line[4096];

		if (CFStringGetCString(text, line, sizeof(line), kCFStringEncodingUTF8)) {
			printf("a80211_test: %s: %s\n", tag, line);
		}
		CFRelease(text);
	} else {
		printf("a80211_test: %s: (null)\n", tag);
	}
}

static int
asked(int32_t type, uint32_t room)
{
	uint8_t buffer[4096] = { 0 };
	int error = a80211_get(handle, type, 0, buffer, room);
	unsigned i;

	printf("a80211_test: get %d -> %d, first bytes:", type, error);
	for (i = 0; i < 40; i++) {
		printf(" %02x", buffer[i]);
	}
	printf("\n");
	return error;
}

static int
power(int on)
{
	struct {
		uint32_t version, radios, state[4];
	} asked = { .version = 1, .radios = 1, .state = { on } };

	return a80211_set(handle, 19, NULL, &asked, sizeof(asked));
}

int
main(int argc, char **argv)
{
	void *io;
	int error;

	setbuf(stdout, NULL);
	fprintf(stderr, "a80211_test: start\n");
	io = dlopen("/System/Library/PrivateFrameworks/IO80211.framework/IO80211", RTLD_NOW);
	fprintf(stderr, "a80211_test: dlopen -> %p\n", io);
	if (io == NULL) {
		printf("a80211_test: IO80211: %s\n", dlerror());
		return 1;
	}
	a80211_open = dlsym(io, "Apple80211Open");
	a80211_bind = dlsym(io, "Apple80211BindToInterface");
	a80211_close = dlsym(io, "Apple80211Close");
	a80211_get = dlsym(io, "Apple80211Get");
	a80211_set = dlsym(io, "Apple80211Set");
	a80211_current = dlsym(io, "Apple80211CopyCurrentNetwork");
	printf("a80211_test: open %p bind %p get %p set %p current %p\n", a80211_open, a80211_bind,
	    a80211_get, a80211_set, a80211_current);

	if (argc >= 2 && strcmp(argv[1], "wifilog") == 0) {
		CFPreferencesSetValue(CFSTR("WiFiManagerLoggingEnabled"), kCFBooleanTrue,
		    CFSTR("com.apple.wifid"), kCFPreferencesAnyUser, kCFPreferencesAnyHost);
		CFPreferencesSetValue(CFSTR("LoggingEnabled"), kCFBooleanTrue,
		    CFSTR("com.apple.wifid"), kCFPreferencesAnyUser, kCFPreferencesAnyHost);
		printf("a80211_test: com.apple.wifid written: %s\n",
		    CFPreferencesAppSynchronize(CFSTR("com.apple.wifid")) ? "synced" : "NOT synced");
		return 0;
	}
	if (argc >= 3 && strcmp(argv[1], "mwpower") == 0) {
		void *mw = dlopen("/System/Library/PrivateFrameworks/MobileWiFi.framework/MobileWiFi", RTLD_NOW);
		CFTypeRef (*client_create)(CFAllocatorRef, int) =
		    mw ? dlsym(mw, "WiFiManagerClientCreate") : NULL;
		int (*client_power)(CFTypeRef, int) = mw ? dlsym(mw, "WiFiManagerClientSetPower") : NULL;
		CFTypeRef client = NULL;

		fprintf(stderr, "a80211_test: MobileWiFi %p create %p power %p\n", mw, client_create, client_power);
		if (client_create != NULL) {
			client = client_create(kCFAllocatorDefault, 0);
			fprintf(stderr, "a80211_test: client %p\n", client);
		}
		if (client != NULL && client_power != NULL) {
			error = client_power(client, atoi(argv[2]) != 0);
			fprintf(stderr, "a80211_test: WiFiManagerClientSetPower %s -> %d\n", argv[2], error);
		}
		return 0;
	}
	if (argc >= 2 && strcmp(argv[1], "debugplist") == 0) {
		const char *path = "/private/var/Managed Preferences/mobile/com.apple.MobileWiFi.debug.plist";
		if (argc >= 3 && strcmp(argv[2], "off") == 0) {
			fprintf(stderr, "a80211_test: remove %s: %s\n", path,
			    unlink(path) == 0 ? "gone" : strerror(errno));
			return 0;
		}
		const char *body = "<?xml version=\"1.0\"?>\n<plist version=\"1.0\">\n<dict>\n"
		    "\t<key>WiFiManagerLoggingEnabled</key><true/>\n"
		    "</dict>\n</plist>\n";
		FILE *fp = fopen(path, "w");

		fprintf(stderr, "a80211_test: %s: %s\n", path, fp ? "written" : strerror(errno));
		if (fp != NULL) {
			fputs(body, fp);
			fclose(fp);
		}
		return fp == NULL;
	}
	if (argc >= 2 && strcmp(argv[1], "mwclient") == 0) {
		void *mw = dlopen("/System/Library/PrivateFrameworks/MobileWiFi.framework/MobileWiFi", RTLD_NOW);
		CFTypeRef (*client_create)(CFAllocatorRef, int) =
		    mw ? dlsym(mw, "WiFiManagerClientCreate") : NULL;
		CFArrayRef (*copy_devices)(CFTypeRef) = mw ? dlsym(mw, "WiFiManagerClientCopyDevices") : NULL;
		CFDictionaryRef (*dev_network)(CFTypeRef) = mw ? dlsym(mw, "WiFiDeviceClientCopyCurrentNetwork") : NULL;
		CFStringRef (*dev_name)(CFTypeRef) = mw ? dlsym(mw, "WiFiDeviceClientGetInterfaceName") : NULL;
		int (*dev_power)(CFTypeRef) = mw ? dlsym(mw, "WiFiDeviceClientGetPower") : NULL;
		CFTypeRef client = NULL;
		CFArrayRef devices = NULL;

		fprintf(stderr, "a80211_test: client_create %p devices %p network %p name %p power %p\n",
		    client_create, copy_devices, dev_network, dev_name, dev_power);
		if (client_create != NULL) {
			client = client_create(kCFAllocatorDefault, 0);
			fprintf(stderr, "a80211_test: client %p\n", client);
		}
		if (client != NULL && copy_devices != NULL) {
			devices = copy_devices(client);
			fprintf(stderr, "a80211_test: devices %p count %ld\n", devices,
			    devices ? CFArrayGetCount(devices) : -1L);
		}
		if (devices != NULL) {
			for (CFIndex i = 0; i < CFArrayGetCount(devices); i++) {
				CFTypeRef device = (CFTypeRef)CFArrayGetValueAtIndex(devices, i);
				char name[64] = "?";
				CFStringRef n = dev_name ? dev_name(device) : NULL;

				if (n != NULL) {
					CFStringGetCString(n, name, sizeof(name), kCFStringEncodingUTF8);
				}
				fprintf(stderr, "a80211_test: device %ld %s power %d\n", (long)i, name,
				    dev_power ? dev_power(device) : -1);
				if (dev_network != NULL) {
					CFDictionaryRef net = dev_network(device);

					fprintf(stderr, "a80211_test: %s current network %p\n", name, net);
					if (net != NULL) {
						show("network dict", net);
						CFRelease(net);
					}
				}
			}
		}
		return 0;
	}
	if (a80211_open == NULL || a80211_bind == NULL || a80211_get == NULL) {
		return 1;
	}
	error = a80211_open(&handle);
	printf("a80211_test: Apple80211Open -> %d (%p)\n", error, handle);
	if (error == 0) {
		error = a80211_bind(handle, CFSTR("en0"));
		printf("a80211_test: Apple80211BindToInterface en0 -> %d\n", error);
	}
	if (error != 0) {
		return 1;
	}
	if (argc >= 3 && strcmp(argv[1], "power") == 0) {
		printf("a80211_test: set power %s -> %d\n", argv[2], power(atoi(argv[2]) != 0));
		return 0;
	}
	if (argc >= 2 && strcmp(argv[1], "leave") == 0) {
		printf("a80211_test: set disassociate(22) -> %d\n", a80211_set(handle, 22, NULL, NULL, 0));
		return 0;
	}
	if (argc >= 2 && strcmp(argv[1], "scan") == 0) {
		int (*scan)(void *, CFArrayRef *, CFDictionaryRef) = dlsym(io, "Apple80211Scan");
		CFArrayRef list = NULL;

		fprintf(stderr, "a80211_test: Apple80211Scan %p\n", scan);
		if (scan != NULL) {
			CFDictionaryRef params = CFDictionaryCreate(kCFAllocatorDefault, NULL, NULL, 0,
			    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
			error = scan(handle, &list, params);
			printf("a80211_test: scan -> %d, list %p count %ld\n", error, list,
			    list ? CFArrayGetCount(list) : -1L);
			if (list != NULL) {
				for (CFIndex i = 0; i < CFArrayGetCount(list); i++) {
					CFDictionaryRef net = CFArrayGetValueAtIndex(list, i);

					show("net", net);
				}
				CFRelease(list);
			}
		}
		return 0;
	}
	if (a80211_current != NULL) {
		CFDictionaryRef network = NULL;

		error = a80211_current(handle, &network);
		printf("a80211_test: CopyCurrentNetwork -> %d\n", error);
		if (network != NULL) {
			show("current network", network);
			show("SSID_STR", CFDictionaryGetValue(network, CFSTR("SSID_STR")));
			show("SSID", CFDictionaryGetValue(network, CFSTR("SSID")));
			show("BSSID", CFDictionaryGetValue(network, CFSTR("BSSID")));
			CFRelease(network);
		}
	}
	asked(13, 4);                                   /* STATE */
	asked(1, 32);                                   /* SSID */
	asked(9, 6);                                    /* BSSID */
	asked(103, 2272);                               /* CURRENT_NETWORK as a Get */
	if (a80211_close != NULL) {
		a80211_close(handle);
	}
	return 0;
}
