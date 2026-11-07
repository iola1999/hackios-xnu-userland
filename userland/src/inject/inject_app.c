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
		} else if (strcmp(name, "MobileSafari") == 0) {
			uuid = "A1B2C3D4-5E6F-4A7B-8C9D-0E1F2A3B4C5D";
		} else if (strcmp(name, "com.apple.WebKit.Networking") == 0 ||
		           strcmp(name, "com.apple.WebKit.WebContent") == 0 ||
		           strcmp(name, "com.apple.WebKit.GPU") == 0) {
			uuid = "A1B2C3D4-5E6F-4A7B-8C9D-0E1F2A3B4C5D";
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
			static const char *const weather_subdirs[] = {
				"/Library/Weather", "/Library/WeatherDaemon"
			};
			static const char *const maps_subdirs[] = {
				"/Library/Maps"
			};
			char path[320];

			(void)mkdir("/private/var/mobile/Containers", 0755);
			(void)mkdir("/private/var/mobile/Containers/Data", 0755);
			(void)mkdir("/private/var/mobile/Containers/Data/Application", 0755);
			(void)mkdir("/private/var/mobile/Containers/Shared", 0755);
			(void)mkdir("/private/var/mobile/Containers/Shared/AppGroup", 0755);

			for (unsigned i = 0; i < sizeof(subdirs) / sizeof(subdirs[0]); i++) {
				snprintf(path, sizeof(path), "%s%s", home, subdirs[i]);
				(void)mkdir(path, 0755);
			}
			if (strcmp(name, "Weather") == 0) {
				for (unsigned i = 0; i < sizeof(weather_subdirs) / sizeof(weather_subdirs[0]); i++) {
					snprintf(path, sizeof(path), "%s%s", home, weather_subdirs[i]);
					(void)mkdir(path, 0755);
				}
			} else if (strcmp(name, "Maps") == 0) {
				for (unsigned i = 0; i < sizeof(maps_subdirs) / sizeof(maps_subdirs[0]); i++) {
					snprintf(path, sizeof(path), "%s%s", home, maps_subdirs[i]);
					(void)mkdir(path, 0755);
				}
			}
		}

		/* AppGroup containers and shared directory structures */
		static const char *const group_subdirs[] = {
			"", "/Documents", "/Library", "/Library/Preferences", "/Library/Caches", "/tmp"
		};
		char gpath[320];

		for (unsigned i = 0; i < sizeof(group_subdirs) / sizeof(group_subdirs[0]); i++) {
			snprintf(gpath, sizeof(gpath), "/private/var/mobile/Containers/Shared/AppGroup/group.com.apple.weather%s", group_subdirs[i]);
			(void)mkdir(gpath, 0755);
			snprintf(gpath, sizeof(gpath), "/private/var/mobile/Containers/Shared/AppGroup/group.com.apple.Maps%s", group_subdirs[i]);
			(void)mkdir(gpath, 0755);
		}

		/* Shared daemon and caching paths */
		static const char *const shared_paths[] = {
			"/private/var/mobile/Library/Weather", "/private/var/mobile/Library/WeatherDaemon",
			"/private/var/mobile/Library/Caches/Weather", "/private/var/mobile/Library/Caches/com.apple.weather",
			"/private/var/mobile/Library/Caches/com.apple.weatherd", "/private/var/mobile/Library/SyncedPreferences",
			"/private/var/mobile/Library/Maps", "/private/var/mobile/Library/Caches/Maps",
			"/private/var/mobile/Library/Caches/com.apple.Maps", "/private/var/mobile/Library/Caches/GeoServices"
		};
		for (unsigned i = 0; i < sizeof(shared_paths) / sizeof(shared_paths[0]); i++) {
			(void)mkdir(shared_paths[i], 0755);
		}

		/* Fallback DNS resolver setup in /private/var/run/resolv.conf */
		(void)mkdir("/private/var/run", 0775);
		struct stat st_rc;
		if (stat("/private/var/run/resolv.conf", &st_rc) != 0 || st_rc.st_size == 0) {
			int rcfd = open("/private/var/run/resolv.conf", O_WRONLY | O_CREAT | O_TRUNC, 0644);
			if (rcfd >= 0) {
				static const char default_dns[] =
					"nameserver 119.29.29.29\n"
					"nameserver 223.5.5.5\n"
					"nameserver 8.8.8.8\n"
					"nameserver 1.1.1.1\n";
				(void)write(rcfd, default_dns, sizeof(default_dns) - 1);
				close(rcfd);
			}
		}
		if (stat("/private/etc/resolv.conf", &st_rc) != 0) {
			(void)symlink("/private/var/run/resolv.conf", "/private/etc/resolv.conf");
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

#include "inject_cellular.inc"

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

	/* Check system clock for TLS certificate validation sanity */
	time_t boot_sec = time(NULL);
	if (boot_sec < 1767225600L) {
		char time_warn[256];
		int tw_len = snprintf(time_warn, sizeof(time_warn),
		    "redwood: %s: WARNING: system clock (%ld) is before 2026! TLS handshake depends on com.apple.timed NTP sync.\n",
		    name, (long)boot_sec);
		say(time_warn, (size_t)(tw_len > 0 ? tw_len : 0));
	}

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

	watch_network_classes();
	if (en0_has_ipv4()) {
		dispatch_async(dispatch_get_main_queue(), ^{ post_geo_network_reachable_notification(); });
	}

	app_enable_webkit_vp9();
	_dyld_register_func_for_add_image(app_dyld_image_added);

	n = snprintf(line, sizeof(line), "redwood: %s: started as uid %d (effective %d), watching for what kills it\n",
	    name, (int)getuid(), (int)geteuid());
	say(line, (size_t)(n > 0 ? n : 0));
}
