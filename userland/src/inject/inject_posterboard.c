/*
 * PosterBoard's data store, observed from inside: on this boot
 * -[PBFPosterExtensionDataStore initWithURL:...error:] returns nil, and the only line that reaches
 * the console is the caller's "Data store could not be created for an unknown reason", because the
 * init's own messages are below the level this image prints. Loaded into PosterBoard through
 * inject.conf (the same library as SpringBoard's adaptations, its own constructor), this prints:
 *
 *   pb26 error N: DOMAIN CODE DESCRIPTION, made at IMAGE+OFFSET ...  for every NSError PosterBoard
 *       creates through -initWithDomain:code:userInfo:, the first ERROR_LINES of them, with the
 *       frames above Foundation as image name and offset (the offset is the one in the shared cache
 *       image, comparable with tools/dsc/dsc.py addresses minus the image's base);
 *   pb26 data store init URL -> RESULT, error E  around the init itself;
 *   pb26 STEP -> RESULT, error E  for each step the init can fail on without an error of its own
 *       (the migrator's four checks and the SQLite database), whose messages are signposts;
 *   pb26 migrate URL from F to T -> RESULT, error E  around the class method the migrator's
 *       migrateDataStoreToCurrentVersion: calls, whose error that caller drops, and
 *   pb26 newest valid version -> V  for the version it migrates from (NSNotFound when no version
 *       between the minimum and the current one passes the viability check).
 *
 * Only errors with a PosterBoard frame among their callers are printed: the rest are UIKit's and
 * Foundation's own and used to take the whole budget.
 *
 * One change, not a probe: a data store version is valid only when each of its directories and
 * files carries the resource values PFPosterPathURLResourceValues() (PosterFoundation) names
 * (_PBFDetermineDataStoreViabilityForVersionWithinBaseURL, through the NSURL category method
 * -pbf_URLIsReachableAndConformToResourceValues:error:), and one of them is the file protection
 * class. The RAM disk volume has no data protection, so that key cannot read back as asked, no
 * version is ever valid and the migration from 59 to 61 fails on every one of PosterBoard's
 * attempts (render13: 1151). The category methods that compare and set resource values get the
 * dictionary without NSURLFileProtectionKey here; the first calls of each print the URL, the
 * values asked for and the answer:
 *
 *   pb26 METHOD URL asked {VALUES} -> RESULT
 */
#include <CoreFoundation/CoreFoundation.h>
#include <crt_externs.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <mach-o/dyld.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>
#include <notify.h>
#include <dispatch/dispatch.h>

enum { ERROR_LINES = 300, FRAMES = 8 };

static unsigned errors;
static id (*error_init)(id, SEL, id, long, id);
static id (*store_init)(id, SEL, id, id, id, id, BOOL, id, id *);
static BOOL (*up_to_date)(id, SEL);
static long (*migrate)(id, SEL, id *);
static BOOL (*bundle_migration)(id, SEL, id *), (*validate)(id, SEL, id *);
static id (*database_init)(id, SEL, id, unsigned long, id *);
static BOOL (*necessary_migrations)(id, SEL, id, BOOL, id *);
static BOOL (*migrate_between)(id, SEL, id, unsigned long, unsigned long, BOOL, id *);
static unsigned long (*newest_valid)(id, SEL);

static const char *
text_of(id object)
{
    if (!object) return "nil";
    id described = ((id (*)(id, SEL))objc_msgSend)(object, sel_registerName("description"));
    const char *utf8 = described ? ((const char *(*)(id, SEL))objc_msgSend)(described,
        sel_registerName("UTF8String")) : NULL;
    return utf8 ? utf8 : "?";
}

/* The callers as "image+offset", skipping this file and Foundation's NSError constructors. */
static void
callers(char *out, size_t size)
{
    void *frames[FRAMES + 4];
    int n = backtrace(frames, FRAMES + 4);
    size_t used = 0;
    out[0] = '\0';
    for (int i = 2; i < n && used + 1 < size; i++) {
        Dl_info info;
        if (!dladdr(frames[i], &info) || !info.dli_fname) continue;
        const char *name = strrchr(info.dli_fname, '/');
        name = name ? name + 1 : info.dli_fname;
        used += (size_t)snprintf(out + used, size - used, " %s+%#lx", name,
            (unsigned long)((char *)frames[i] - (char *)info.dli_fbase));
    }
}

static id
logged_error_init(id self, SEL cmd, id domain, long code, id info)
{
    id made = error_init(self, cmd, domain, code, info);
    if (errors < ERROR_LINES) {
        char where[512];
        callers(where, sizeof(where));
        if (strstr(where, "Poster")) {
            fprintf(stderr, "pb26 error %u: %s %ld %.400s, made at%s\n", errors, text_of(domain), code,
                text_of(info), where);
            errors++;
        }
    }
    return made;
}

static id
logged_store_init(id self, SEL cmd, id url, id assertions, id extensions, id observer, BOOL migrated,
    id monitor, id *error)
{
    id local = nil;
    id *out = error ? error : &local;
    fprintf(stderr, "pb26 data store init %s, extension provider %s, migrated %d\n", text_of(url),
        extensions ? class_getName(object_getClass(extensions)) : "nil", migrated);
    id made = store_init(self, cmd, url, assertions, extensions, observer, migrated, monitor, out);
    fprintf(stderr, "pb26 data store init -> %p, error %.400s\n", (void *)made, text_of(*out));
    return made;
}

static BOOL
logged_up_to_date(id self, SEL cmd)
{
    BOOL answer = up_to_date(self, cmd);
    fprintf(stderr, "pb26 isDataStoreUpToDateForCurrentVersion -> %d\n", answer);
    return answer;
}

static long
logged_migrate(id self, SEL cmd, id *error)
{
    id local = nil;
    id *out = error ? error : &local;
    long result = migrate(self, cmd, out);
    fprintf(stderr, "pb26 migrateDataStoreToCurrentVersion -> %ld, error %.400s\n", result, text_of(*out));
    return result;
}

static BOOL
logged_bundle_migration(id self, SEL cmd, id *error)
{
    id local = nil;
    id *out = error ? error : &local;
    BOOL ok = bundle_migration(self, cmd, out);
    fprintf(stderr, "pb26 performPosterBundleIdentifierMigrationIfNeeded -> %d, error %.400s\n", ok,
        text_of(*out));
    return ok;
}

static BOOL
logged_validate(id self, SEL cmd, id *error)
{
    id local = nil;
    id *out = error ? error : &local;
    BOOL ok = validate(self, cmd, out);
    fprintf(stderr, "pb26 validateDataStoreIntegrity -> %d, error %.400s\n", ok, text_of(*out));
    return ok;
}

static id
logged_database_init(id self, SEL cmd, id url, unsigned long options, id *error)
{
    id local = nil;
    id *out = error ? error : &local;
    id made = database_init(self, cmd, url, options, out);
    fprintf(stderr, "pb26 database %s options %#lx -> %p, error %.400s\n", text_of(url), options,
        (void *)made, text_of(*out));
    return made;
}

static BOOL
logged_necessary_migrations(id self, SEL cmd, id url, BOOL force, id *error)
{
    id local = nil;
    id *out = error ? error : &local;
    BOOL ok = necessary_migrations(self, cmd, url, force, out);
    fprintf(stderr, "pb26 performNecessaryMigrations %s force %d -> %d, error %.400s\n", text_of(url),
        force, ok, text_of(*out));
    return ok;
}

enum { RESOURCE_LINES = 8 };

static BOOL (*reachable_conform)(id, SEL, id, id *), (*conforms)(id, SEL, id, id *);
static BOOL (*update_values)(id, SEL, id, id *), (*recursive_update)(id, SEL, id, id *);

static BOOL
resource_call(BOOL (*original)(id, SEL, id, id *), unsigned *calls, id self, SEL cmd, id values, id *error)
{
    BOOL ok = original(self, cmd, values, error);
    if ((*calls)++ < RESOURCE_LINES)
        fprintf(stderr, "pb26 %s %s asked %.300s -> %d\n", sel_getName(cmd), text_of(self), text_of(values), ok);
    return ok;
}

static BOOL
logged_reachable_conform(id self, SEL cmd, id values, id *error)
{
    static unsigned calls;
    return resource_call(reachable_conform, &calls, self, cmd, values, error);
}

static BOOL
logged_conforms(id self, SEL cmd, id values, id *error)
{
    static unsigned calls;
    return resource_call(conforms, &calls, self, cmd, values, error);
}

static BOOL
logged_update_values(id self, SEL cmd, id values, id *error)
{
    static unsigned calls;
    return resource_call(update_values, &calls, self, cmd, values, error);
}

static BOOL
logged_recursive_update(id self, SEL cmd, id values, id *error)
{
    static unsigned calls;
    return resource_call(recursive_update, &calls, self, cmd, values, error);
}

static BOOL
logged_migrate_between(id self, SEL cmd, id url, unsigned long from, unsigned long to, BOOL cleanup,
    id *error)
{
    id local = nil;
    id *out = error ? error : &local;
    BOOL ok = migrate_between(self, cmd, url, from, to, cleanup, out);
    fprintf(stderr, "pb26 migrate %s from %lu to %lu -> %d, error %.400s\n", text_of(url), from, to, ok,
        text_of(*out));
    return ok;
}

static unsigned long
logged_newest_valid(id self, SEL cmd)
{
    unsigned long version = newest_valid(self, cmd);
    fprintf(stderr, "pb26 newest valid version -> %lu\n", version);
    return version;
}

static void
replace_in(BOOL class_method, const char *class_name, const char *selector, IMP with, void *original)
{
    Class cls = objc_getClass(class_name);
    SEL sel = sel_registerName(selector);
    Method m = !cls ? NULL : class_method ? class_getClassMethod(cls, sel) : class_getInstanceMethod(cls, sel);
    if (!m) {
        fprintf(stderr, "pb26: no %c[%s %s] to watch\n", class_method ? '+' : '-', class_name, selector);
        return;
    }
    *(IMP *)original = method_setImplementation(m, with);
}

static void
replace(const char *class_name, const char *selector, IMP with, void *original)
{
    replace_in(NO, class_name, selector, with, original);
}

__attribute__((constructor)) static void
install_posterboard(void)
{
    if (!getprogname() || strcmp(getprogname(), "PosterBoard")) return;
    replace("NSError", "initWithDomain:code:userInfo:", (IMP)logged_error_init, &error_init);
    replace("PBFPosterExtensionDataStore",
        "initWithURL:runtimeAssertionProvider:extensionProvider:observer:wasMigrationJustPerformed:"
        "applicationStateMonitor:error:", (IMP)logged_store_init, &store_init);
    replace("PBFPosterExtensionDataStoreMigrator", "isDataStoreUpToDateForCurrentVersion",
        (IMP)logged_up_to_date, &up_to_date);
    replace("PBFPosterExtensionDataStoreMigrator", "migrateDataStoreToCurrentVersion:",
        (IMP)logged_migrate, &migrate);
    replace("PBFPosterExtensionDataStoreMigrator", "performPosterBundleIdentifierMigrationIfNeeded:",
        (IMP)logged_bundle_migration, &bundle_migration);
    replace("PBFPosterExtensionDataStoreMigrator", "validateDataStoreIntegrity:", (IMP)logged_validate,
        &validate);
    replace("PBFPosterExtensionDataStoreSQLiteDatabase", "initWithURL:options:error:",
        (IMP)logged_database_init, &database_init);
    replace("PBFPosterExtensionDataStoreXPCServiceGlue",
        "_lock_performNecessaryMigrationsForDataStoreAtURL:shouldForce:error:",
        (IMP)logged_necessary_migrations, &necessary_migrations);
    replace_in(YES, "PBFPosterExtensionDataStoreMigrator",
        "migrateDataStoreAtBaseURL:fromVersion:toVersion:cleanupAfterMigrationSucceeds:error:",
        (IMP)logged_migrate_between, &migrate_between);
    replace("PBFPosterExtensionDataStoreMigrator", "_mostUpToDateValidDataStoreToMigrateFrom",
        (IMP)logged_newest_valid, &newest_valid);
    replace("NSURL", "pbf_URLIsReachableAndConformToResourceValues:error:", (IMP)logged_reachable_conform,
        &reachable_conform);
    replace("NSURL", "pbf_URLConformsToExpectedResourceValues:error:", (IMP)logged_conforms, &conforms);
    replace("NSURL", "pbf_updateResourceValues:error:", (IMP)logged_update_values, &update_values);
    replace("NSURL", "pbf_recursivelyUpdateResourceValues:error:", (IMP)logged_recursive_update,
        &recursive_update);
    fprintf(stderr, "pb26: watching NSError, the data store init and its steps\n");
}

/*
 * MercuryPosterExtension, the provider of the default wallpaper, is an ExtensionKit extension
 * whose standard error is /dev/null, so its os_log lines and the Metal library's printf output are
 * lost. Here its standard error goes into a pipe and a thread passes each line to
 * kern.redwood.say ("say PID MercuryPosterExtension: ...").
 *
 * It has no launchd job for JOB_ENV to give variables to: /redwood/etc/env/MercuryPosterExtension
 * (EXT_ENV in tools/k90auto/package.py), one KEY=VALUE a line, is put in its environment here and
 * the process executes itself again, because some variables are read once while libSystem starts,
 * before any inserted library runs: OS_ACTIVITY_DT_MODE=YES, the copy of os_log on standard error,
 * set here and not again in kernel-12377-cmd44-bo25 printed none of the extension's os_log lines.
 * REDWOOD_EXT_ENV marks the second start. The kernel inserts this library again at that exec
 * (kern.exec_inject matches the program name).
 */
#define EXT_ENV_FILE "/redwood/etc/env/MercuryPosterExtension"

static int
read_env(const char *path)
{
    FILE *file = fopen(path, "r");
    char line[256];
    int count = 0;

    while (file != NULL && fgets(line, sizeof(line), file) != NULL) {
        char *equals = strchr(line, '=');

        line[strcspn(line, "\n")] = '\0';
        if (equals == NULL || equals == line) continue;
        *equals = '\0';
        setenv(line, equals + 1, 1);
        count++;
    }
    if (file != NULL) fclose(file);
    return count;
}

static void *
forward_stderr(void *arg)
{
    FILE *in = fdopen((int)(intptr_t)arg, "r");
    char line[256];

    /* paced under the 200 lines a second the kernel keeps; a full pipe makes the writer wait */
    while (in != NULL && fgets(line, sizeof(line), in) != NULL) {
        sysctlbyname("kern.redwood.say", NULL, NULL, line, strlen(line));
        usleep(5000);
    }
    return NULL;
}

static int s_wallpaper_suspended = 0;
#define MAX_DISPLAY_LINKS 16
static id s_display_links[MAX_DISPLAY_LINKS];
static int s_num_display_links = 0;
static pthread_mutex_t s_link_mutex = PTHREAD_MUTEX_INITIALIZER;

static void (*orig_add_to_run_loop)(id, SEL, id, id);
static void (*orig_display_link_invalidate)(id, SEL);
static void (*orig_cmdbuf_commit)(id, SEL);

static void
mercury_display_link_add_to_run_loop(id self, SEL cmd, id runloop, id mode)
{
    pthread_mutex_lock(&s_link_mutex);
    int found = 0;
    for (int i = 0; i < s_num_display_links; i++) {
        if (s_display_links[i] == self) {
            found = 1;
            break;
        }
    }
    if (!found && s_num_display_links < MAX_DISPLAY_LINKS) {
        s_display_links[s_num_display_links++] = self;
    }
    if (s_wallpaper_suspended) {
        ((void (*)(id, SEL, BOOL))objc_msgSend)(self, sel_registerName("setPaused:"), YES);
    }
    pthread_mutex_unlock(&s_link_mutex);

    if (orig_add_to_run_loop) {
        orig_add_to_run_loop(self, cmd, runloop, mode);
    }
}

static void
mercury_display_link_invalidate(id self, SEL cmd)
{
    pthread_mutex_lock(&s_link_mutex);
    for (int i = 0; i < s_num_display_links; i++) {
        if (s_display_links[i] == self) {
            s_display_links[i] = s_display_links[--s_num_display_links];
            break;
        }
    }
    pthread_mutex_unlock(&s_link_mutex);

    if (orig_display_link_invalidate) {
        orig_display_link_invalidate(self, cmd);
    }
}

static void
mercury_set_suspended(int suspend)
{
    pthread_mutex_lock(&s_link_mutex);
    s_wallpaper_suspended = suspend;
    for (int i = 0; i < s_num_display_links; i++) {
        id link = s_display_links[i];
        if (link) {
            ((void (*)(id, SEL, BOOL))objc_msgSend)(link, sel_registerName("setPaused:"), suspend ? YES : NO);
        }
    }
    pthread_mutex_unlock(&s_link_mutex);
    fprintf(stderr, "pb26: mercury poster submissions %s\n", suspend ? "suspended" : "resumed");
}

static void
mercury_cmdbuf_commit(id self, SEL cmd)
{
    if (s_wallpaper_suspended) {
        usleep(200000);
    }
    if (orig_cmdbuf_commit) {
        orig_cmdbuf_commit(self, cmd);
    }
}

static void
install_mercury_hooks(void)
{
    static int hooks_installed = 0;
    if (hooks_installed) return;
    hooks_installed = 1;

    Class dl_cls = objc_getClass("CADisplayLink");
    if (dl_cls) {
        Method m_add = class_getInstanceMethod(dl_cls, sel_registerName("addToRunLoop:forMode:"));
        if (m_add) {
            orig_add_to_run_loop = (void (*)(id, SEL, id, id))method_getImplementation(m_add);
            method_setImplementation(m_add, (IMP)mercury_display_link_add_to_run_loop);
        }
        Method m_inv = class_getInstanceMethod(dl_cls, sel_registerName("invalidate"));
        if (m_inv) {
            orig_display_link_invalidate = (void (*)(id, SEL))method_getImplementation(m_inv);
            method_setImplementation(m_inv, (IMP)mercury_display_link_invalidate);
        }
    }

    Class cb_cls = objc_getClass("_MTLCommandBuffer");
    if (cb_cls) {
        Method m_commit = class_getInstanceMethod(cb_cls, sel_registerName("commit"));
        if (m_commit) {
            orig_cmdbuf_commit = (void (*)(id, SEL))method_getImplementation(m_commit);
            method_setImplementation(m_commit, (IMP)mercury_cmdbuf_commit);
        }
    }

    int token_suspend = 0;
    notify_register_dispatch("com.redwood.wallpaper.suspend", &token_suspend, dispatch_get_main_queue(), ^(int t) {
        (void)t;
        mercury_set_suspended(1);
    });

    int token_resume = 0;
    notify_register_dispatch("com.redwood.wallpaper.resume", &token_resume, dispatch_get_main_queue(), ^(int t) {
        (void)t;
        mercury_set_suspended(0);
    });

    fprintf(stderr, "pb26: mercury poster throttling and suspension hooks installed\n");
}

static void
mercury_image_callback(const struct mach_header *header, intptr_t slide)
{
    (void)header;
    (void)slide;
    Class dl_cls = objc_getClass("CADisplayLink");
    if (dl_cls) {
        install_mercury_hooks();
    }
}

__attribute__((constructor)) static void
install_mercury(void)
{
    int ends[2];
    pthread_t thread;

    if (!getprogname() || strcmp(getprogname(), "MercuryPosterExtension")) return;
    if (getenv("REDWOOD_EXT_ENV") == NULL && read_env(EXT_ENV_FILE) > 0) {
        char path[1024];
        uint32_t size = sizeof(path);

        setenv("REDWOOD_EXT_ENV", EXT_ENV_FILE, 1);
        if (_NSGetExecutablePath(path, &size) == 0)
            execve(path, *_NSGetArgv(), *_NSGetEnviron());
    }

    install_mercury_hooks();
    _dyld_register_func_for_add_image(mercury_image_callback);

    if (pipe(ends) != 0) return;
    if (pthread_create(&thread, NULL, forward_stderr, (void *)(intptr_t)ends[0]) != 0) {
        close(ends[0]);
        close(ends[1]);
        return;
    }
    pthread_detach(thread);
    dup2(ends[1], STDERR_FILENO);
    close(ends[1]);
    if (getenv("REDWOOD_EXT_ENV") != NULL)
        fprintf(stderr, "pb26: started again with the variables of %s\n", EXT_ENV_FILE);
}


/*
 * Poster snapshots as PNG. PosterUIFoundation writes every snapshot as ATX ("Snapshot-[%@].atx",
 * +[PUIImageOnDiskFormat defaultATX]): ASTC blocks laid out for Apple's GPU. SpringBoard's wallpaper
 * window shows such a snapshot, QuartzCore sends it to backboardd as a block-compressed image, and
 * here its data is empty: SpringBoard asks for a memory entry of 0 bytes (kernel-12377-cmd46-bo34),
 * backboardd answers "CompressedImage port is MACH_PORT_NULL!" and drops the whole commit of the
 * window, the hosted poster scene included. Both ATX formats answer with the PNG one instead; the
 * reader goes by the bytes and not by the file name. PosterUIFoundation is loaded at launch by
 * PosterBoard, SpringBoard and the poster extensions, and otherwise when an image is added.
 */
static id
png_format(id self, SEL cmd)
{
    (void)cmd;
    return ((id (*)(id, SEL))objc_msgSend)(self, sel_registerName("png"));
}

static id
png_format_sized(id self, SEL cmd, long block_size, BOOL twiddling)
{
    (void)block_size;
    (void)twiddling;
    return png_format(self, cmd);
}

static void
use_png_snapshots(const struct mach_header *header, intptr_t slide)
{
    static int done;
    Class format = objc_getClass("PUIImageOnDiskFormat");

    (void)header;
    (void)slide;
    if (done || !format) return;
    done = 1;
    method_setImplementation(class_getClassMethod(format, sel_registerName("defaultATX")),
        (IMP)png_format);
    method_setImplementation(class_getClassMethod(format, sel_registerName("atxWithBlockSize:twiddling:")),
        (IMP)png_format_sized);
    fprintf(stderr, "pb26: %s writes poster snapshots as PNG\n", getprogname());
}

__attribute__((constructor)) static void
install_png_snapshots(void)
{
    _dyld_register_func_for_add_image(use_png_snapshots);
}
