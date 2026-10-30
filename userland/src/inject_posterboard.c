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
#include <dlfcn.h>
#include <execinfo.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

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

/* NSURLFileProtectionKey, looked up because this library does not link Foundation. */
static id
without_protection(id values)
{
    static id *key;
    if (!key) key = dlsym(RTLD_DEFAULT, "NSURLFileProtectionKey");
    if (!values || !key || !*key) return values;
    id copy = ((id (*)(id, SEL))objc_msgSend)(values, sel_registerName("mutableCopy"));
    ((void (*)(id, SEL, id))objc_msgSend)(copy, sel_registerName("removeObjectForKey:"), *key);
    return ((id (*)(id, SEL))objc_msgSend)(copy, sel_registerName("autorelease"));
}

static BOOL (*reachable_conform)(id, SEL, id, id *), (*conforms)(id, SEL, id, id *);
static BOOL (*update_values)(id, SEL, id, id *), (*recursive_update)(id, SEL, id, id *);

static BOOL
resource_call(BOOL (*original)(id, SEL, id, id *), unsigned *calls, id self, SEL cmd, id values, id *error)
{
    BOOL ok = original(self, cmd, without_protection(values), error);
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
