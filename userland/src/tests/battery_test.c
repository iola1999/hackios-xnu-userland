/*
 * The battery as iOS sees it: the kernel's power source (the IOPMPowerSource that powerd reads,
 * iokit/Kernel/AnnibaleBattery.cpp) and what powerd has made of it (the power sources list that
 * the status bar, the lock screen and Settings get through IOPS), side by side with the kernel's
 * own numbers (kern.annibale.battery_*).
 *
 *   battery_test            once
 *   battery_test N S        N times, S seconds apart
 *   battery_test open URL   have SpringBoard open a URL, such as prefs:root=BATTERY_USAGE (Settings >
 *                           Battery): for screenshots of a page that is hard to reach by touches alone
 *                           (needs the entitlement in battery_test.entitlements, which build.sh signs in)
 *   battery_test powerlog   what iOS's power log (aggregated, CurrentPowerlog.PLSQL) has collected: every
 *                           table with rows, how many and over what time; one row of each of the accounting
 *                           tables Settings > Battery reads (the energy per application comes from them)
 *   battery_test publish    publish a type 2 test power source through IOPSCreatePowerSource /
 *                           IOPSSetPowerSourceDetails and read the power source lists back: whether the
 *                           test source appears tells powerd's serving side apart from the kernel
 *                           battery's own chain when the list is empty
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <dlfcn.h>
#include <glob.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

/* IOKit's IOPowerSources.h and IOPSKeys.h, which the SDK leaves out */
extern CFTypeRef IOPSCopyPowerSourcesInfo(void);
extern CFTypeRef IOPSCopyPowerSourcesByType(int type);
extern CFArrayRef IOPSCopyPowerSourcesList(CFTypeRef blob);
extern CFDictionaryRef IOPSGetPowerSourceDescription(CFTypeRef blob, CFTypeRef ps);
extern CFStringRef IOPSGetProvidingPowerSourceType(CFTypeRef snapshot);
extern CFDictionaryRef IOPSCopyExternalPowerAdapterDetails(void);
extern double IOPSGetTimeRemainingEstimate(void);
extern int IOPSGetPercentRemaining(int *percent, bool *charging, bool *full);

/* the publisher SPI, verified against the 23G90 IOKit binary: create hands back an opaque token
   through the out pointer, details is any CFDictionary and travels to powerd as it is */
extern int IOPSCreatePowerSource(void **out);
extern int IOPSSetPowerSourceDetails(void *ps, CFDictionaryRef details);
extern int IOPSReleasePowerSource(void *ps);

/* a value in short: a number, yes or no, a string, or a dictionary of them in braces */
static void
put(char *line, size_t size, CFTypeRef value)
{
	size_t used = strlen(line);
	char text[256] = "";

	if (value == NULL) {
		snprintf(line + used, size - used, "-");
	} else if (CFGetTypeID(value) == CFBooleanGetTypeID()) {
		snprintf(line + used, size - used, "%s", CFBooleanGetValue(value) ? "yes" : "no");
	} else if (CFGetTypeID(value) == CFNumberGetTypeID()) {
		long long n = 0;

		CFNumberGetValue(value, kCFNumberLongLongType, &n);
		snprintf(line + used, size - used, "%lld", n);
	} else if (CFGetTypeID(value) == CFStringGetTypeID()) {
		CFStringGetCString(value, text, sizeof(text), kCFStringEncodingUTF8);
		snprintf(line + used, size - used, "\"%s\"", text);
	} else if (CFGetTypeID(value) == CFDictionaryGetTypeID()) {
		CFIndex count = CFDictionaryGetCount(value);
		const void **keys = calloc(count + 1, sizeof(void *)), **values = calloc(count + 1, sizeof(void *));

		CFDictionaryGetKeysAndValues(value, keys, values);
		snprintf(line + used, size - used, "{");
		for (CFIndex i = 0; i < count; i++) {
			used = strlen(line);
			text[0] = 0;
			if (CFGetTypeID(keys[i]) == CFStringGetTypeID()) {
				CFStringGetCString(keys[i], text, sizeof(text), kCFStringEncodingUTF8);
			}
			snprintf(line + used, size - used, "%s%s=", i ? ", " : "", text);
			put(line, size, values[i]);
		}
		used = strlen(line);
		snprintf(line + used, size - used, "}");
		free(keys);
		free(values);
	} else {
		snprintf(line + used, size - used, "(something else)");
	}
}

static void
show(const char *what, CFTypeRef object)
{
	static char line[16384];

	if (object == NULL) {
		printf("%s: none\n", what);
		return;
	}
	line[0] = 0;
	put(line, sizeof(line), object);
	printf("%s: %s\n", what, line);
}

static int
kernel(const char *name)
{
	int value = -1;
	size_t size = sizeof(value);

	sysctlbyname(name, &value, &size, NULL, 0);
	return value;
}

static void
once(void)
{
	io_service_t service = IOServiceGetMatchingService(0, IOServiceMatching("IOPMPowerSource"));
	CFTypeRef blob = IOPSCopyPowerSourcesInfo();
	CFArrayRef list = blob ? IOPSCopyPowerSourcesList(blob) : NULL;
	CFDictionaryRef adapter = IOPSCopyExternalPowerAdapterDetails();
	int percent = -1;
	bool charging = false, full = false;
	int got = IOPSGetPercentRemaining(&percent, &charging, &full);

	printf("battery_test: the kernel says %d%%, %d mV, %d mA, %d C, battery %d, charger %d, charging %d, from the DSP %d\n",
	    kernel("kern.annibale.battery_soc"), kernel("kern.annibale.battery_voltage_mv"), kernel("kern.annibale.battery_current_ma"),
	    kernel("kern.annibale.battery_temperature_c"), kernel("kern.annibale.battery_present"),
	    kernel("kern.annibale.battery_charger_present"), kernel("kern.annibale.battery_charging"),
	    kernel("kern.annibale.battery_from_dsp"));
	if (service != 0) {
		CFMutableDictionaryRef properties = NULL;
		io_name_t name = "";

		IORegistryEntryGetName(service, name);
		if (IORegistryEntryCreateCFProperties(service, &properties, kCFAllocatorDefault, 0) == KERN_SUCCESS) {
			printf("battery_test: the power source in the kernel is %s\n", name);
			show("battery_test: its properties", properties);
			CFRelease(properties);
		}
		IOObjectRelease(service);
	} else {
		printf("battery_test: there is no IOPMPowerSource in the kernel\n");
	}
	printf("battery_test: IOPSGetPercentRemaining: %d (%#x), %d%%, charging %d, full %d; time remaining %.0f s\n", got, got,
	    percent, charging, full, IOPSGetTimeRemainingEstimate());
	if (blob != NULL) {
		show("battery_test: powered by", IOPSGetProvidingPowerSourceType(blob));
	}
	for (CFIndex i = 0; list != NULL && i < CFArrayGetCount(list); i++) {
		show("battery_test: power source", IOPSGetPowerSourceDescription(blob, CFArrayGetValueAtIndex(list, i)));
	}
	if (list == NULL || CFArrayGetCount(list) == 0) {
		printf("battery_test: powerd lists no power source\n");
	}
	show("battery_test: the power adapter", adapter);
	if (adapter != NULL) CFRelease(adapter);
	if (list != NULL) CFRelease(list);
	if (blob != NULL) CFRelease(blob);
	fflush(stdout);
}

/* the name the published test source carries, so it can be told apart from the kernel's battery */
#define PROBE_NAME "annibale-psprobe"

/* what one blob holds: how many entries, whether the probe source is among them, whether a type
   InternalBattery entry is (that is the kernel's battery made it through powerd's publishing) */
static void
scan(CFTypeRef blob, int *entries, int *probe, int *battery)
{
	CFArrayRef list = blob ? IOPSCopyPowerSourcesList(blob) : NULL;

	*entries = 0;
	*probe = 0;
	*battery = 0;
	for (CFIndex i = 0; list != NULL && i < CFArrayGetCount(list); i++) {
		CFDictionaryRef description = IOPSGetPowerSourceDescription(blob, CFArrayGetValueAtIndex(list, i));
		CFTypeRef name = description ? CFDictionaryGetValue(description, CFSTR("Name")) : NULL;
		CFTypeRef type = description ? CFDictionaryGetValue(description, CFSTR("Type")) : NULL;
		char text[128] = "";

		*entries += 1;
		if (name != NULL && CFGetTypeID(name) == CFStringGetTypeID()) {
			CFStringGetCString(name, text, sizeof(text), kCFStringEncodingUTF8);
			if (strcmp(text, PROBE_NAME) == 0) *probe = 1;
		}
		if (type != NULL && CFGetTypeID(type) == CFStringGetTypeID()) {
			CFStringGetCString(type, text, sizeof(text), kCFStringEncodingUTF8);
			if (strcmp(text, "InternalBattery") == 0) *battery = 1;
		}
	}
	if (list != NULL) CFRelease(list);
}

/* powerd's publishing chain, probed from the client side: publish a type 2 power source of our own
   (Type "UPS" is what powerd's update handler maps to slot type 2) and read the lists back. powerd
   watches the publisher's pid and drops the source when the process exits, so everything happens in
   this one run. The verdict line is what an unattended run is judged by:
     publish-path-works    the test source is in the list: transport, the MIG server and the parsing
                           all work, so an empty list means the kernel battery's own chain broke
     transport-or-serve    the create or the update failed, or a source powerd accepted never appears:
                           the break is in the MIG service or the transport, not in the battery chain */
static int
publish(void)
{
	int entries, probe, battery;
	CFTypeRef blob = IOPSCopyPowerSourcesInfo();

	scan(blob, &entries, &probe, &battery);
	printf("battery_test: probe baseline: %d entries, battery %s\n", entries, battery ? "present" : "ABSENT");
	if (blob != NULL) CFRelease(blob);

	void *ps = NULL;
	int created = IOPSCreatePowerSource(&ps);

	printf("battery_test: probe create: %#x, token %s\n", created, ps ? "yes" : "no");
	if (created != 0 || ps == NULL) {
		printf("battery_test: probe verdict: transport-or-serve (create failed, powerd's MIG service is unreachable or broken)\n");
		return 1;
	}

	int eighty = 88, hundred = 100;
	CFNumberRef current = CFNumberCreate(NULL, kCFNumberIntType, &eighty);
	CFNumberRef maximum = CFNumberCreate(NULL, kCFNumberIntType, &hundred);
	CFMutableDictionaryRef details = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

	CFDictionarySetValue(details, CFSTR("Type"), CFSTR("UPS"));
	CFDictionarySetValue(details, CFSTR("Name"), CFSTR(PROBE_NAME));
	CFDictionarySetValue(details, CFSTR("Transport Type"), CFSTR("UPS"));
	CFDictionarySetValue(details, CFSTR("Power Source State"), CFSTR("AC Power"));
	CFDictionarySetValue(details, CFSTR("Is Charging"), kCFBooleanTrue);
	CFDictionarySetValue(details, CFSTR("Is Present"), kCFBooleanTrue);
	CFDictionarySetValue(details, CFSTR("Current Capacity"), current);
	CFDictionarySetValue(details, CFSTR("Max Capacity"), maximum);

	int updated = IOPSSetPowerSourceDetails(ps, details);

	printf("battery_test: probe update: %#x\n", updated);
	CFRelease(details);
	CFRelease(current);
	CFRelease(maximum);
	if (updated != 0) {
		printf("battery_test: probe verdict: transport-ok-update-rejected (the create reached powerd but the details were refused)\n");
		IOPSReleasePowerSource(ps);
		return 1;
	}

	/* the update is answered after powerd has stored the description, but allow a second for any
	   dispatch settling before reading back */
	sleep(1);

	int type_entries[5] = {0}, type_probe[5] = {0};
	int battery_type3 = 0;

	for (int type = 0; type <= 4; type++) {
		CFTypeRef readback = IOPSCopyPowerSourcesByType(type);
		int unused = 0;

		scan(readback, &type_entries[type], &type_probe[type], type == 3 ? &battery_type3 : &unused);
		if (readback != NULL) CFRelease(readback);
	}
	printf("battery_test: probe readback: entries type0 %d, type1 %d, type2 %d, type3 %d, type4 %d\n", type_entries[0],
	    type_entries[1], type_entries[2], type_entries[3], type_entries[4]);
	printf("battery_test: probe readback: probe in type0 %d, type2 %d, type3 %d; battery in type3 %s\n", type_probe[0],
	    type_probe[2], type_probe[3], battery_type3 ? "present" : "ABSENT");

	CFDictionaryRef adapter = IOPSCopyExternalPowerAdapterDetails();

	show("battery_test: probe adapter", adapter);
	if (adapter != NULL) CFRelease(adapter);

	if (type_probe[3]) {
		printf("battery_test: probe verdict: publish-path-works (transport, serve and parse are fine; "
		    "battery %s in the same list, so the break is in the kernel-battery chain: discovery, snapshot or description write)\n",
		    battery_type3 ? "present — contradiction, recheck" : "ABSENT");
	} else if (type_probe[0]) {
		printf("battery_test: probe verdict: filter-anomaly (the probe is in type0 but not type3, the query filtering itself is suspect)\n");
	} else {
		printf("battery_test: probe verdict: transport-or-serve (powerd accepted the source but no list shows it: "
		    "the MIG reply, the serialization or the array building is broken)\n");
	}
	IOPSReleasePowerSource(ps);
	fflush(stdout);
	return type_probe[3] ? 0 : 1;
}

/* SpringBoardServices' way of opening a URL, as a program with the entitlement may */
static int
open_url(const char *text)
{
	void *sbs = dlopen("/System/Library/PrivateFrameworks/SpringBoardServices.framework/SpringBoardServices", RTLD_NOW);
	int (*open)(CFURLRef, Boolean) = sbs ? (int (*)(CFURLRef, Boolean))dlsym(sbs, "SBSOpenSensitiveURLAndUnlock") : NULL;
	CFURLRef url = CFURLCreateWithBytes(NULL, (const UInt8 *)text, (CFIndex)strlen(text), kCFStringEncodingUTF8, NULL);
	int result;

	if (open == NULL || url == NULL) {
		printf("battery_test: cannot open %s: %s\n", text, open == NULL ? "no SBSOpenSensitiveURLAndUnlock" : "not a URL");
		return 1;
	}
	result = open(url, true);
	printf("battery_test: SpringBoard was asked to open %s: %s\n", text, result ? "it does" : "it REFUSES");
	CFRelease(url);
	return result ? 0 : 1;
}

/* the power log's database, read only (aggregated keeps writing it) */
static int
powerlog(void)
{
	glob_t found;
	sqlite3 *db;
	sqlite3_stmt *tables;

	if (glob("/private/var/containers/Shared/SystemGroup/*/Library/BatteryLife/CurrentPowerlog.PLSQL", 0, NULL, &found) != 0 ||
	    found.gl_pathc == 0) {
		printf("battery_test: no power log database (is aggregated running?)\n");
		return 1;
	}
	printf("battery_test: the power log: %s\n", found.gl_pathv[0]);
	if (sqlite3_open_v2(found.gl_pathv[0], &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
		printf("battery_test: cannot open it: %s\n", sqlite3_errmsg(db));
		return 1;
	}
	sqlite3_busy_timeout(db, 2000);
	if (sqlite3_prepare_v2(db, "SELECT name FROM sqlite_master WHERE type = 'table' ORDER BY name", -1, &tables, NULL) != SQLITE_OK) {
		printf("battery_test: cannot list its tables: %s\n", sqlite3_errmsg(db));
		return 1;
	}
	while (sqlite3_step(tables) == SQLITE_ROW) {
		const char *name = (const char *)sqlite3_column_text(tables, 0);
		char query[512];
		sqlite3_stmt *count;
		long rows = 0;
		double first = 0, last = 0;

		snprintf(query, sizeof(query), "SELECT COUNT(*), MIN(timestamp), MAX(timestamp) FROM \"%s\"", name);
		if (sqlite3_prepare_v2(db, query, -1, &count, NULL) != SQLITE_OK) {
			snprintf(query, sizeof(query), "SELECT COUNT(*), 0, 0 FROM \"%s\"", name);
			if (sqlite3_prepare_v2(db, query, -1, &count, NULL) != SQLITE_OK) {
				continue;
			}
		}
		if (sqlite3_step(count) == SQLITE_ROW) {
			rows = sqlite3_column_int64(count, 0);
			first = sqlite3_column_double(count, 1);
			last = sqlite3_column_double(count, 2);
		}
		sqlite3_finalize(count);
		if (rows == 0) {
			continue;
		}
		printf("battery_test: powerlog %-70s %6ld rows, %.0f s from the first to the last\n", name, rows, last - first);
		if (strncmp(name, "PLAccountingOperator", 20) == 0 || strncmp(name, "PLBatteryAgent_EventBackward_Battery", 36) == 0) {
			sqlite3_stmt *row;

			snprintf(query, sizeof(query), "SELECT * FROM \"%s\" ORDER BY ID DESC LIMIT 1", name);
			if (sqlite3_prepare_v2(db, query, -1, &row, NULL) == SQLITE_OK) {
				if (sqlite3_step(row) == SQLITE_ROW) {
					char line[1024] = "";

					for (int c = 0; c < sqlite3_column_count(row) && strlen(line) < sizeof(line) - 80; c++) {
						const unsigned char *v = sqlite3_column_text(row, c);

						snprintf(line + strlen(line), sizeof(line) - strlen(line), " %s=%.40s", sqlite3_column_name(row, c),
						    v ? (const char *)v : "-");
					}
					printf("battery_test: powerlog   the last row:%s\n", line);
				}
				sqlite3_finalize(row);
			}
		}
	}
	sqlite3_finalize(tables);
	sqlite3_close(db);
	globfree(&found);
	return 0;
}

int
main(int argc, char **argv)
{
	if (argc > 2 && strcmp(argv[1], "open") == 0) {
		return open_url(argv[2]);
	}
	if (argc > 1 && strcmp(argv[1], "powerlog") == 0) {
		return powerlog();
	}
	if (argc > 1 && strcmp(argv[1], "publish") == 0) {
		return publish();
	}
	int times = argc > 1 ? atoi(argv[1]) : 1;
	int seconds = argc > 2 ? atoi(argv[2]) : 10;

	for (int i = 0; i < times; i++) {
		if (i > 0) sleep(seconds);
		once();
	}
	return 0;
}
