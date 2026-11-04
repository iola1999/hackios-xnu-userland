/*
 * wifijoin: joins a Wi-Fi network through wifid, as Settings does, without touching the screen.
 *
 * usage: wifijoin [FILE]     (default /redwood/fw/network: the network's name on the first line,
 *                             its passphrase on the second)
 *
 * MobileWiFi's client calls: wait up to 600 s for wifid to list a device, turn the power on, scan
 * for the name up to 5 times, put the passphrase in the network record and associate, up to 3 times:
 * the first association with a 5 GHz network after boot failed in every round before this tool
 * (docs/progress/2026-11-01-wifi-ath12k-driver.md). wifid then keeps the network among the known
 * ones, so it rejoins by itself until the data volume is rebuilt at the next boot. Needs the
 * entitlements in wifijoin.entitlements. The association callback is not taken as the answer: in
 * kernel-12377-rate8-app1 it reported error -3902 for an association whose handshake completed and
 * whose link came up, so success is wifid's current network being the one asked for.
 *
 * Built into out/ios26-resume/wifijoin/wifijoin, where tools/k90auto/cycle.py packs it from:
 *   xcrun -sdk iphoneos clang -arch arm64e -miphoneos-version-min=26.0 -Os -framework CoreFoundation \
 *       userland/src/wifijoin.c -o out/ios26-resume/wifijoin/wifijoin
 *   codesign --force --sign - --entitlements userland/src/wifijoin.entitlements out/ios26-resume/wifijoin/wifijoin
 */
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

typedef struct __WiFiManagerClient *WiFiManagerClientRef;
typedef struct __WiFiDeviceClient *WiFiDeviceClientRef;
typedef struct __WiFiNetwork *WiFiNetworkRef;

typedef void (*WiFiScanCallback)(WiFiDeviceClientRef, CFArrayRef, int, void *);
typedef void (*WiFiAssociateCallback)(WiFiDeviceClientRef, WiFiNetworkRef, CFDictionaryRef, int, void *);

/* MobileWiFi is a private framework the SDK has no stub for; its functions are looked up at start */
static WiFiManagerClientRef (*WiFiManagerClientCreate)(CFAllocatorRef, int);
static CFArrayRef (*WiFiManagerClientCopyDevices)(WiFiManagerClientRef);
static void (*WiFiManagerClientScheduleWithRunLoop)(WiFiManagerClientRef, CFRunLoopRef, CFStringRef);
static void (*WiFiManagerClientSetPower)(WiFiManagerClientRef, Boolean);
static int (*WiFiDeviceClientScanAsync)(WiFiDeviceClientRef, CFDictionaryRef, WiFiScanCallback, void *);
static int (*WiFiDeviceClientAssociateAsync)(WiFiDeviceClientRef, WiFiNetworkRef, WiFiAssociateCallback, void *);
static CFStringRef (*WiFiNetworkGetSSID)(WiFiNetworkRef);
static WiFiNetworkRef (*WiFiDeviceClientCopyCurrentNetwork)(WiFiDeviceClientRef);
static Boolean (*WiFiNetworkSetPassword)(WiFiNetworkRef, CFStringRef);

static Boolean
load_mobilewifi(void)
{
	void *lib = dlopen("/System/Library/PrivateFrameworks/MobileWiFi.framework/MobileWiFi", RTLD_NOW);

	if (lib == NULL) {
		fprintf(stderr, "wifijoin: %s\n", dlerror());
		return false;
	}
#define LOOKUP(name) if ((*(void **)&name = dlsym(lib, #name)) == NULL) { fprintf(stderr, "wifijoin: no %s\n", #name); return false; }
	LOOKUP(WiFiManagerClientCreate)
	LOOKUP(WiFiManagerClientCopyDevices)
	LOOKUP(WiFiManagerClientScheduleWithRunLoop)
	LOOKUP(WiFiManagerClientSetPower)
	LOOKUP(WiFiDeviceClientScanAsync)
	LOOKUP(WiFiDeviceClientAssociateAsync)
	LOOKUP(WiFiNetworkGetSSID)
	LOOKUP(WiFiDeviceClientCopyCurrentNetwork)
	LOOKUP(WiFiNetworkSetPassword)
#undef LOOKUP
	return true;
}

struct join {
	CFStringRef name;
	WiFiNetworkRef found;
	int error;
	Boolean done;
};

static void
scanned(WiFiDeviceClientRef device, CFArrayRef results, int error, void *context)
{
	struct join *join = context;

	(void)device;
	join->error = error;
	for (CFIndex i = 0; results != NULL && i < CFArrayGetCount(results); i++) {
		WiFiNetworkRef network = (WiFiNetworkRef)CFArrayGetValueAtIndex(results, i);
		CFStringRef ssid = WiFiNetworkGetSSID(network);

		if (ssid != NULL && CFEqual(ssid, join->name)) {
			join->found = (WiFiNetworkRef)CFRetain(network);
			break;
		}
	}
	printf("wifijoin: scan: %ld networks, error %d, %s\n", results ? (long)CFArrayGetCount(results) : 0L, error,
	       join->found ? "the network is among them" : "the network is not among them");
	join->done = true;
	CFRunLoopStop(CFRunLoopGetCurrent());
}

static void
associated(WiFiDeviceClientRef device, WiFiNetworkRef network, CFDictionaryRef info, int error, void *context)
{
	struct join *join = context;

	(void)device;
	(void)network;
	(void)info;
	join->error = error;
	join->done = true;
	CFRunLoopStop(CFRunLoopGetCurrent());
}

/* whether wifid's current network has NAME: the callback's error is not enough (see above) */
static Boolean
joined(WiFiDeviceClientRef device, CFStringRef name)
{
	WiFiNetworkRef current = NULL;
	Boolean same = false;

	for (int i = 0; i < 10 && current == NULL; i++) {
		current = WiFiDeviceClientCopyCurrentNetwork(device);
		if (current == NULL) {
			sleep(1);
		}
	}
	if (current != NULL) {
		same = WiFiNetworkGetSSID(current) != NULL && CFEqual(WiFiNetworkGetSSID(current), name);
		CFRelease(current);
	}
	return same;
}

/* runs the run loop until a callback set join->done or SECONDS passed; whether it was set */
static Boolean
wait_for(struct join *join, double seconds)
{
	join->done = false;
	CFRunLoopRunInMode(kCFRunLoopDefaultMode, seconds, false);
	return join->done;
}

static CFStringRef
read_line(FILE *f)
{
	char text[256];
	size_t n;

	if (fgets(text, sizeof(text), f) == NULL) {
		return NULL;
	}
	n = strcspn(text, "\r\n");
	text[n] = 0;
	return n ? CFStringCreateWithCString(NULL, text, kCFStringEncodingUTF8) : NULL;
}

int
main(int argc, char **argv)
{
	const char *path = argc > 1 ? argv[1] : "/redwood/fw/network";
	FILE *f = fopen(path, "r");
	struct join join = { 0 };
	CFStringRef password;
	WiFiManagerClientRef manager;
	CFArrayRef devices = NULL;
	WiFiDeviceClientRef device;

	setvbuf(stdout, NULL, _IOLBF, 0);
	if (f == NULL) {
		perror(path);
		return 2;
	}
	join.name = read_line(f);
	password = read_line(f);
	fclose(f);
	if (join.name == NULL) {
		fprintf(stderr, "wifijoin: %s has no network name on its first line\n", path);
		return 2;
	}

	if (!load_mobilewifi()) {
		return 1;
	}
	/*
	 * A client lists the devices wifid had when the client was made, and en0 comes up minutes into
	 * the boot (about 200 s in kernel-12377-rate8-app2): a new client every 5 s until one lists it.
	 */
	for (int waited = 0; waited < 600; waited += 5) {
		manager = WiFiManagerClientCreate(kCFAllocatorDefault, 0);
		devices = manager != NULL ? WiFiManagerClientCopyDevices(manager) : NULL;
		if (devices != NULL && CFArrayGetCount(devices) > 0) {
			break;
		}
		if (devices != NULL) {
			CFRelease(devices);
			devices = NULL;
		}
		if (manager != NULL) {
			CFRelease(manager);
			manager = NULL;
		}
		sleep(5);
	}
	if (devices == NULL) {
		fprintf(stderr, "wifijoin: wifid lists no Wi-Fi device after 600 s\n");
		return 1;
	}
	printf("wifijoin: wifid lists %ld Wi-Fi device(s)\n", (long)CFArrayGetCount(devices));
	WiFiManagerClientScheduleWithRunLoop(manager, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
	device = (WiFiDeviceClientRef)CFArrayGetValueAtIndex(devices, 0);
	WiFiManagerClientSetPower(manager, true);

	for (int i = 0; i < 5 && join.found == NULL; i++) {
		const void *key = CFSTR("SSID_STR");
		const void *value = join.name;
		CFDictionaryRef request = CFDictionaryCreate(NULL, &key, &value, 1, &kCFTypeDictionaryKeyCallBacks,
		                                             &kCFTypeDictionaryValueCallBacks);
		int status = WiFiDeviceClientScanAsync(device, request, scanned, &join);

		CFRelease(request);
		if (status != 0) {
			printf("wifijoin: scan request refused (%d)\n", status);
		} else if (!wait_for(&join, 20)) {
			printf("wifijoin: no scan result in 20 s\n");
		}
		if (join.found == NULL) {
			sleep(3);
		}
	}
	if (join.found == NULL) {
		fprintf(stderr, "wifijoin: the network was not heard\n");
		return 1;
	}
	if (password != NULL && !WiFiNetworkSetPassword(join.found, password)) {
		printf("wifijoin: the passphrase was not stored\n");
	}
	for (int attempt = 1; attempt <= 3; attempt++) {
		join.error = -1;
		if (WiFiDeviceClientAssociateAsync(device, join.found, associated, &join) != 0) {
			printf("wifijoin: association %d: request refused\n", attempt);
		} else if (!wait_for(&join, 45)) {
			printf("wifijoin: association %d: no answer in 45 s\n", attempt);
		} else {
			printf("wifijoin: association %d: %s (error %d)\n", attempt, join.error == 0 ? "done" : "FAILED", join.error);
		}
		if (joined(device, join.name)) {
			printf("wifijoin: wifid's current network is the one asked for\n");
			return 0;
		}
		sleep(5);
	}
	return 1;
}
