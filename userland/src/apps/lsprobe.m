/*
 * lsprobe: what the two interfaces a home screen asks for its applications return on this boot.
 *
 * SpringBoard builds its icon model from the application list LaunchServices keeps, and pluginkit
 * fills its extension records from the same place, so a desktop with two icons and no .appex host
 * can be either "LaunchServices knows two applications" or "LaunchServices knows all of them and
 * nothing ever appears". The logs cannot tell those apart: with the console forwarding at its
 * default level, SpringBoard says six lines in a whole boot. This program asks directly.
 *
 * usage: lsprobe [SECONDS ...]      (each argument: print the answer that many seconds from start)
 *
 * It runs as one more line of /etc/rc.jobs, ends with "&" there so the gesture script keeps its own
 * timing, and prints one line per fact so a console capture can be grepped:
 *
 *   lsprobe t=60 workspace=15 mobileinstallation=15 files_applications=64 ...
 *
 * Nothing here changes the system: the two interfaces are called with their own default arguments,
 * and the file system is only read.
 */
#import <Foundation/Foundation.h>
#include <dirent.h>
#include <dlfcn.h>
#include <mach/mach.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* libproc and the bootstrap lookup live in libSystem, but the iPhoneOS SDK ships neither header.
 * The declarations below are the shipped ABI: proc_listpids(PROC_ALL_PIDS, 0, buffer, bytes),
 * proc_name returns the length of the 16-byte p_comm, bootstrap_look_up2 as it is used by every
 * XPC client on the platform. */
extern int proc_listpids(unsigned type, unsigned typeinfo, void *buffer, int buffersize);
extern int proc_name(int pid, char *buffer, unsigned buffersize);
extern int proc_pidpath(int pid, void *buffer, unsigned int buffersize);
/* CPU time per process. Two samples of this tell apart a process that is spinning from one that is
 * waiting: on 2026-10-28 the screen froze at 09:23:16.7 while touch still reached backboardd, and
 * "SpringBoard is burning CPU retrying" and "SpringBoard is blocked" need different fixes. */
extern int proc_pid_rusage(int pid, int flavor, void *info);
#define RUSAGE_INFO_V4 4
extern mach_port_t bootstrap_port;
extern kern_return_t bootstrap_look_up2(mach_port_t bp, const char *service_name, mach_port_t *sp,
                                        int pids, unsigned long long flags);

/* say() is plain vsnprintf, which has no %@: an object argument there comes out as a bare "@", so
 * every id is turned into its UTF-8 bytes before it reaches the format. */
static const char *
utf8(id object)
{
	const char *bytes = object == nil ? NULL : [object UTF8String];

	return bytes == NULL ? "" : bytes;
}

/* The console line every fact goes through: one write, so lines from the several probes in a boot
 * do not interleave in the middle of a number. */
static void
say(const char *format, ...)
{
	char line[4096];
	va_list args;
	int n;

	va_start(args, format);
	n = vsnprintf(line, sizeof(line) - 1, format, args);
	va_end(args);
	if (n <= 0) {
		return;
	}
	if (n > (int)sizeof(line) - 2) {
		n = (int)sizeof(line) - 2;
	}
	line[n++] = '\n';
	fwrite(line, 1, (size_t)n, stdout);
	/* Without this the console keeps whole lines inside stdio's buffer until it fills or the
	 * program exits, so a probe that blocks for one call looks exactly like a probe that is slow,
	 * and the line before the block never reaches a snapshot. Two rounds of device time were lost
	 * to that ambiguity. Flushing per line costs one write per line and removes it. */
	fflush(stdout);
}

/* How many entries a directory has, -1 when it cannot be read: /private/var is a memory disk rebuilt
 * from etc/var.skel at every boot, so "the directory exists and is empty" is a different answer from
 * "it does not exist", and both are worth printing. */
static long
count_entries(const char *path)
{
	DIR *dir = opendir(path);
	long count = 0;

	if (dir == NULL) {
		return -1;
	}
	while (readdir(dir) != NULL) {
		count++;
	}
	closedir(dir);
	return count > 2 ? count - 2 : 0;      /* minus "." and ".." */
}

static int
exists(const char *path)
{
	struct stat st;

	return stat(path, &st) == 0;
}

/* The extension records a LaunchServices application proxy carries. The selector has been called
 * appExtensionPlugins, plugInKitPlugins and extensionPlugins across releases; asking for each in
 * turn costs nothing and an absent one just returns NO from respondsToSelector. */
static unsigned long
plugins_of(id proxy)
{
	static const char *const names[] = {"appExtensionPlugins", "plugInKitPlugins",
	                                    "extensionPlugins", "plugins"};
	unsigned long total = 0;

	for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		SEL selector = NSSelectorFromString([NSString stringWithUTF8String:names[i]]);

		if (![proxy respondsToSelector:selector]) {
			continue;
		}
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Warc-performSelector-leaks"
		id value = [proxy performSelector:selector];
#pragma clang diagnostic pop
		if ([value isKindOfClass:[NSArray class]]) {
			total += (unsigned long)[(NSArray *)value count];
		} else if ([value isKindOfClass:[NSSet class]]) {
			total += (unsigned long)[(NSSet *)value count];
		}
	}
	return total;
}

/* LSApplicationWorkspace allApplications: the list SpringBoard's icon model is built from. */
static void
probe_workspace(unsigned long seconds)
{
	Class workspaceClass = NSClassFromString(@"LSApplicationWorkspace");

	if (workspaceClass == Nil) {
		say("lsprobe t=%lu workspace=class-missing", seconds);
		return;
	}
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Warc-performSelector-leaks"
	id workspace = [workspaceClass performSelector:NSSelectorFromString(@"defaultWorkspace")];
	if (workspace == nil) {
		say("lsprobe t=%lu workspace=instance-missing", seconds);
		return;
	}
	unsigned long extensions = 0;
	NSMutableArray *identifiers = [NSMutableArray array];
	id applications = [workspace performSelector:NSSelectorFromString(@"allApplications")];
#pragma clang diagnostic pop

	if (![applications isKindOfClass:[NSArray class]]) {
		say("lsprobe t=%lu workspace=not-an-array", seconds);
		return;
	}
	for (id proxy in (NSArray *)applications) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Warc-performSelector-leaks"
		id identifier = [proxy performSelector:NSSelectorFromString(@"bundleIdentifier")];
#pragma clang diagnostic pop
		unsigned long plugins = plugins_of(proxy);

		extensions += plugins;
		[identifiers addObject:[NSString stringWithFormat:@"%@/%lu",
		                        [identifier isKindOfClass:[NSString class]] ? identifier : @"?",
		                        plugins]];
	}
	say("lsprobe t=%lu workspace=%lu workspace_extensions=%lu", seconds,
	    (unsigned long)[(NSArray *)applications count], extensions);
	say("lsprobe t=%lu bundles=%s", seconds, utf8([identifiers componentsJoinedByString:@" "]));
}

/* The MobileInstallation interface installd answers. MobileInstallationLookup is gone in iOS 26:
 * the shared cache of 23G90 exports 23 names beginning MobileInstallation but only
 * MobileInstallationLookupUninstalled and MobileInstallationLookupSystemAppState among them are
 * lookups (dyld_shared_cache_arm64e.35.dyldlinkedit:2014988-2014989). So the probe asks for the
 * names in order and prints which one answered; a count of installed applications comes from
 * LSApplicationWorkspace, which this same run prints as workspace=. */
static const char *const MI_LOOKUPS[] = {
	"MobileInstallationLookupUninstalled",
	"MobileInstallationLookupSystemAppState",
	"MobileInstallationLookup",
};

static void
probe_mobile_installation(unsigned long seconds)
{
	void *framework = dlopen("/System/Library/PrivateFrameworks/MobileInstallation.framework/MobileInstallation",
	                         RTLD_LAZY);
	CFArrayRef (*lookup)(CFDictionaryRef, CFErrorRef *);
	CFArrayRef result;
	const char *name = NULL;

	if (framework == NULL) {
		say("lsprobe t=%lu mobileinstallation=framework-missing", seconds);
		return;
	}
	for (unsigned i = 0; i < sizeof(MI_LOOKUPS) / sizeof(MI_LOOKUPS[0]); i++) {
		*(void **)(&lookup) = dlsym(framework, MI_LOOKUPS[i]);
		if (lookup != NULL) {
			name = MI_LOOKUPS[i];
			break;
		}
	}
	if (lookup == NULL) {
		say("lsprobe t=%lu mobileinstallation=symbol-missing", seconds);
		dlclose(framework);
		return;
	}
	/* A NULL options dictionary asks for every application this process can see. */
	result = lookup(NULL, NULL);
	if (result == NULL) {
		say("lsprobe t=%lu mobileinstallation=no-answer", seconds);
	} else if (CFGetTypeID(result) != CFArrayGetTypeID()) {
		say("lsprobe t=%lu mobileinstallation=not-an-array", seconds);
		CFRelease(result);
	} else {
		CFIndex count = CFArrayGetCount(result);

		say("lsprobe t=%lu mobileinstallation=%s count=%ld", seconds, name, (long)count);
		CFRelease(result);
	}
	dlclose(framework);
}

/* How many application containers hold a PosterBoard data store, and the first one's name.
 *
 * The wallpaper store is not at /private/var/mobile/Library/PosterBoard: PosterBoard is an
 * application, so its data lives under its own container. The store path inside the container is
 * Library/Application Support/PBPosterExtensionDataSource (the layout described for iOS 16;
 * 2026-10-28 the shipped iOS 26 image has no Library/PosterBoard directory anywhere, so the older
 * probe path could only ever answer -1). Counting hits answers two questions at once: whether any
 * container has a store, and whether the probe was looking in the wrong place.
 */
static long
poster_stores(const char *tail, char *first, size_t first_size)
{
	const char *root = "/private/var/mobile/Containers/Data/Application";
	struct dirent *entry;
	DIR *dir = opendir(root);
	long hits = 0;

	first[0] = '\0';
	if (dir == NULL) {
		return -1;
	}
	while ((entry = readdir(dir)) != NULL) {
		char path[1200];
		struct stat info;

		if (entry->d_name[0] == '.') {
			continue;
		}
		snprintf(path, sizeof(path), "%s/%s%s", root, entry->d_name, tail);
		if (stat(path, &info) == 0 && S_ISDIR(info.st_mode)) {
			if (hits == 0) {
				snprintf(first, first_size, "%s", entry->d_name);
			}
			hits++;
		}
	}
	closedir(dir);
	return hits;
}

/* How long ago a file was written, in seconds, or -1 when it is not there. /private/var is a ramfs
 * rebuilt at every boot, so this age is time since this boot wrote the file: the probe at t=60 with
 * an age of 12 says it was written at t=48, which can be lined up against the log without carrying a
 * clock between the two. Who wrote it is not observable from here. */
static long
written_age(const char *path)
{
	struct stat info;

	if (stat(path, &info) != 0) {
		return -1;
	}
	return (long)(time(NULL) - info.st_mtimespec.tv_sec);
}

/* The two places the answers are written down, and the layout file the icon model would be read
 * from if a previous boot had saved one. */
static void
probe_files(unsigned long seconds)
{
	say("lsprobe t=%lu files_applications=%ld files_staged=%ld mobileinstallation_dir=%ld "
	    "iconstate=%d icondomain=%d posterboard_dir=%ld posterboard_db=%d pluginkit_dir=%ld "
	    "containers_system=%ld",
	    seconds,
	    count_entries("/Applications"),
	    count_entries("/private/var/staged_system_apps"),
	    count_entries("/private/var/mobile/Library/MobileInstallation"),
	    exists("/private/var/mobile/Library/SpringBoard/IconState.plist"),
	    exists("/private/var/mobile/Library/SpringBoard"),
	    count_entries("/private/var/mobile/Library/PosterBoard"),
	    exists("/private/var/mobile/Library/PosterBoard/PosterBoard.sqlite"),
	    count_entries("/private/var/mobile/Library/PlugInKit"),
	    count_entries("/private/var/containers/Data/Application"));

	/* Ages beside the two existence answers, so a round says when the file appeared and not only
	 * that it is there. */
	char store[64];
	/* Two names, because the store was renamed: iOS 16 used PBPosterExtensionDataSource and the
	 * iOS 26 shared cache (23G90, dyld_shared_cache_arm64e.42) carries PRBPosterExtensionDataStore
	 * and not the older string at all. A count of 0 under one name says nothing about the other. */
	long older = poster_stores("/Library/Application Support/PBPosterExtensionDataSource",
	                           store, sizeof(store));
	long stores = poster_stores("/Library/Application Support/PRBPosterExtensionDataStore",
	                            store, sizeof(store));

	say("lsprobe t=%lu poster_stores=%ld first=%s poster_stores_ios16=%ld", seconds, stores,
	    stores > 0 ? store : "-", older);
	say("lsprobe t=%lu iconstate_age=%ld icondomain_age=%ld",
	    seconds,
	    written_age("/private/var/mobile/Library/SpringBoard/IconState.plist"),
	    written_age("/private/var/mobile/Library/SpringBoard"));
}

/* The processes whose presence and pid this boot depends on, and whose pid is how a restart shows.
 * proc_name returns p_comm, which the kernel cuts at 16 bytes, so the comparison is a prefix. */
static const char *const WATCHED[] = {
	"PosterBoard", "SpringBoard", "backboardd", "chronod", "extensionkitse", "iconservicesage",
	"pkd", "installd", "containermanag", "lsd", "iconservices", "replicatord",
	/* dasd and appprotectiond are the two the rounds after this one register one at a time: whether
	 * the daemon is running is the reading that says the job took effect, and the Code=4099 count
	 * alone cannot tell "the service answered" from "the client stopped asking". */
	"dasd", "appprotectiond",
};

/* User and system CPU milliseconds for the watched processes, read from proc_pid_rusage.
 * The struct's user and system time are the first two fields of every rusage_info version, so a
 * call with a large enough buffer reads them whatever the version the kernel fills in. */
static void
cpu_times(unsigned long seconds)
{
	char line[4096];
	size_t used = 0;
	int written;

	written = snprintf(line, sizeof(line), "lsprobe t=%lu cpu procs=", seconds);
	if (written > 0) {
		used = (size_t)written;
	}
	for (unsigned i = 0; i < sizeof(WATCHED) / sizeof(WATCHED[0]) && used < sizeof(line); i++) {
		char comm[2 * MAXPATHLEN];
		char buffer[2048];
		int bytes = proc_listpids(1, 0, buffer, (int)sizeof(buffer));
		pid_t pid = 0;
		long long user_ms, system_ms;

		for (int offset = 0; offset + (int)sizeof(int) <= bytes; offset += (int)sizeof(int)) {
			int candidate = 0;
			int length;

			memcpy(&candidate, buffer + offset, sizeof(candidate));
			if (candidate <= 0) {
				continue;
			}
			length = proc_name(candidate, comm, (unsigned)sizeof(comm));
			if (length > 0 && strncmp(comm, WATCHED[i], strlen(WATCHED[i])) == 0) {
				pid = candidate;
				break;
			}
		}
		if (pid == 0) {
			continue;
		}
		{
			/* rusage_info_v0 begins with a 16-byte uuid, then ri_user_time and ri_system_time;
			 * every later version keeps that prefix and appends fields. */
			struct {
				uint8_t uuid[16];
				uint64_t user_time;
				uint64_t system_time;
				uint64_t rest[30];
			} info;
			int status;

			memset(&info, 0, sizeof(info));
			status = proc_pid_rusage(pid, RUSAGE_INFO_V4, &info);
			if (status != 0) {
				continue;
			}
			user_ms = (long long)(info.user_time / 1000000ULL);
			system_ms = (long long)(info.system_time / 1000000ULL);
		}
		written = snprintf(line + used, sizeof(line) - used, " %s:%d:%lld/%lld", WATCHED[i], pid,
		                   user_ms, system_ms);
		if (written > 0) {
			used += (size_t)written;
		}
	}
	say("%s", line);
}

static void
probe_processes(unsigned long seconds)
{
	char buffer[8192];
	int bytes = proc_listpids(1 /* PROC_ALL_PIDS */, 0, buffer, (int)sizeof(buffer));
	NSMutableString *line = [NSMutableString string];

	if (bytes <= 0) {
		say("lsprobe t=%lu procs=list-failed", seconds);
		return;
	}
	for (unsigned i = 0; i < sizeof(WATCHED) / sizeof(WATCHED[0]); i++) {
		int found = 0;

		for (int offset = 0; offset + (int)sizeof(int) <= bytes; offset += (int)sizeof(int)) {
			int pid = 0;
			char comm[2 * MAXPATHLEN];
			int length;

			memcpy(&pid, buffer + offset, sizeof(pid));
			if (pid <= 0) {
				continue;
			}
			length = proc_name(pid, comm, (unsigned)sizeof(comm));
			if (length <= 0 || (size_t)length < strlen(WATCHED[i])) {
				continue;
			}
			if (strncmp(comm, WATCHED[i], strlen(WATCHED[i])) == 0) {
				[line appendFormat:@"%s=%d ", WATCHED[i], pid];
				found = 1;
				break;
			}
		}
		if (!found) {
			[line appendFormat:@"%s=- ", WATCHED[i]];
		}
	}
	say("lsprobe t=%lu procs=%s", seconds,
	    utf8([line stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]]));
	cpu_times(seconds);
}

/* Whether anything answers on the wallpaper service. PosterBoard's job sets RunAtLoad, so launchd
 * starts it at load regardless and this lookup cannot change the boot: it either reaches the
 * instance or reports that nothing registered the name. */
/* The whole process table once, with executable paths. The named scan above answers "did this come
 * up"; this answers "what did come up", which is how a service that was supposed to start and did
 * not becomes visible without knowing its name in advance. Paths separate the two processes that
 * share a comm name (the framework's support binary under its own path, an .appex host under
 * PlugIns). Printed at the later offset only: at t=60 the table is still growing. */
static void
probe_table(unsigned long seconds)
{
	char buffer[65536];
	int bytes = proc_listpids(1 /* PROC_ALL_PIDS */, 0, buffer, (int)sizeof(buffer));
	int count = 0;

	if (bytes <= 0) {
		say("lsprobe t=%lu table=list-failed", seconds);
		return;
	}
	for (int offset = 0; offset + (int)sizeof(int) <= bytes; offset += (int)sizeof(int)) {
		int pid = 0;
		char path[2 * MAXPATHLEN];
		char comm[2 * MAXPATHLEN];

		memcpy(&pid, buffer + offset, sizeof(pid));
		if (pid <= 0 || proc_pidpath(pid, path, (unsigned)sizeof(path)) <= 0) {
			continue;
		}
		if (proc_name(pid, comm, (unsigned)sizeof(comm)) <= 0) {
			strlcpy(comm, "?", sizeof(comm));
		}
		say("lsprobe t=%lu pid=%d name=%s path=%s", seconds, pid, comm, path);
		count++;
	}
	say("lsprobe t=%lu table=%d", seconds, count);
}

static void
probe_poster_service(unsigned long seconds)
{
	mach_port_t port = MACH_PORT_NULL;
	kern_return_t kr = bootstrap_look_up2(bootstrap_port, "com.apple.posterboardservices.services",
	                                      &port, 0, 0);

	say("lsprobe t=%lu poster_service=%s kr=%d", seconds,
	    kr == KERN_SUCCESS ? "present" : "absent", (int)kr);
	if (kr == KERN_SUCCESS) {
		mach_port_deallocate(mach_task_self(), port);
	}
}

/* What ReportCrash wrote. The memory disk holds only this boot, so the directory being empty and
 * the directory holding a crash are both answers. Names first; the newest two also get their opening
 * bytes, because the reason a host exited is in the first lines of the report and the console ring
 * keeps about a megabyte, which is enough for two reports and not for twenty. */
static int
name_compare(const void *left, const void *right)
{
	return strcmp(*(const char *const *)left, *(const char *const *)right);
}

static void
probe_crashes(unsigned long seconds)
{
	static const char *const DIRS[] = {
		"/private/var/mobile/Library/Logs/CrashReporter",
		"/private/var/root/Library/Logs/CrashReporter",
	};
	NSMutableString *names = [NSMutableString string];
	unsigned long total = 0;

	for (unsigned d = 0; d < sizeof(DIRS) / sizeof(DIRS[0]); d++) {
		static const char *const LABELS[] = {"mobile", "root"};
		DIR *dir = opendir(DIRS[d]);
		struct dirent *entry;
		const char *list[32];
		int count = 0, shown = 0;

		if (dir == NULL) {
			[names appendFormat:@"%s=- ", LABELS[d]];
			continue;
		}
		while ((entry = readdir(dir)) != NULL && count < 32) {
			char child[1024];
			struct stat info;

			if (entry->d_name[0] == '.') {
				continue;
			}
			snprintf(child, sizeof(child), "%s/%s", DIRS[d], entry->d_name);
			/* ReportCrash writes into DiagnosticLogs beside the directory it also uses for older
			 * reports, so a listing of the top level alone says "1 file" when the answer is "one
			 * subdirectory holding n reports". One level down is where the names carry the process. */
			if (stat(child, &info) == 0 && S_ISDIR(info.st_mode)) {
				const char *outer = entry->d_name;
				DIR *inner = opendir(child);
				struct dirent *report;

				if (inner != NULL) {
					while ((report = readdir(inner)) != NULL && count < 32) {
						char nested[2048];
						struct stat nested_info;

						if (report->d_name[0] == '.') {
							continue;
						}
						snprintf(nested, sizeof(nested), "%s/%s", child, report->d_name);
						if (stat(nested, &nested_info) != 0 || !S_ISREG(nested_info.st_mode)) {
							continue;
						}
						char label[1200];

						snprintf(label, sizeof(label), "%s/%s(%lld)", outer, report->d_name,
						         (long long)nested_info.st_size);
						list[count++] = strdup(label);
						total++;
					}
					closedir(inner);
					continue;
				}
			}
			list[count++] = strdup(entry->d_name);
			total++;
		}
		closedir(dir);
		qsort(list, (size_t)count, sizeof(list[0]), name_compare);
		for (int i = 0; i < count && shown < 6; i++, shown++) {
			[names appendFormat:@"%s/%s ", LABELS[d], list[i]];
		}
		for (int i = 0; i < count; i++) {
			free((void *)list[i]);
		}
	}
	say("lsprobe t=%lu crash_files=%lu crashes=%s", seconds, total,
	    utf8([names stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]]));

	if (total == 0 || seconds < 300) {
		return;
	}
	/* Only the mobile directory is dumped: ReportCrash runs as root but writes the reports of the
	 * extensions and applications there, and two reports is what the ring can hold. */
	DIR *dir = opendir(DIRS[0]);
	struct dirent *entry;
	char newest[2][256];
	int found = 0;

	memset(newest, 0, sizeof(newest));
	if (dir == NULL) {
		return;
	}
	while ((entry = readdir(dir)) != NULL && found < 2) {
		if (entry->d_name[0] == '.') {
			continue;
		}
		strlcpy(newest[found], entry->d_name, sizeof(newest[0]));
		found++;
	}
	closedir(dir);
	for (int i = 0; i < found; i++) {
		char path[512], body[1201];
		FILE *file;
		size_t got;

		snprintf(path, sizeof(path), "%s/%s", DIRS[0], newest[i]);
		file = fopen(path, "r");
		if (file == NULL) {
			continue;
		}
		got = fread(body, 1, sizeof(body) - 1, file);
		fclose(file);
		body[got] = 0;
		for (size_t k = 0; k < got; k++) {
			if (body[k] == '\n') {
				body[k] = ' ';
			}
		}
		say("lsprobe t=%lu crashfile=%s bytes=%zu head=%.900s", seconds, newest[i], got, body);
	}
}

/* How many names a directory holds, without keeping them: the three listings below ask "is this
 * tree empty" and "how many" far more often than they need the names, and the counts are what tells
 * an empty DarwinDirectory from a populated one. */
static unsigned long
count_children(const char *path)
{
	DIR *dir = opendir(path);
	struct dirent *entry;
	unsigned long count = 0;

	if (dir == NULL) {
		return 0;
	}
	while ((entry = readdir(dir)) != NULL) {
		if (entry->d_name[0] != '.') {
			count++;
		}
	}
	closedir(dir);
	return count;
}

/* Every name a directory holds, joined with spaces, at most `limit` of them. Used for the one listing
 * whose names matter (the top level of mobile/Library), where the interesting fact is which of
 * MobileInstallation, SpringBoard, PosterBoard and Containers exist. No container UUIDs are ever
 * printed: they identify the device. */
static NSString *
join_children(const char *path, int limit)
{
	NSMutableString *joined = [NSMutableString string];
	DIR *dir = opendir(path);
	struct dirent *entry;
	int shown = 0;

	if (dir == NULL) {
		return joined;
	}
	while ((entry = readdir(dir)) != NULL && shown < limit) {
		if (entry->d_name[0] == '.') {
			continue;
		}
		[joined appendFormat:@"%s/%lu ", entry->d_name,
		                       (unsigned long)count_children([NSString stringWithFormat:@"%s/%s",
		                                                     path, entry->d_name].UTF8String)];
		shown++;
	}
	closedir(dir);
	return joined;
}

/* The tree the persona subsystem reads. Depth is bounded because the tree is small in principle and
 * a wrong turn would otherwise print thousands of lines into a ring that holds one megabyte. */
static void
walk_darwindir(const char *path, const char *relative, int depth, unsigned long *total, int *shown,
               NSMutableString *out)
{
	DIR *dir = opendir(path);
	struct dirent *entry;

	if (dir == NULL) {
		return;
	}
	while ((entry = readdir(dir)) != NULL) {
		char child[512], child_relative[512];
		struct stat info;

		if (entry->d_name[0] == '.') {
			continue;
		}
		(*total)++;
		snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
		snprintf(child_relative, sizeof(child_relative), "%s/%s", relative, entry->d_name);
		if (lstat(child, &info) != 0) {
			continue;
		}
		if (*shown < 40) {
			if (S_ISDIR(info.st_mode)) {
				[out appendFormat:@"%s/%lu ", child_relative, (unsigned long)count_children(child)];
			} else {
				[out appendFormat:@"%s/%lld ", child_relative, (long long)info.st_size];
			}
			(*shown)++;
		}
		if (S_ISDIR(info.st_mode) && depth > 1) {
			walk_darwindir(child, child_relative, depth - 1, total, shown, out);
		}
	}
	closedir(dir);
}

static void
probe_persona(unsigned long seconds)
{
	NSMutableString *tree = [NSMutableString string];
	unsigned long total = 0;
	int shown = 0;

	walk_darwindir("/private/var/db/DarwinDirectory", "", 3, &total, &shown, tree);
	say("lsprobe t=%lu darwindir_entries=%lu showing_first=%d tree=%s", seconds, total, shown,
	    utf8([tree stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]]));
	say("lsprobe t=%lu mobilelibrary=%s", seconds,
	    utf8([join_children("/private/var/mobile/Library", 40)
	         stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]]));
	say("lsprobe t=%lu containers_mobile=%lu containers_plugin=%lu", seconds,
	    count_children("/private/var/mobile/Containers/Data/Application"),
	    count_children("/private/var/mobile/Containers/Data/PluginKitPlugin"));
}

static void
probe(unsigned long seconds)
{
	/* Cheap probes first: they only stat, read directories or ask libproc, and they carry the facts
	 * the last two rounds failed to retrieve. The three that can block on another process
	 * (LSApplicationWorkspace talks to lsd, MobileInstallationLookup to installd, the bootstrap
	 * lookup to launchd) run afterwards, so a block there costs its own section and no more. */
	say("lsprobe probe t=%lu start", seconds);
	if (seconds >= 300) {
		/* The process table is the one answer no other source gives and it is the one that kept
		 * going missing, so at the late offset it runs before anything that can block. */
		say("lsprobe t=%lu begin=table", seconds);
		probe_table(seconds);
	}
	say("lsprobe t=%lu begin=files", seconds);
	probe_files(seconds);
	say("lsprobe t=%lu begin=processes", seconds);
	probe_processes(seconds);
	say("lsprobe t=%lu begin=crashes", seconds);
	probe_crashes(seconds);
	say("lsprobe t=%lu begin=persona", seconds);
	probe_persona(seconds);
	say("lsprobe t=%lu begin=poster", seconds);
	probe_poster_service(seconds);
	say("lsprobe t=%lu begin=mobileinstallation", seconds);
	probe_mobile_installation(seconds);
	say("lsprobe t=%lu begin=workspace", seconds);
	probe_workspace(seconds);
	say("lsprobe probe t=%lu done", seconds);
}

int
main(int argc, char **argv)
{
	/* Unconditional and first: without it "the process was never started" and "it started and then
	 * blocked before its first reading" both leave the log empty, and telling them apart has cost
	 * several rounds. One line makes the two cases different. */
	say("lsprobe started pid=%d argc=%d", (int)getpid(), argc);
	unsigned long elapsed = 0;

	if (argc < 2) {
		say("lsprobe usage: lsprobe [SECONDS ...]");
		return 2;
	}
	say("lsprobe start pid=%d", (int)getpid());
	for (int i = 1; i < argc; i++) {
		char *end = NULL;
		long target = strtol(argv[i], &end, 10);

		/* The schedule is counted from this program's own start, which init makes within the first
		 * second of the boot; each argument is an absolute offset, not a gap between probes. */
		if (end == argv[i] || target < 0) {
			say("lsprobe ignoring argument %s", argv[i]);
			continue;
		}
		if ((unsigned long)target < elapsed) {
			say("lsprobe ignoring argument %s: before the previous probe", argv[i]);
			continue;
		}
		say("lsprobe waiting %lu s until t=%lu", (unsigned long)target - elapsed, (unsigned long)target);
		sleep((unsigned)((unsigned long)target - elapsed));
		elapsed = (unsigned long)target;
		probe(elapsed);
	}
	say("lsprobe done");
	return 0;
}