/*
 * libredwood-app.dylib: code of ours inside an application, to find out why it dies (see inject.c
 * for how it gets there; build.sh's INJECT names the programs).
 *
 * An application that is launched and gone again in a third of a second leaves nothing behind here:
 * there is no crash reporter in this system, and what it writes on its standard error goes nowhere.
 * This gives it a standard error -- the console -- so that the Swift runtime's last words are read,
 * and says the frames of the stack when a signal kills it, under the dyld shared cache's slide,
 * which `ipsw dyld a2s <cache> <address>` turns back into names.
 */
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <spawn.h>
#include <sys/stat.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <execinfo.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <mach/mach.h>

extern const void *_dyld_get_shared_cache_range(size_t *length);

#define CACHE_BASE 0x180000000UL

static uintptr_t slide;
static int console = -1;
static const char *who = "?";

static void
say(const char *text, size_t length)
{
	if (console < 0) {
		console = open("/dev/console", O_WRONLY);
	}
	(void)write(console >= 0 ? console : 2, text, length);
}

static void
killed(int number, siginfo_t *info, void *context)
{
	void *frames[32];
	char line[900];
	int n = backtrace(frames, 32), k;

	(void)context;
	k = snprintf(line, sizeof(line), "redwood: %s: SIGNAL %d at %p (the cache's slide %#lx), the stack:", who, number,
	    info != NULL ? info->si_addr : NULL, (unsigned long)slide);
	for (int i = 0; i < n && k < (int)sizeof(line) - 24; i++) {
		k += snprintf(line + k, sizeof(line) - (size_t)k, " %#lx", (unsigned long)((uintptr_t)frames[i] - slide));
	}
	line[k++] = '\n';
	say(line, (size_t)k);
	signal(number, SIG_DFL);
	raise(number);
}

/*
 * Two things an application does on its way to a download, said out loud. Sileo queues a package and
 * nothing happens: no connection is made and no error is shown, so the question is how far it gets --
 * does it ask UIKit for a background task (a round trip to another daemon, which can wait for ever if
 * that daemon does not answer), and does it ever start a URL session task, for what address.
 */
static unsigned long (*begin_task)(id, SEL, id);
static void (*resume_task)(id, SEL);

static unsigned long
watched_begin_task(id self, SEL cmd, id handler)
{
	unsigned long task;
	char line[160];
	int n;

	n = snprintf(line, sizeof(line), "redwood: %s: asking UIKit for a background task\n", who);
	say(line, (size_t)(n > 0 ? n : 0));
	task = begin_task(self, cmd, handler);
	n = snprintf(line, sizeof(line), "redwood: %s: the background task is %lu\n", who, task);
	say(line, (size_t)(n > 0 ? n : 0));
	return task;
}

static void
watched_resume_task(id self, SEL cmd)
{
	const char *(*string)(id, SEL) = (const char *(*)(id, SEL))objc_msgSend;
	id (*get)(id, SEL) = (id (*)(id, SEL))objc_msgSend;
	const char *url = "?";
	char line[700];
	id request, address, text;
	int n;

	request = get(self, sel_registerName("originalRequest"));
	address = request != NULL ? get(request, sel_registerName("URL")) : NULL;
	text = address != NULL ? get(address, sel_registerName("absoluteString")) : NULL;
	if (text != NULL) {
		url = string(text, sel_registerName("UTF8String"));
	}
	n = snprintf(line, sizeof(line), "redwood: %s: starting a session task for %s\n", who,
	    url != NULL ? url : "?");
	say(line, (size_t)(n > 0 ? n : 0));
	resume_task(self, cmd);
}

/*
 * One setting of Sileo's, set for it at every start: "Show Packages From Canister" (ShowProvisional,
 * on by default). Canister is the search service Sileo asks about packages, and where its servers
 * cannot be reached the asking never stops -- 3810 session tasks in two minutes in one run here,
 * and the download of a package never gets its turn among them. With the setting off Sileo keeps to
 * the repositories themselves, which work. The prefix lives in memory and is new at every boot, so
 * there is nowhere to keep this but here. It is done on the main queue, not here in the library's
 * own initializer: at that point Foundation is loaded but has not run its own initializers yet, and
 * asking it for the defaults then kills the application before it says a word.
 */
static void
keep_to_the_repositories(void)
{
	id (*get)(id, SEL) = (id (*)(id, SEL))objc_msgSend;
	id (*from_text)(id, SEL, const char *) = (id (*)(id, SEL, const char *))objc_msgSend;
	void (*set_bool)(id, SEL, signed char, id) = (void (*)(id, SEL, signed char, id))objc_msgSend;
	id defaults = (id)objc_getClass("NSUserDefaults");
	id key = from_text((id)objc_getClass("NSString"), sel_registerName("stringWithUTF8String:"), "ShowProvisional");

	if (defaults == NULL || key == NULL) {
		return;
	}
	defaults = get(defaults, sel_registerName("standardUserDefaults"));
	if (defaults != NULL) {
		set_bool(defaults, sel_registerName("setBool:forKey:"), 0, key);
		say("redwood: Sileo: its Canister search is turned off (it is not reachable here)\n", 73);
	}
}

/*
 * apt-get install, not its simulation (-s...): the one that ends in "Unable to fetch some archives"
 * when Sileo runs it and installs the package when a terminal runs the same command.
 */
static int
apt_install(char *const argv[])
{
	int install = 0;

	if (argv == NULL || argv[0] == NULL || strcmp(argv[0], "apt-get") != 0) {
		return 0;
	}
	for (int i = 1; argv[i] != NULL; i++) {
		if (argv[i][0] == '-' && argv[i][1] == 's') {
			return 0;
		}
		install |= strcmp(argv[i], "install") == 0;
	}
	return install;
}

/*
 * What apt says goes to Sileo, which shows a line of it at most. Run the same apt-get, with the same
 * descriptors, credential and environment, under a shell that first says on the console who it runs
 * as, what lies in apt's archives and what Sileo's index says of the package, and afterwards how apt
 * ended and what the archives hold then. This is how "Unable to fetch some archives" was found: the
 * archives were empty, Sileo's /bin/mv having never run.
 */
static int
spawn_apt_said(pid_t *pid, const posix_spawn_file_actions_t *actions, const posix_spawnattr_t *attr,
    char *const argv[], char *const envp[])
{
	static char script[] =
	    "exec 7>/dev/console; J=/var/jb\n"
	    "echo \"redwood: apt-get runs as $($J/usr/bin/id), in $(pwd)\" >&7\n"
	    "$J/bin/ls -ln $J/var/cache/apt/archives $J/var/lib/apt/sileolists >&7 2>&1\n"
	    "for a in \"$@\"; do case $a in -*|install) ;; *) p=${a%%=*}\n"
	    "  echo \"redwood: $p in sileolists:\" >&7\n"
	    "  $J/usr/bin/grep -h -A30 \"^Package: $p\\$\" $J/var/lib/apt/sileolists/*Packages 2>&1 | $J/usr/bin/head -40 >&7 ;; esac; done\n"
	    "$J/usr/bin/apt-get \"$@\"\n"
	    "s=$?; echo \"redwood: apt-get exits $s; its archives after:\" >&7\n"
	    "$J/bin/ls -ln $J/var/cache/apt/archives >&7 2>&1; exit $s\n";
	char *wrapped[96];
	int k = 0;

	for (int i = 0; envp != NULL && envp[i] != NULL; i++) {
		char line[600];
		int n = snprintf(line, sizeof(line), "redwood: %s: apt-get's environment: %s\n", who, envp[i]);

		say(line, (size_t)(n < (int)sizeof(line) ? n : (int)sizeof(line) - 1));
	}
	wrapped[k++] = "sh";
	wrapped[k++] = "-c";
	wrapped[k++] = script;
	wrapped[k++] = "apt-get";
	for (int i = 1; argv[i] != NULL && k < 95; i++) {
		wrapped[k++] = argv[i];
	}
	wrapped[k] = NULL;
	return posix_spawn(pid, "/var/jb/bin/sh", actions, attr, wrapped, envp);
}

/*
 * Every program the application starts, with its arguments. Sileo does everything to the system by
 * spawning its set-user-ID helper, which execs apt or dpkg; the arguments are the whole question
 * when apt's answer to Sileo differs from apt's answer on the command line.
 */
static int
watched_spawn(pid_t *pid, const char *path, const posix_spawn_file_actions_t *actions,
    const posix_spawnattr_t *attr, char *const argv[], char *const envp[])
{
	char line[1200];
	int n = snprintf(line, sizeof(line), "redwood: %s: (uid %d) spawns %s", who, (int)getuid(),
	    path != NULL ? path : "?");

	for (int i = 0; argv != NULL && argv[i] != NULL && n < (int)sizeof(line) - 40; i++) {
		n += snprintf(line + n, sizeof(line) - (size_t)n, " %s", argv[i]);
	}
	/*
	 * The size of what it is about to move, while the file is still there: apt refuses the
	 * package Sileo puts in its archives ("Unable to fetch some archives") and takes the very
	 * same package when apt itself fetched it, so the first thing to know is whether the file
	 * is whole.
	 */
	if (argv != NULL && argv[0] != NULL && strcmp(argv[0], "mv") == 0 && argv[1] != NULL) {
		struct stat st;

		if (stat(argv[1], &st) == 0) {
			n += snprintf(line + n, sizeof(line) - (size_t)n, " [%lld bytes]", (long long)st.st_size);
		} else {
			n += snprintf(line + n, sizeof(line) - (size_t)n, " [no such file]");
		}
	}
	n += snprintf(line + n, sizeof(line) - (size_t)n, "\n");
	say(line, (size_t)n);
	if (apt_install(argv)) {
		return spawn_apt_said(pid, actions, attr, argv, envp);
	}
	n = posix_spawn(pid, path, actions, attr, argv, envp);
	if (n != 0) {
		char said[300];
		int k = snprintf(said, sizeof(said), "redwood: %s: ... and that spawn failed: %s\n", who, strerror(n));

		say(said, (size_t)(k < (int)sizeof(said) ? k : (int)sizeof(said) - 1));
	}
	return n;
}

/*
 * A file the application moves or copies itself (NSFileManager, under Swift's FileManager), when it
 * fails: from where, to where, and what the error says. Sileo moves each download into place this
 * way, and says only "The operation couldn't be completed. No such file or directory".
 */
static const char *
text_of(id object)
{
	id (*get)(id, SEL) = (id (*)(id, SEL))objc_msgSend;
	const char *(*utf8)(id, SEL) = (const char *(*)(id, SEL))objc_msgSend;
	id text;

	if (object == NULL) {
		return "(nil)";
	}
	text = (class_respondsToSelector(object_getClass(object), sel_registerName("path")) ?
	    get(object, sel_registerName("path")) : get(object, sel_registerName("description")));
	return text != NULL ? utf8(text, sel_registerName("UTF8String")) : "?";
}

static signed char (*move_url)(id, SEL, id, id, id *), (*copy_url)(id, SEL, id, id, id *);
static signed char (*move_path)(id, SEL, id, id, id *), (*copy_path)(id, SEL, id, id, id *);

static signed char
file_said(const char *what, signed char ok, id from, id to, id error)
{
	if (!ok) {
		char line[1400];
		int n = snprintf(line, sizeof(line), "redwood: %s: %s %s -> %s failed: %s\n", who, what, text_of(from),
		    text_of(to), error != NULL ? text_of(error) : "(no error given)");

		say(line, (size_t)(n < (int)sizeof(line) ? n : (int)sizeof(line) - 1));
	}
	return ok;
}

static signed char
watched_move_url(id self, SEL cmd, id from, id to, id *error)
{
	id mine = NULL;
	signed char ok = move_url(self, cmd, from, to, error != NULL ? error : &mine);

	return file_said("moveItemAtURL", ok, from, to, error != NULL ? *error : mine);
}

static signed char
watched_copy_url(id self, SEL cmd, id from, id to, id *error)
{
	id mine = NULL;
	signed char ok = copy_url(self, cmd, from, to, error != NULL ? error : &mine);

	return file_said("copyItemAtURL", ok, from, to, error != NULL ? *error : mine);
}

static signed char
watched_move_path(id self, SEL cmd, id from, id to, id *error)
{
	id mine = NULL;
	signed char ok = move_path(self, cmd, from, to, error != NULL ? error : &mine);

	return file_said("moveItemAtPath", ok, from, to, error != NULL ? *error : mine);
}

static signed char
watched_copy_path(id self, SEL cmd, id from, id to, id *error)
{
	id mine = NULL;
	signed char ok = copy_path(self, cmd, from, to, error != NULL ? error : &mine);

	return file_said("copyItemAtPath", ok, from, to, error != NULL ? *error : mine);
}

__attribute__((used)) static struct {
	const void *ours, *theirs;
} spawn_interpose __attribute__((section("__DATA,__interpose"))) = {
	(const void *)watched_spawn, (const void *)posix_spawn
};

/* Put ours in the place of a method, keeping the one that was there. */
static void
watch(const char *name, const char *selector, IMP ours, void *theirs)
{
	Class class = objc_getClass(name);
	Method method = class != NULL ? class_getInstanceMethod(class, sel_registerName(selector)) : NULL;

	if (method != NULL && method_getImplementation(method) != ours) {
		*(IMP *)theirs = method_setImplementation(method, ours);
	}
}

static void
setup_app_container_env(const char *name)
{
	char env_path[128];
	char line[256];
	FILE *f;

	snprintf(env_path, sizeof(env_path), "/redwood/etc/env/%s", name);
	f = fopen(env_path, "r");
	if (f != NULL) {
		while (fgets(line, sizeof(line), f) != NULL) {
			char *eq = strchr(line, '=');

			line[strcspn(line, "\r\n")] = '\0';
			if (eq != NULL && eq != line) {
				*eq = '\0';
				setenv(line, eq + 1, 1);
			}
		}
		fclose(f);
	}
	/* Ensure CFFIXED_USER_HOME, HOME and TMPDIR are defined for containerized applications */
	if (getenv("CFFIXED_USER_HOME") == NULL) {
		const char *uuid = NULL;

		if (strcmp(name, "Calculator") == 0) {
			uuid = "28C5E77E-97C5-4927-A2B0-04987EBF4665";
		} else if (strcmp(name, "MobileNotes") == 0) {
			uuid = "39D6F88F-A8D6-4A38-B1F1-15A98FCA5776";
		} else if (strcmp(name, "Reminders") == 0) {
			uuid = "5BF8A0AF-C8E6-4B1A-9D2E-2C3D4E5F6A71";
		} else if (strcmp(name, "MobileCal") == 0) {
			uuid = "7DFA02CB-E0A8-4D3C-BF40-4E5F6A7B8C93";
		} else if (strcmp(name, "Weather") == 0) {
			uuid = "8E5B1A2C-3D4E-4F5A-9B6C-7D8E9F0A1B2C";
		} else if (strcmp(name, "Maps") == 0) {
			uuid = "9F6C2B3D-4E5F-4A6B-8C7D-8E9F0A1B2C3D";
		}
		if (uuid != NULL) {
			char home[256], tmp[256];

			snprintf(home, sizeof(home), "/private/var/mobile/Containers/Data/Application/%s", uuid);
			snprintf(tmp, sizeof(tmp), "%s/tmp", home);
			setenv("CFFIXED_USER_HOME", home, 1);
			setenv("HOME", home, 0);
			setenv("TMPDIR", tmp, 0);
		}
	}
	{
		const char *home = getenv("CFFIXED_USER_HOME");

		if (home != NULL && home[0] == '/') {
			static const char *const subdirs[] = {
				"", "/Documents", "/Library", "/Library/Preferences", "/Library/Caches", "/tmp"
			};
			char path[320];

			(void)mkdir("/private/var/mobile/Containers", 0755);
			(void)mkdir("/private/var/mobile/Containers/Data", 0755);
			(void)mkdir("/private/var/mobile/Containers/Data/Application", 0755);
			(void)mkdir("/private/var/mobile/Containers/Shared", 0755);
			(void)mkdir("/private/var/mobile/Containers/Shared/AppGroup", 0755);
			(void)mkdir("/private/var/mobile/Containers/Shared/AppGroup/group.com.apple.weather", 0755);
			(void)mkdir("/private/var/mobile/Library/Caches/com.apple.weather", 0755);
			(void)mkdir("/private/var/mobile/Library/Caches/com.apple.Maps", 0755);
			for (unsigned i = 0; i < sizeof(subdirs) / sizeof(subdirs[0]); i++) {
				snprintf(path, sizeof(path), "%s%s", home, subdirs[i]);
				(void)mkdir(path, 0755);
			}
		}
	}
}

/*
 * WebKit VP9 software decoding preference injection:
 * WebKit gates libvpx VP9 software decoding behind the internal debug feature
 * 'SWVPDecodersAlwaysEnabled'. Hook -[WKPreferences init] to enable this feature
 * on preferences instances, register libwebrtc VP9 decoder if present,
 * and configure user defaults. Pure C implementation using ObjC runtime.
 */
static IMP orig_wkpref_init;
static int app_vp9_hook_installed;

static id
app_swizzled_wkpref_init(id self, SEL _cmd)
{
	self = ((id (*)(id, SEL))orig_wkpref_init)(self, _cmd);
	if (self != NULL) {
		Class cls = object_getClass(self);
		SEL featuresSel = sel_registerName("_internalDebugFeatures");
		if (cls && class_getInstanceMethod(cls, featuresSel)) {
			id features = ((id (*)(id, SEL))objc_msgSend)((id)cls, featuresSel);
			if (features != NULL) {
				SEL countSel = sel_registerName("count");
				SEL objAtSel = sel_registerName("objectAtIndexedSubscript:");
				SEL keySel = sel_registerName("key");
				SEL setEnabledSel = sel_registerName("_setEnabled:forInternalDebugFeature:");
				unsigned long count = ((unsigned long (*)(id, SEL))objc_msgSend)(features, countSel);
				for (unsigned long i = 0; i < count; i++) {
					id feat = ((id (*)(id, SEL, unsigned long))objc_msgSend)(features, objAtSel, i);
					if (feat != NULL) {
						id key = ((id (*)(id, SEL))objc_msgSend)(feat, keySel);
						if (key != NULL) {
							const char *cstr = ((const char * (*)(id, SEL))objc_msgSend)(key, sel_registerName("UTF8String"));
							if (cstr && strcmp(cstr, "SWVPDecodersAlwaysEnabled") == 0) {
								if (class_getInstanceMethod(cls, setEnabledSel)) {
									((void (*)(id, SEL, BOOL, id))objc_msgSend)(self, setEnabledSel, YES, feat);
									fprintf(stderr, "redwood-app: enabled SWVPDecodersAlwaysEnabled on WKPreferences %p\n", (void *)self);
								}
								break;
							}
						}
					}
				}
			}
		}
	}
	return self;
}

static void
app_enable_webkit_vp9(void)
{
	if (app_vp9_hook_installed) {
		return;
	}
	Class wkPrefClass = objc_getClass("WKPreferences");
	if (wkPrefClass != NULL) {
		SEL initSel = sel_registerName("init");
		Method m = class_getInstanceMethod(wkPrefClass, initSel);
		if (m != NULL) {
			orig_wkpref_init = method_getImplementation(m);
			method_setImplementation(m, (IMP)app_swizzled_wkpref_init);
			app_vp9_hook_installed = 1;
			fprintf(stderr, "redwood-app: installed WKPreferences init hook for SWVPDecodersAlwaysEnabled\n");
		}
	}
	/* Also invoke libwebrtc registerWebKitVP9Decoder directly if loaded in this process */
	void (*reg_vp9)(void) = (void (*)(void))dlsym(RTLD_DEFAULT, "_ZN6webrtc26registerWebKitVP9DecoderEv");
	if (reg_vp9 != NULL) {
		reg_vp9();
		fprintf(stderr, "redwood-app: registered libwebrtc VP9 decoder\n");
	}
	/* Set user defaults keys via NSUserDefaults */
	Class defsCls = objc_getClass("NSUserDefaults");
	if (defsCls != NULL) {
		id defs = ((id (*)(Class, SEL))objc_msgSend)(defsCls, sel_registerName("standardUserDefaults"));
		if (defs != NULL) {
			Class strCls = objc_getClass("NSString");
			SEL setBoolSel = sel_registerName("setBool:forKey:");
			SEL strSel = sel_registerName("stringWithUTF8String:");
			if (strCls && class_getInstanceMethod(object_getClass(defs), setBoolSel)) {
				id k1 = ((id (*)(Class, SEL, const char *))objc_msgSend)(strCls, strSel, "SWVPDecodersAlwaysEnabled");
				id k2 = ((id (*)(Class, SEL, const char *))objc_msgSend)(strCls, strSel, "WebKitSWVPDecodersAlwaysEnabled");
				((void (*)(id, SEL, BOOL, id))objc_msgSend)(defs, setBoolSel, YES, k1);
				((void (*)(id, SEL, BOOL, id))objc_msgSend)(defs, setBoolSel, YES, k2);
			}
		}
	}
}

typedef int nw_path_status_t;
typedef uint32_t SCNetworkReachabilityFlags;
typedef unsigned char Boolean;

static nw_path_status_t (*orig_nw_path_get_status)(void *path) = NULL;
static Boolean (*orig_sc_reach_get_flags)(void *target, SCNetworkReachabilityFlags *flags) = NULL;

static bool
en0_has_ipv4(void)
{
	static time_t last_check = 0;
	static bool cached = false;
	time_t now = time(NULL);

	if (now - last_check < 2 && last_check != 0) {
		return cached;
	}
	last_check = now;
	struct ifaddrs *list = NULL;
	if (getifaddrs(&list) == 0) {
		bool ok = false;
		for (struct ifaddrs *ifa = list; ifa != NULL; ifa = ifa->ifa_next) {
			if (ifa->ifa_name && strcmp(ifa->ifa_name, "en0") == 0 &&
			    ifa->ifa_addr && ifa->ifa_addr->sa_family == AF_INET) {
				ok = true;
				break;
			}
		}
		freeifaddrs(list);
		cached = ok;
		return ok;
	}
	return cached;
}

static nw_path_status_t
hooked_nw_path_get_status(void *path)
{
	nw_path_status_t s = orig_nw_path_get_status ? orig_nw_path_get_status(path) : 1;
	if (s != 1 && en0_has_ipv4()) {
		return 1; /* nw_path_status_satisfied */
	}
	return s;
}

static Boolean
hooked_sc_reach_get_flags(void *target, SCNetworkReachabilityFlags *flags)
{
	Boolean ok = orig_sc_reach_get_flags ? orig_sc_reach_get_flags(target, flags) : 1;
	if (flags != NULL && en0_has_ipv4()) {
		*flags |= 0x00020002; /* kSCNetworkReachabilityFlagsReachable | kSCNetworkReachabilityFlagsIsDirect */
		*flags &= ~(1u << 2); /* clear ConnectionRequired */
	}
	return ok;
}

/* ObjC hooks for Network.framework and GeoServices */
static IMP orig_geo_is_net_reachable;
static IMP orig_geo_is_wifi_reachable;
static IMP orig_nwpath_status;
static IMP orig_nwpath_is_satisfied;
static IMP orig_nwpath_is_expensive;
static IMP orig_nwpath_is_constrained;

static signed char
hooked_geo_is_net_reachable(id self, SEL cmd)
{
	if (en0_has_ipv4()) {
		return 1;
	}
	return orig_geo_is_net_reachable ? ((signed char (*)(id, SEL))orig_geo_is_net_reachable)(self, cmd) : 1;
}

static signed char
hooked_geo_is_wifi_reachable(id self, SEL cmd)
{
	if (en0_has_ipv4()) {
		return 1;
	}
	return orig_geo_is_wifi_reachable ? ((signed char (*)(id, SEL))orig_geo_is_wifi_reachable)(self, cmd) : 1;
}

static int
hooked_nwpath_status(id self, SEL cmd)
{
	int s = orig_nwpath_status ? ((int (*)(id, SEL))orig_nwpath_status)(self, cmd) : 1;
	if (s != 1 && en0_has_ipv4()) {
		return 1; /* nw_path_status_satisfied */
	}
	return s;
}

static signed char
hooked_nwpath_is_satisfied(id self, SEL cmd)
{
	if (en0_has_ipv4()) {
		return 1;
	}
	return orig_nwpath_is_satisfied ? ((signed char (*)(id, SEL))orig_nwpath_is_satisfied)(self, cmd) : 1;
}

static signed char
hooked_nwpath_is_expensive(id self, SEL cmd)
{
	if (en0_has_ipv4()) {
		return 0;
	}
	return orig_nwpath_is_expensive ? ((signed char (*)(id, SEL))orig_nwpath_is_expensive)(self, cmd) : 0;
}

static signed char
hooked_nwpath_is_constrained(id self, SEL cmd)
{
	if (en0_has_ipv4()) {
		return 0;
	}
	return orig_nwpath_is_constrained ? ((signed char (*)(id, SEL))orig_nwpath_is_constrained)(self, cmd) : 0;
}

struct app_symbol_rebind {
	const char *name;
	void *replacement;
	void **original;
};

static void
rebind_image_symbols(const struct mach_header *header, intptr_t slide_val)
{
	const struct mach_header_64 *mh = (const struct mach_header_64 *)header;
	if (mh->magic != MH_MAGIC_64) {
		return;
	}
	const struct load_command *lc = (const struct load_command *)((uintptr_t)mh + sizeof(struct mach_header_64));
	uintptr_t linkedit_base = 0;
	const struct symtab_command *symtab = NULL;
	const struct dysymtab_command *dysymtab = NULL;

	for (uint32_t i = 0; i < mh->ncmds; i++) {
		if (lc->cmd == LC_SEGMENT_64) {
			const struct segment_command_64 *seg = (const struct segment_command_64 *)lc;
			if (strcmp(seg->segname, "__LINKEDIT") == 0) {
				linkedit_base = (uintptr_t)slide_val + seg->vmaddr - seg->fileoff;
			}
		} else if (lc->cmd == LC_SYMTAB) {
			symtab = (const struct symtab_command *)lc;
		} else if (lc->cmd == LC_DYSYMTAB) {
			dysymtab = (const struct dysymtab_command *)lc;
		}
		lc = (const struct load_command *)((uintptr_t)lc + lc->cmdsize);
	}
	if (!linkedit_base || !symtab || !dysymtab) {
		return;
	}

	const struct nlist_64 *symList = (const struct nlist_64 *)(linkedit_base + symtab->symoff);
	const char *strTab = (const char *)(linkedit_base + symtab->stroff);
	const uint32_t *indirectSyms = (const uint32_t *)(linkedit_base + dysymtab->indirectsymoff);

	static struct app_symbol_rebind target_rebinds[] = {
		{ "_nw_path_get_status", (void *)hooked_nw_path_get_status, (void **)&orig_nw_path_get_status },
		{ "_SCNetworkReachabilityGetFlags", (void *)hooked_sc_reach_get_flags, (void **)&orig_sc_reach_get_flags },
	};
	const size_t num_rebinds = sizeof(target_rebinds) / sizeof(target_rebinds[0]);

	lc = (const struct load_command *)((uintptr_t)mh + sizeof(struct mach_header_64));
	for (uint32_t i = 0; i < mh->ncmds; i++) {
		if (lc->cmd == LC_SEGMENT_64) {
			const struct segment_command_64 *seg = (const struct segment_command_64 *)lc;
			if (strncmp(seg->segname, "__DATA", 6) == 0) {
				const struct section_64 *sec = (const struct section_64 *)((uintptr_t)seg + sizeof(struct segment_command_64));
				for (uint32_t j = 0; j < seg->nsects; j++) {
					uint8_t type = sec[j].flags & SECTION_TYPE;
					if (type == S_NON_LAZY_SYMBOL_POINTERS || type == S_LAZY_SYMBOL_POINTERS) {
						uint32_t sec_indirect_offset = sec[j].reserved1;
						void **pointers = (void **)((uintptr_t)slide_val + sec[j].addr);
						size_t entry_count = sec[j].size / sizeof(void *);

						for (size_t k = 0; k < entry_count; k++) {
							uint32_t symIndex = indirectSyms[sec_indirect_offset + k];
							if (symIndex == INDIRECT_SYMBOL_LOCAL || symIndex == INDIRECT_SYMBOL_ABS) {
								continue;
							}
							uint32_t strOffset = symList[symIndex].n_un.n_strx;
							if (strOffset == 0) continue;
							const char *symbol_name = strTab + strOffset;
							for (size_t r = 0; r < num_rebinds; r++) {
								if (strcmp(symbol_name, target_rebinds[r].name) == 0) {
									if (target_rebinds[r].original != NULL && *target_rebinds[r].original == NULL) {
										*target_rebinds[r].original = pointers[k];
									}
									vm_prot_t orig_prot = VM_PROT_READ | VM_PROT_WRITE;
									vm_address_t page_addr = (vm_address_t)&pointers[k] & ~PAGE_MASK;
									vm_size_t page_size = PAGE_SIZE;
									vm_protect(mach_task_self(), page_addr, page_size, FALSE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY);
									pointers[k] = target_rebinds[r].replacement;
									vm_protect(mach_task_self(), page_addr, page_size, FALSE, orig_prot);
									break;
								}
							}
						}
					}
				}
			}
		}
		lc = (const struct load_command *)((uintptr_t)lc + lc->cmdsize);
	}
}

static void
app_dyld_image_added(const struct mach_header *mh, intptr_t vmaddr_slide)
{
	rebind_image_symbols(mh, vmaddr_slide);
	if (!app_vp9_hook_installed) {
		app_enable_webkit_vp9();
	}
}

__attribute__((constructor))
static void
redwood_app_init(void)
{
	static const int signals[] = { SIGTRAP, SIGABRT, SIGILL, SIGSEGV, SIGBUS, SIGSYS };
	struct sigaction action = { 0 };
	const char *name = getprogname();
	char line[200];
	size_t length = 0;
	int n;

	if (name == NULL) {
		return;
	}
	who = name;
	slide = (uintptr_t)_dyld_get_shared_cache_range(&length) - CACHE_BASE;
	setup_app_container_env(name);
	/*
	 * An application's standard error goes nowhere here, and that is where the Swift runtime says
	 * what it is about to die of ("Fatal error: ..."), so it is pointed at the console.
	 */
	console = open("/dev/console", O_WRONLY);
	if (console >= 0) {
		dup2(console, 2);
	}
	action.sa_sigaction = killed;
	action.sa_flags = SA_SIGINFO | SA_NODEFER | SA_RESETHAND;
	sigemptyset(&action.sa_mask);
	for (unsigned i = 0; i < sizeof(signals) / sizeof(signals[0]); i++) {
		sigaction(signals[i], &action, NULL);
	}
	if (strcmp(name, "Sileo") == 0) {
		dispatch_async(dispatch_get_main_queue(), ^{ keep_to_the_repositories(); });
	}
	watch("UIApplication", "beginBackgroundTaskWithExpirationHandler:", (IMP)watched_begin_task, &begin_task);
	/*
	 * NSURLSessionTask alone: its subclasses inherit the very same method, and watching it twice
	 * would leave ours as the "original" of itself -- a call that never returns.
	 */
	watch("NSURLSessionTask", "resume", (IMP)watched_resume_task, &resume_task);
	watch("NSFileManager", "moveItemAtURL:toURL:error:", (IMP)watched_move_url, &move_url);
	watch("NSFileManager", "copyItemAtURL:toURL:error:", (IMP)watched_copy_url, &copy_url);
	watch("NSFileManager", "moveItemAtPath:toPath:error:", (IMP)watched_move_path, &move_path);
	watch("NSFileManager", "copyItemAtPath:toPath:error:", (IMP)watched_copy_path, &copy_path);

	watch("GEONetworkObserver", "isNetworkReachable", (IMP)hooked_geo_is_net_reachable, &orig_geo_is_net_reachable);
	watch("GEONetworkObserver", "isWiFiReachable", (IMP)hooked_geo_is_wifi_reachable, &orig_geo_is_wifi_reachable);
	watch("NWPath", "status", (IMP)hooked_nwpath_status, &orig_nwpath_status);
	watch("NWPath", "isSatisfied", (IMP)hooked_nwpath_is_satisfied, &orig_nwpath_is_satisfied);
	watch("NWPath", "isExpensive", (IMP)hooked_nwpath_is_expensive, &orig_nwpath_is_expensive);
	watch("NWPath", "isConstrained", (IMP)hooked_nwpath_is_constrained, &orig_nwpath_is_constrained);
	watch("NWConcrete_nw_path", "status", (IMP)hooked_nwpath_status, &orig_nwpath_status);
	watch("NWConcrete_nw_path", "isSatisfied", (IMP)hooked_nwpath_is_satisfied, &orig_nwpath_is_satisfied);

	app_enable_webkit_vp9();
	_dyld_register_func_for_add_image(app_dyld_image_added);

	n = snprintf(line, sizeof(line), "redwood: %s: started as uid %d (effective %d), watching for what kills it\n",
	    name, (int)getuid(), (int)geteuid());
	say(line, (size_t)(n > 0 ? n : 0));
}
