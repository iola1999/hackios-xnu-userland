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
#include <fcntl.h>
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

/* mwscanjoin: the associate wifid accepts needs a network record of the scan-result
   shape (WiFiDeviceManagerUserForcedAssociation CFEquals it against results[0]),
   so this joins with the record WiFiDeviceClientScanAsync hands back. */
static CFTypeRef mwsj_assoc;            /* WiFiDeviceClientAssociateAsync */
static CFTypeRef mwsj_device;           /* devices[0] */
static char mwsj_ssid[33];              /* the network to join */
static int mwsj_sent;

static void
mwsj_assoc_cb(CFTypeRef client, CFTypeRef a2, CFTypeRef a3, CFTypeRef a4)
{
	fprintf(stderr, "a80211_test: assoc callback: %p %p %p %p\n", client, a2, a3, a4);
	if (a2 != NULL) {
		show("assoc a2", a2);
	}
}

static void
mwsj_scan_cb(CFTypeRef client, CFTypeRef a2, CFTypeRef a3, CFTypeRef a4)
{
	fprintf(stderr, "a80211_test: scan callback: client %p a2 %p a3 %p a4 %p\n", client, a2, a3, a4);
	if (a2 == NULL || CFGetTypeID(a2) != CFArrayGetTypeID()) {
		if (a2 != NULL) {
			show("scan a2", a2);
		}
		return;
	}
	CFArrayRef results = (CFArrayRef)a2;
	void *mw = dlopen("/System/Library/PrivateFrameworks/MobileWiFi.framework/MobileWiFi", RTLD_NOW);
	CFTypeRef (*net_prop)(CFTypeRef, CFStringRef) = mw ? dlsym(mw, "WiFiNetworkGetProperty") : NULL;
	CFTypeRef picked = NULL;

	fprintf(stderr, "a80211_test: scan results %ld\n", CFArrayGetCount(results));
	for (CFIndex i = 0; i < CFArrayGetCount(results); i++) {
		CFTypeRef item = (CFTypeRef)CFArrayGetValueAtIndex(results, i);
		CFTypeRef name = net_prop != NULL ? net_prop(item, CFSTR("SSID_STR")) : NULL;
		char got[64] = { 0 };

		if (name != NULL && CFGetTypeID(name) == CFStringGetTypeID()) {
			CFStringGetCString((CFStringRef)name, got, sizeof(got), kCFStringEncodingUTF8);
		}
		fprintf(stderr, "a80211_test: scan result %ld ssid '%s' item %p\n", (long)i, got, item);
		if (picked == NULL && strcmp(got, mwsj_ssid) == 0) {
			picked = item;
		}
	}
	if (picked != NULL && !mwsj_sent && mwsj_assoc != NULL) {
		void *mw2 = dlopen("/System/Library/PrivateFrameworks/MobileWiFi.framework/MobileWiFi",
		    RTLD_NOW);
		int (*disassoc)(CFTypeRef) = mw2 ? dlsym(mw2, "WiFiDeviceClientDisassociate") : NULL;
		int (*assoc)(CFTypeRef, CFTypeRef, CFDictionaryRef, void *) = (void *)mwsj_assoc;

		mwsj_sent = 1;
		/* the kernel's standing rejoin leaves wifid "currently associated",
		   which short-circuits its forced-association path before any scan */
		if (disassoc != NULL) {
			int d = disassoc(mwsj_device);

			fprintf(stderr, "a80211_test: WiFiDeviceClientDisassociate -> %d\n", d);
			CFRunLoopRunInMode(kCFRunLoopDefaultMode, 3, false);
		}
		int error = assoc(mwsj_device, picked, NULL, mwsj_assoc_cb);
		fprintf(stderr, "a80211_test: WiFiDeviceClientAssociateAsync(scan-result %p) -> %d\n",
		    picked, error);
	} else if (picked == NULL) {
		fprintf(stderr, "a80211_test: %s not in this scan\n", mwsj_ssid);
	}
}

static int
mwscanjoin(const uint8_t *ssid, unsigned ssid_len, const uint8_t *passphrase,
    unsigned passphrase_len, const uint8_t pmk[32])
{
	void *mw = dlopen("/System/Library/PrivateFrameworks/MobileWiFi.framework/MobileWiFi", RTLD_NOW);
	CFTypeRef (*client_create)(CFAllocatorRef, int) =
	    mw ? dlsym(mw, "WiFiManagerClientCreate") : NULL;
	CFArrayRef (*copy_devices)(CFTypeRef) = mw ? dlsym(mw, "WiFiManagerClientCopyDevices") : NULL;
	CFTypeRef (*net_ssid)(CFStringRef) = mw ? dlsym(mw, "WiFiNetworkCreateWithSsid") : NULL;
	void (*net_set)(CFTypeRef, CFStringRef, CFTypeRef) =
	    mw ? dlsym(mw, "WiFiNetworkSetProperty") : NULL;
	int (*net_pass)(CFTypeRef, CFStringRef) = mw ? dlsym(mw, "WiFiNetworkSetPassword") : NULL;
	void (*net_wpa)(CFTypeRef, int) = mw ? dlsym(mw, "WiFiNetworkSetWPA") : NULL;
	int (*add_net)(CFTypeRef, CFTypeRef) = mw ? dlsym(mw, "WiFiManagerClientAddNetwork") : NULL;
	int (*scan)(CFTypeRef, CFDictionaryRef, void *, void *) =
	    mw ? dlsym(mw, "WiFiDeviceClientScanAsync") : NULL;
	void (*schedule)(CFTypeRef, CFRunLoopRef, CFStringRef) =
	    mw ? dlsym(mw, "WiFiManagerClientScheduleWithRunLoop") : NULL;
	mwsj_assoc = mw ? dlsym(mw, "WiFiDeviceClientAssociateAsync") : NULL;
	char pass[65] = { 0 };

	if (mw == NULL || client_create == NULL || copy_devices == NULL || net_ssid == NULL ||
	    add_net == NULL || scan == NULL || mwsj_assoc == NULL) {
		fprintf(stderr, "a80211_test: mwxjoin: missing pieces: mw %p create %p devices %p "
		    "ssid %p add %p scan %p assoc %p\n", mw, client_create, copy_devices, net_ssid,
		    add_net, scan, mwsj_assoc);
		return 1;
	}
	memcpy(mwsj_ssid, ssid, ssid_len);
	if (passphrase_len != 0) {
		memcpy(pass, passphrase, passphrase_len < 64 ? passphrase_len : 64);
	} else {
		static const char hex[] = "0123456789abcdef";
		for (unsigned i = 0; i < 32; i++) {
			pass[i * 2] = hex[pmk[i] >> 4];
			pass[i * 2 + 1] = hex[pmk[i] & 15];
		}
	}
	CFTypeRef client = client_create(kCFAllocatorDefault, 0);
	/* the client's XPC replies ride this runloop; without scheduling, async
	   callbacks (scan results, association events) never reach us */
	if (schedule != NULL && client != NULL) {
		schedule(client, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
	}
	CFStringRef s = CFStringCreateWithCString(kCFAllocatorDefault, mwsj_ssid,
	    kCFStringEncodingUTF8);
	CFTypeRef net = net_ssid ? net_ssid(s) : NULL;

	if (net != NULL && pass[0] != '\0') {
		CFStringRef secret = CFStringCreateWithCString(kCFAllocatorDefault, pass,
		    kCFStringEncodingUTF8);

		if (net_set != NULL) {
			net_set(net, CFSTR("isWPA"), kCFBooleanTrue);
			net_set(net, CFSTR("password"), secret);
		}
		if (net_wpa != NULL) {
			net_wpa(net, 1);
		}
		if (net_pass != NULL) {
			fprintf(stderr, "a80211_test: WiFiNetworkSetPassword -> %d\n",
			    net_pass(net, secret));
		}
	}
	if (net != NULL) {
		fprintf(stderr, "a80211_test: WiFiManagerClientAddNetwork -> %d\n",
		    add_net(client, net));
	}
	CFArrayRef devices = copy_devices(client);
	if (devices == NULL || CFArrayGetCount(devices) == 0) {
		fprintf(stderr, "a80211_test: mwxjoin: no devices\n");
		return 1;
	}
	mwsj_device = CFArrayGetValueAtIndex(devices, 0);
	CFDictionaryRef params = CFDictionaryCreate(kCFAllocatorDefault, NULL, NULL, 0,
	    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	int error = scan(mwsj_device, params, mwsj_scan_cb, NULL);
	fprintf(stderr, "a80211_test: WiFiDeviceClientScanAsync -> %d\n", error);
	/* MobileWiFi delivers client callbacks on the runloop; sleeping here would
	   drop them, and the session dies with this client anyway */
	CFRunLoopRunInMode(kCFRunLoopDefaultMode, 40, false);
	fprintf(stderr, "a80211_test: mwxjoin done, assoc sent %d\n", mwsj_sent);
	return 0;
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
	if (argc >= 2 && (strcmp(argv[1], "mwjoin") == 0 || strcmp(argv[1], "mwxjoin") == 0)) {
		/* registers the standing network with wifid the way a join in Settings
		   does: WiFiManagerClientAddNetwork puts it on the known list (and its
		   passphrase in the keychain), WiFiDeviceClientAssociateAsync has wifid
		   associate itself so its own association state - what SpringBoard's
		   icon reads - reflects the link */
		struct {
			uint8_t ssid[32], ssid_length, bssid[6];
			int security;
			uint8_t pmk[32], passphrase[64], passphrase_length;
		} saved;
		int fd = open("/private/var/db/wifi.standing", O_RDONLY);
		ssize_t got = fd < 0 ? -1 : read(fd, &saved, sizeof(saved));

		if (fd >= 0) {
			close(fd);
		}
		fprintf(stderr, "a80211_test: standing read %zd ssid_length %u passphrase_length %u\n",
		    got, got > 0 ? saved.ssid_length : 0, got > 0 ? saved.passphrase_length : 0);
		if (got < (ssize_t)sizeof(saved) || saved.ssid_length == 0 || saved.ssid_length > 32) {
			return 1;
		}
		if (strcmp(argv[1], "mwxjoin") == 0) {
			return mwscanjoin(saved.ssid, saved.ssid_length, saved.passphrase,
			    saved.passphrase_length, saved.pmk);
		}
		void *mw = dlopen("/System/Library/PrivateFrameworks/MobileWiFi.framework/MobileWiFi", RTLD_NOW);
		CFTypeRef (*client_create)(CFAllocatorRef, int) =
		    mw ? dlsym(mw, "WiFiManagerClientCreate") : NULL;
		CFArrayRef (*copy_devices)(CFTypeRef) = mw ? dlsym(mw, "WiFiManagerClientCopyDevices") : NULL;
		CFTypeRef (*net_ssid)(CFStringRef) = mw ? dlsym(mw, "WiFiNetworkCreateWithSsid") : NULL;
		void (*net_set)(CFTypeRef, CFStringRef, CFTypeRef) =
		    mw ? dlsym(mw, "WiFiNetworkSetProperty") : NULL;
		int (*net_pass)(CFTypeRef, CFStringRef) = mw ? dlsym(mw, "WiFiNetworkSetPassword") : NULL;
		void (*net_wpa)(CFTypeRef, int) = mw ? dlsym(mw, "WiFiNetworkSetWPA") : NULL;
		int (*add_net)(CFTypeRef, CFTypeRef) = mw ? dlsym(mw, "WiFiManagerClientAddNetwork") : NULL;
		int (*assoc)(CFTypeRef, CFTypeRef, CFDictionaryRef, void *) =
		    mw ? dlsym(mw, "WiFiDeviceClientAssociateAsync") : NULL;
		CFArrayRef (*copy_nets)(CFTypeRef) = mw ? dlsym(mw, "WiFiManagerClientCopyMutableNetworks") : NULL;
		CFTypeRef client = NULL, net = NULL;
		CFArrayRef devices = NULL;
		char ssid[33] = { 0 }, pass[65] = { 0 };

		fprintf(stderr, "a80211_test: create %p devices %p net_ssid %p set %p add %p assoc %p\n",
		    client_create, copy_devices, net_ssid, net_set, add_net, assoc);
		memcpy(ssid, saved.ssid, saved.ssid_length);
		if (saved.passphrase_length != 0) {
			memcpy(pass, saved.passphrase,
			    saved.passphrase_length < 64 ? saved.passphrase_length : 64);
		} else {
			static const char hex[] = "0123456789abcdef";
			for (unsigned i = 0; i < sizeof(saved.pmk); i++) {
				pass[i * 2] = hex[saved.pmk[i] >> 4];
				pass[i * 2 + 1] = hex[saved.pmk[i] & 15];
			}
			/* a 64-digit key reaches the driver as the PMK itself (cipher 6), not
			   the passphrase it would come from */
		}
		if (client_create != NULL) {
			client = client_create(kCFAllocatorDefault, 0);
		}
		if (client != NULL && net_ssid != NULL) {
			net = net_ssid(CFStringCreateWithCString(kCFAllocatorDefault, ssid,
			    kCFStringEncodingUTF8));
			fprintf(stderr, "a80211_test: network %p ssid %s\n", net, ssid);
		}
		if (net != NULL && net_set != NULL && pass[0] != '\0') {
			CFStringRef secret =
			    CFStringCreateWithCString(kCFAllocatorDefault, pass, kCFStringEncodingUTF8);

			net_set(net, CFSTR("isWPA"), kCFBooleanTrue);
			net_set(net, CFSTR("password"), secret);
			if (net_wpa != NULL) {
				net_wpa(net, 1);
			}
			if (net_pass != NULL) {
				error = net_pass(net, secret);
				fprintf(stderr, "a80211_test: WiFiNetworkSetPassword -> %d\n", error);
			}
		}
		if (net != NULL && add_net != NULL) {
			error = add_net(client, net);
			fprintf(stderr, "a80211_test: WiFiManagerClientAddNetwork -> %d (%#x)\n", error, error);
		}
		if (copy_nets != NULL && client != NULL) {
			CFArrayRef nets = copy_nets(client);

			fprintf(stderr, "a80211_test: mutable networks %p count %ld\n", nets,
			    nets ? CFArrayGetCount(nets) : -1L);
			if (nets != NULL) {
				for (CFIndex i = 0; i < CFArrayGetCount(nets); i++) {
					show("known", CFArrayGetValueAtIndex(nets, i));
				}
				CFRelease(nets);
			}
		}
		if (client != NULL && copy_devices != NULL) {
			devices = copy_devices(client);
		}
		CFArrayRef nets2 = copy_nets != NULL && client != NULL ? copy_nets(client) : NULL;
		CFTypeRef stored = nets2 != NULL && CFArrayGetCount(nets2) > 0 ?
		    CFArrayGetValueAtIndex(nets2, 0) : net;
		if (assoc != NULL && devices != NULL && CFArrayGetCount(devices) > 0 && stored != NULL) {
			const void *k[] = { CFSTR("password") };
			const void *v[] = { CFStringCreateWithCString(kCFAllocatorDefault, pass,
			    kCFStringEncodingUTF8) };
			CFDictionaryRef opts = CFDictionaryCreate(kCFAllocatorDefault, k, v, 1,
			    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

			error = assoc((CFTypeRef)CFArrayGetValueAtIndex(devices, 0), stored, opts, NULL);
			fprintf(stderr, "a80211_test: WiFiDeviceClientAssociateAsync(stored %p) -> %d\n",
			    stored, error);
			/* the associate is async and dies with this client: stay around
			   so wifid can finish scanning and issue the join */
			for (int i = 0; i < 30; i++) {
				sleep(1);
			}
			fprintf(stderr, "a80211_test: mwjoin done\n");
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
