/*
 * Read the current LaunchServices registration and the FBS identity derived from it.
 * Usage: app-identity BUNDLE_ID
 * Build: tools/tests/build-app-identity.sh out/app-identity
 * Host checks: python3 tools/tests/test-app-identity.py
 * Exit 0 means all requested queries completed; 1 means incomplete queries; 2 means invalid input.
 * FBSApplicationInfo is a new snapshot, separate from SpringBoard's cached application objects.
 * Private method signatures were checked against the iOS 26.6.2 / 23G90 shared cache.
 */
#import <Foundation/Foundation.h>
#import <objc/message.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

@interface LSApplicationProxy : NSObject
+ (id)applicationProxyForIdentifier:(NSString *)identifier;
@end

@interface LSApplicationRecord : NSObject
- (id)initWithBundleIdentifier:(NSString *)identifier allowPlaceholder:(BOOL)allow error:(NSError **)error;
- (id)fbs_processIdentityForApplicationIdentity:(id)identity;
@end

@interface FBSApplicationInfo : NSObject
- (id)initWithApplicationProxy:(id)proxy;
@end

static void
problem(NSMutableArray *errors, NSString *field, NSString *reason)
{
    [errors addObject:@{@"field": field, @"reason": reason}];
}

static char
type_code(const char *encoding)
{
    return encoding[strspn(encoding, "rnNoORV")];
}

static BOOL
check_method(id target, NSString *name, char result, const char *arguments, NSMutableArray *errors)
{
    SEL selector = NSSelectorFromString(name);
    if (!target || ![target respondsToSelector:selector]) {
        problem(errors, name, target ? @"selector unavailable" : @"object unavailable");
        return NO;
    }
    NSMethodSignature *signature = [target methodSignatureForSelector:selector];
    char actual = signature ? type_code(signature.methodReturnType) : '\0';
    BOOL matches = signature && signature.numberOfArguments == 2 + strlen(arguments) &&
        (actual == result || (result == 'B' && actual == 'c'));
    for (NSUInteger i = 0; matches && arguments[i]; i++) {
        const char *encoding = [signature getArgumentTypeAtIndex:i + 2];
        actual = type_code(encoding);
        matches = actual == arguments[i] || (arguments[i] == 'B' && actual == 'c');
        // The sole pointer parameter is NSError **, encoded as ^@ in 23G90.
        if (matches && arguments[i] == '^') {
            matches = strcmp(encoding + strspn(encoding, "rnNoORV"), "^@") == 0;
        }
    }
    if (!matches) {
        problem(errors, name, @"method signature differs from the checked ABI");
    }
    return matches;
}

static id
object_value(id target, NSString *name, BOOL nullable, NSMutableArray *errors)
{
    if (!check_method(target, name, '@', "", errors)) return nil;
    @try {
        id value = ((id (*)(id, SEL))objc_msgSend)(target, NSSelectorFromString(name));
        if (!value && !nullable) problem(errors, name, @"query returned nil");
        return value;
    } @catch (NSException *exception) {
        problem(errors, name, [NSString stringWithFormat:@"%@: %@", exception.name, exception.reason]);
        return nil;
    }
}

static id
object_field(id target, NSString *name, BOOL nullable, NSMutableDictionary *output, NSMutableArray *errors)
{
    id value = object_value(target, name, nullable, errors);
    if ([value isKindOfClass:NSString.class]) {
        output[name] = value;
    } else if ([value isKindOfClass:NSURL.class]) {
        output[name] = [value absoluteString];
    } else {
        output[name] = NSNull.null;
        if (value) problem(errors, name, @"query returned an unexpected object type");
    }
    return value;
}

static void
integer_field(id target, NSString *name, char type, NSMutableDictionary *output, NSMutableArray *errors)
{
    output[name] = NSNull.null;
    if (!check_method(target, name, type, "", errors)) return;
    SEL selector = NSSelectorFromString(name);
    @try {
        if (type == 'B') {
            output[name] = @(((BOOL (*)(id, SEL))objc_msgSend)(target, selector));
        } else {
            output[name] = @(((uint64_t (*)(id, SEL))objc_msgSend)(target, selector));
        }
    } @catch (NSException *exception) {
        problem(errors, name, [NSString stringWithFormat:@"%@: %@", exception.name, exception.reason]);
    }
}

static NSDictionary *
identity_fields(id identity, NSMutableArray *errors)
{
    NSMutableDictionary *result = [NSMutableDictionary dictionary];
    if (!identity) return result;
    object_field(identity, @"bundleIdentifier", NO, result, errors);
    object_field(identity, @"identityString", NO, result, errors);
    object_field(identity, @"personaUniqueString", YES, result, errors);
    integer_field(identity, @"personaType", 'Q', result, errors);
    return result;
}

static void
file_fields(id location, NSString *field, NSMutableDictionary *files, NSMutableArray *errors)
{
    NSString *path = nil;
    if ([location isKindOfClass:NSURL.class] && [location isFileURL]) {
        path = [location path];
    } else if ([location isKindOfClass:NSString.class]) {
        path = location;
    }
    if (!path.length) {
        files[field] = NSNull.null;
        problem(errors, field, @"file path unavailable");
        return;
    }
    struct stat info;
    if (stat(path.fileSystemRepresentation, &info) != 0) {
        int saved_errno = errno;
        files[field] = @{@"path": path, @"errno": @(saved_errno)};
        problem(errors, field, [NSString stringWithUTF8String:strerror(saved_errno)]);
        return;
    }
    files[field] = @{@"path": path, @"st_dev": @(info.st_dev), @"st_ino": @(info.st_ino),
                     @"st_mode": @(info.st_mode)};
}

static void
load_framework(const char *path, NSMutableArray *errors)
{
    if (!dlopen(path, RTLD_LAZY | RTLD_LOCAL)) {
        const char *message = dlerror();
        problem(errors, [NSString stringWithUTF8String:path],
                message ? [NSString stringWithUTF8String:message] : @"framework unavailable");
    }
}

static void
inspect(NSString *identifier, NSMutableDictionary *report, NSMutableArray *errors)
{
    NSMutableDictionary *proxy_fields = [NSMutableDictionary dictionary];
    NSMutableDictionary *record_fields = [NSMutableDictionary dictionary];
    NSMutableDictionary *fbs_fields = [NSMutableDictionary dictionary];
    NSMutableDictionary *files = [NSMutableDictionary dictionary];
    [report addEntriesFromDictionary:@{@"bundle_id": identifier, @"proxy": proxy_fields,
        @"record": record_fields, @"fbs": fbs_fields, @"files": files,
        @"caller": @{@"uid": @(getuid()), @"euid": @(geteuid())},
        @"fbs_source": @"new FBSApplicationInfo derived from current LS proxy"}];
    fbs_fields[@"status"] = @"not_queried";
    load_framework("/System/Library/Frameworks/CoreServices.framework/CoreServices", errors);

    Class proxy_class = NSClassFromString(@"LSApplicationProxy");
    id proxy = nil;
    if (check_method(proxy_class, @"applicationProxyForIdentifier:", '@', "@", errors)) {
        proxy = [(id)proxy_class applicationProxyForIdentifier:identifier];
        if (!proxy) problem(errors, @"LSApplicationProxy", @"application was not found");
    }
    BOOL valid_proxy = NO;
    if (proxy) {
        object_field(proxy, @"bundleIdentifier", NO, proxy_fields, errors);
        object_field(proxy, @"applicationType", NO, proxy_fields, errors);
        object_field(proxy, @"bundleExecutable", NO, proxy_fields, errors);
        id bundle = object_field(proxy, @"bundleURL", NO, proxy_fields, errors);
        id executable = object_field(proxy, @"canonicalExecutablePath", NO, proxy_fields, errors);
        for (NSString *name in @[@"isInstalled", @"isPlaceholder", @"isRestricted"]) {
            integer_field(proxy, name, 'B', proxy_fields, errors);
        }
        NSMutableDictionary *state_fields = [NSMutableDictionary dictionary];
        id state = object_value(proxy, @"appState", NO, errors);
        integer_field(state, @"isValid", 'B', state_fields, errors);
        proxy_fields[@"appState"] = state_fields;
        valid_proxy = [proxy_fields[@"isInstalled"] isEqual:@YES] &&
            [state_fields[@"isValid"] isEqual:@YES] &&
            [bundle isKindOfClass:NSURL.class] && [bundle isFileURL] && [bundle path].length;
        if ([proxy_fields[@"isInstalled"] isEqual:@NO]) {
            problem(errors, @"LSApplicationProxy.isInstalled", @"application is not installed");
        }
        file_fields(bundle, @"proxy.bundleURL", files, errors);
        file_fields(executable, @"proxy.canonicalExecutablePath", files, errors);
    }

    id record = [NSClassFromString(@"LSApplicationRecord") alloc];
    if (check_method(record, @"initWithBundleIdentifier:allowPlaceholder:error:", '@', "@B^", errors)) {
        NSError *error = nil;
        record = [(LSApplicationRecord *)record initWithBundleIdentifier:identifier allowPlaceholder:NO error:&error];
        if (!record || error) problem(errors, @"LSApplicationRecord", error.description ?: @"application was not found");
    } else {
        record = nil;
    }
    BOOL valid_identities = NO;
    if (record) {
        object_field(record, @"bundleIdentifier", NO, record_fields, errors);
        id bundle = object_field(record, @"URL", NO, record_fields, errors);
        id executable = object_field(record, @"executableURL", NO, record_fields, errors);
        integer_field(record, @"bundleInode", 'Q', record_fields, errors);
        integer_field(record, @"executableInode", 'Q', record_fields, errors);
        integer_field(record, @"isPlaceholder", 'B', record_fields, errors);
        file_fields(bundle, @"record.URL", files, errors);
        file_fields(executable, @"record.executableURL", files, errors);
        id identities = object_value(record, @"identities", NO, errors);
        if ([identities isKindOfClass:NSArray.class]) {
            NSMutableArray *items = [NSMutableArray array];
            for (id identity in identities) [items addObject:identity_fields(identity, errors)];
            record_fields[@"identities"] = items;
            valid_identities = items.count > 0;
            if (!items.count) problem(errors, @"LSApplicationRecord.identities", @"query returned an empty array");
        } else {
            record_fields[@"identities"] = NSNull.null;
            if (identities) problem(errors, @"LSApplicationRecord.identities", @"query returned an unexpected object type");
        }
    }

    load_framework("/System/Library/PrivateFrameworks/FrontBoardServices.framework/FrontBoardServices", errors);
    id fbs_record = nil;
    id fbs_identity = nil;
    id fbs_process = nil;
    if (valid_proxy && record && valid_identities) {
        fbs_record = object_value(proxy, @"fbs_correspondingApplicationRecord", NO, errors);
        if (fbs_record) {
            NSMutableDictionary *corresponding = [NSMutableDictionary dictionary];
            object_field(fbs_record, @"bundleIdentifier", NO, corresponding, errors);
            object_field(fbs_record, @"URL", NO, corresponding, errors);
            fbs_fields[@"correspondingRecord"] = corresponding;
            id identities = object_value(fbs_record, @"identities", NO, errors);
            if ([identities isKindOfClass:NSArray.class] && [identities count]) {
                fbs_identity = [identities firstObject];
            } else {
                problem(errors, @"FBSApplicationInfo.identities", @"corresponding record has no identity array with an entry");
            }
            if (fbs_identity && check_method(fbs_record, @"fbs_processIdentityForApplicationIdentity:", '@', "@", errors)) {
                fbs_process = [(LSApplicationRecord *)fbs_record fbs_processIdentityForApplicationIdentity:fbs_identity];
                if (!fbs_process) problem(errors, @"FBSApplicationInfo.processIdentity", @"query returned nil");
            }
        }
    }
    id info = [NSClassFromString(@"FBSApplicationInfo") alloc];
    // Native FBS asserts on invalid LS input, including a nonnil unregistered proxy.
    if (!valid_proxy || !record || !valid_identities || !fbs_identity || !fbs_process) {
        info = nil;
        fbs_fields[@"status"] = @"skipped";
        problem(errors, @"FBSApplicationInfo", @"requires an installed, valid LS proxy with a file bundle URL, application records and application/process identities");
    } else if (check_method(info, @"initWithApplicationProxy:", '@', "@", errors)) {
        info = [(FBSApplicationInfo *)info initWithApplicationProxy:proxy];
        if (!info) problem(errors, @"FBSApplicationInfo", @"initialization returned nil");
        fbs_fields[@"status"] = info ? @"queried" : @"initialization_failed";
    } else {
        info = nil;
        fbs_fields[@"status"] = @"api_unavailable";
    }
    if (info) {
        integer_field(info, @"type", 'Q', fbs_fields, errors);
        id executable = object_field(info, @"executableURL", NO, fbs_fields, errors);
        file_fields(executable, @"fbs.executableURL", files, errors);
        id identity = object_value(info, @"applicationIdentity", NO, errors);
        fbs_fields[@"applicationIdentity"] = identity ? identity_fields(identity, errors) : NSNull.null;
        id process = object_value(info, @"processIdentity", NO, errors);
        NSMutableDictionary *process_fields = [NSMutableDictionary dictionary];
        if (process) {
            object_field(process, @"description", NO, process_fields, errors);
            object_field(process, @"embeddedApplicationIdentifier", YES, process_fields, errors);
            object_field(process, @"personaString", YES, process_fields, errors);
            object_field(process, @"applicationJobLabel", YES, process_fields, errors);
        }
        fbs_fields[@"processIdentity"] = process ? process_fields : NSNull.null;
    }
}

int
main(int argc, char **argv)
{
    @autoreleasepool {
        if (argc != 2 || !argv[1][0] || !strcmp(argv[1], "--help")) {
            fprintf(stderr, "usage: app-identity BUNDLE_ID\n");
            return argc == 2 && !strcmp(argv[1], "--help") ? 0 : 2;
        }
        NSString *identifier = [NSString stringWithUTF8String:argv[1]];
        if (!identifier.length) {
            fprintf(stderr, "app-identity: bundle ID must be nonempty UTF-8\n");
            return 2;
        }
        NSMutableArray *errors = [NSMutableArray array];
        NSMutableDictionary *report = [NSMutableDictionary dictionary];
        @try {
            inspect(identifier, report, errors);
        } @catch (NSException *exception) {
            problem(errors, @"query", [NSString stringWithFormat:@"%@: %@", exception.name, exception.reason]);
        }
        report[@"errors"] = errors;
        report[@"query_complete"] = errors.count ? @NO : @YES;
        NSError *error = nil;
        NSData *json = [NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingPrettyPrinted | NSJSONWritingSortedKeys error:&error];
        if (!json) {
            fprintf(stderr, "app-identity: JSON encoding failed: %s\n", error.description.UTF8String);
            return 1;
        }
        fwrite(json.bytes, 1, json.length, stdout);
        fputc('\n', stdout);
        return errors.count ? 1 : 0;
    }
}
