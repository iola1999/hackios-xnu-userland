/*
 * appinst: installs an application on this boot from an .ipa or an .app directory.
 *
 * The application is unpacked into a bundle container of its own under
 * /private/var/containers/Bundle/Application, containermanagerd makes its data container, and
 * LaunchServices registers it, which puts the icon on the home screen. installd takes no part, so no
 * provisioning profile is asked for: the kernel's AMFI stand-in (bsd/kern/kern_minimac.c) runs every
 * ad hoc signed program. /private/var is rebuilt at every boot, so whatever is installed here is gone
 * after a reboot.
 *
 * usage: appinst install FILE.ipa|DIR.app
 *        appinst remove BUNDLE-ID
 *        appinst list
 *        appinst open BUNDLE-ID     (launches it, as a tap on its icon does)
 *        appinst url URL            (opens it in the application that handles it, Safari for http)
 *        appinst watch              (installs each .ipa that appears in the download folders and in
 *                                    /private/var/tmp/ipa, where tools/k90auto/install-ipa.py puts one)
 *
 * The registration is TrollStore's (registerApplicationDictionary: with the keys installd would have
 * written), with ApplicationType System so that FrontBoard does not look for a profile at launch.
 * Needs the entitlements in appinst.entitlements; run it as root.
 *
 * Built into out/ios26-resume/appinst/appinst, where tools/k90auto/cycle.py packs it from:
 *   xcrun -sdk iphoneos clang -arch arm64e -miphoneos-version-min=26.0 -Os -fobjc-arc \
 *       -framework Foundation -larchive userland/src/appinst.m -o out/ios26-resume/appinst/appinst
 *   codesign --force --sign - --entitlements userland/src/appinst.entitlements out/ios26-resume/appinst/appinst
 */
#import <Foundation/Foundation.h>
#include <dlfcn.h>
#include <stdio.h>
#include <sys/stat.h>

/* libarchive, in the shared cache; the iOS SDK has the library and no header */
struct archive;
struct archive_entry;
struct archive *archive_read_new(void);
int archive_read_support_format_zip(struct archive *);
int archive_read_open_filename(struct archive *, const char *, size_t);
int archive_read_next_header(struct archive *, struct archive_entry **);
int archive_read_data_skip(struct archive *);
int archive_read_extract(struct archive *, struct archive_entry *, int);
const char *archive_error_string(struct archive *);
int archive_read_free(struct archive *);
const char *archive_entry_pathname(struct archive_entry *);
void archive_entry_set_pathname(struct archive_entry *, const char *);
#define ARCHIVE_OK                      0
#define ARCHIVE_EXTRACT_PERM            0x0002
#define ARCHIVE_EXTRACT_TIME            0x0004
#define ARCHIVE_EXTRACT_SECURE_SYMLINKS 0x0100
#define ARCHIVE_EXTRACT_SECURE_NODOTDOT 0x0200

@interface LSApplicationWorkspace : NSObject
+ (instancetype)defaultWorkspace;
- (BOOL)registerApplicationDictionary:(NSDictionary *)dictionary;
- (BOOL)unregisterApplication:(NSURL *)url;
- (BOOL)openApplicationWithBundleID:(NSString *)identifier;
- (BOOL)openURL:(NSURL *)url;
@end

@interface LSApplicationProxy : NSObject
+ (instancetype)applicationProxyForIdentifier:(NSString *)identifier;
- (NSURL *)bundleURL;
- (NSString *)applicationType;
- (NSString *)applicationIdentifier;
@end

@interface MCMContainer : NSObject
+ (instancetype)containerWithIdentifier:(NSString *)identifier createIfNecessary:(BOOL)create existed:(BOOL *)existed
                                  error:(NSError **)error;
- (NSURL *)url;
@end

static NSString *const BUNDLES = @"/private/var/containers/Bundle/Application";
/* where tools/k90auto/install-ipa.py writes an .ipa over USB, in pieces (rwsh "pull PATH OFFSET") */
static NSString *const INBOX = @"/private/var/tmp/ipa";

/* the entries under Payload/ of the archive, into DEST (Payload/ taken off); NO and said why otherwise */
static BOOL
unpack(NSString *ipa, NSString *dest)
{
	struct archive *a = archive_read_new();
	struct archive_entry *entry;
	int flags = ARCHIVE_EXTRACT_PERM | ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_SECURE_SYMLINKS | ARCHIVE_EXTRACT_SECURE_NODOTDOT;
	BOOL ok = YES;
	unsigned files = 0;

	archive_read_support_format_zip(a);
	if (archive_read_open_filename(a, ipa.fileSystemRepresentation, 1 << 16) != ARCHIVE_OK) {
		fprintf(stderr, "appinst: %s: %s\n", ipa.UTF8String, archive_error_string(a));
		archive_read_free(a);
		return NO;
	}
	while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
		NSString *path = @(archive_entry_pathname(entry));

		if (![path hasPrefix:@"Payload/"] || path.length == 8) {
			archive_read_data_skip(a);
			continue;
		}
		archive_entry_set_pathname(entry, [dest stringByAppendingPathComponent:[path substringFromIndex:8]].fileSystemRepresentation);
		if (archive_read_extract(a, entry, flags) != ARCHIVE_OK) {
			fprintf(stderr, "appinst: %s: %s\n", path.UTF8String, archive_error_string(a));
			ok = NO;
			break;
		}
		files++;
	}
	archive_read_free(a);
	printf("appinst: %u files unpacked\n", files);
	return ok && files > 0;
}

static NSString *
app_in(NSString *dir)
{
	for (NSString *name in [NSFileManager.defaultManager contentsOfDirectoryAtPath:dir error:nil]) {
		if ([name.pathExtension isEqualToString:@"app"]) {
			return [dir stringByAppendingPathComponent:name];
		}
	}
	return nil;
}

/* the application extensions inside, as LaunchServices wants them listed with their bundle */
static NSDictionary *
plugins_of(NSString *app)
{
	NSMutableDictionary *plugins = [NSMutableDictionary dictionary];
	NSString *dir = [app stringByAppendingPathComponent:@"PlugIns"];

	for (NSString *name in [NSFileManager.defaultManager contentsOfDirectoryAtPath:dir error:nil]) {
		NSString *path = [dir stringByAppendingPathComponent:name];
		NSDictionary *info = [NSDictionary dictionaryWithContentsOfFile:[path stringByAppendingPathComponent:@"Info.plist"]];
		NSString *identifier = info[@"CFBundleIdentifier"];

		if (identifier == nil) {
			continue;
		}
		plugins[identifier] = @{ @"ApplicationType": @"PluginKitPlugin", @"CFBundleIdentifier": identifier,
		                         @"CodeInfoIdentifier": identifier, @"Path": path, @"IsAdHocSigned": @YES,
		                         @"SignerOrganization": @"Apple Inc.", @"SignerIdentity": @"Apple iPhone OS Application Signing",
		                         @"PluginOwnerBundleID": [NSDictionary dictionaryWithContentsOfFile:
		                             [app stringByAppendingPathComponent:@"Info.plist"]][@"CFBundleIdentifier"] ?: @"" };
	}
	return plugins;
}

static int
install_into(NSString *source, NSString *bundle)
{
	NSFileManager *fm = NSFileManager.defaultManager;
	NSString *app;
	NSDictionary *info;
	NSError *error = nil;

	if (![fm createDirectoryAtPath:bundle withIntermediateDirectories:YES attributes:@{ NSFilePosixPermissions: @0755 } error:&error]) {
		fprintf(stderr, "appinst: %s: %s\n", bundle.UTF8String, error.description.UTF8String);
		return 1;
	}
	if ([source.pathExtension isEqualToString:@"app"]) {
		if (![fm copyItemAtPath:source toPath:[bundle stringByAppendingPathComponent:source.lastPathComponent] error:&error]) {
			fprintf(stderr, "appinst: copying %s: %s\n", source.UTF8String, error.description.UTF8String);
			return 1;
		}
	} else if (!unpack(source, bundle)) {
		return 1;
	}
	app = app_in(bundle);
	info = [NSDictionary dictionaryWithContentsOfFile:[app stringByAppendingPathComponent:@"Info.plist"]];
	if (app == nil || info[@"CFBundleIdentifier"] == nil || info[@"CFBundleExecutable"] == nil) {
		fprintf(stderr, "appinst: no application with an Info.plist naming its identifier and executable in %s\n", source.UTF8String);
		return 1;
	}
	NSString *identifier = info[@"CFBundleIdentifier"];
	chmod([app stringByAppendingPathComponent:info[@"CFBundleExecutable"]].fileSystemRepresentation, 0755);

	/* an earlier copy on this boot goes first, or LaunchServices keeps the old path */
	LSApplicationProxy *old = [NSClassFromString(@"LSApplicationProxy") applicationProxyForIdentifier:identifier];
	if (old.bundleURL != nil && [old.bundleURL.path hasPrefix:BUNDLES]) {
		[[NSClassFromString(@"LSApplicationWorkspace") defaultWorkspace] unregisterApplication:old.bundleURL];
		[fm removeItemAtPath:old.bundleURL.path.stringByDeletingLastPathComponent error:nil];
	}

	dlopen("/System/Library/PrivateFrameworks/MobileContainerManager.framework/MobileContainerManager", RTLD_NOW);
	MCMContainer *data = [NSClassFromString(@"MCMAppDataContainer") containerWithIdentifier:identifier createIfNecessary:YES
	                                                                               existed:NULL error:&error];
	NSString *home = data.url.path;
	if (home == nil) {
		fprintf(stderr, "appinst: no data container for %s: %s\n", identifier.UTF8String, error.description.UTF8String);
		return 1;
	}
	NSDictionary *record = @{
		@"ApplicationType": @"System",
		@"CFBundleIdentifier": identifier,
		@"CodeInfoIdentifier": identifier,
		@"CompatibilityState": @0,
		@"Container": home,
		@"IsContainerized": @YES,
		@"EnvironmentVariables": @{ @"CFFIXED_USER_HOME": home, @"HOME": home, @"TMPDIR": [home stringByAppendingPathComponent:@"tmp"] },
		@"IsDeletable": @NO,
		@"Path": app,
		@"IsAdHocSigned": @YES,
		@"LSInstallType": @1,
		@"HasMIDBasedSINF": @0,
		@"MissingSINF": @0,
		@"FamilyID": @0,
		@"IsOnDemandInstallCapable": @0,
		@"SignerOrganization": @"Apple Inc.",
		@"SignatureVersion": @132352,
		@"SignerIdentity": @"Apple iPhone OS Application Signing",
		@"_LSBundlePlugins": plugins_of(app),
	};
	if (![[NSClassFromString(@"LSApplicationWorkspace") defaultWorkspace] registerApplicationDictionary:record]) {
		fprintf(stderr, "appinst: LaunchServices refused %s\n", identifier.UTF8String);
		return 1;
	}
	printf("appinst: %s installed at %s, data in %s\n", identifier.UTF8String, app.UTF8String, home.UTF8String);
	return 0;
}

/* a failed install leaves no bundle container behind: /private/var is in memory */
static int
install(NSString *source)
{
	NSString *bundle = [BUNDLES stringByAppendingPathComponent:NSUUID.UUID.UUIDString];
	int status = install_into(source, bundle);

	if (status != 0) {
		[NSFileManager.defaultManager removeItemAtPath:bundle error:nil];
	}
	return status;
}

static void
add_dirs_in(NSMutableArray<NSString *> *dirs, NSString *parent, NSString *below)
{
	for (NSString *name in [NSFileManager.defaultManager contentsOfDirectoryAtPath:parent error:nil]) {
		NSString *dir = below ? [NSString pathWithComponents:@[ parent, name, below ]] : [parent stringByAppendingPathComponent:name];
		BOOL is_dir = NO;

		if ([NSFileManager.defaultManager fileExistsAtPath:dir isDirectory:&is_dir] && is_dir) {
			[dirs addObject:dir];
		}
	}
}

/*
 * Safari saves to "On My iPhone > Downloads", which is File Provider Storage/Downloads in the app group
 * of the FileProvider local storage; the group's directory has a new UUID on every data volume. On this
 * system that folder is never made (kernel-12377-rate8-app1), and the download stays in a directory of
 * its own under Library/Safari/Downloads of Safari's data container, so those are watched as well.
 */
static NSArray<NSString *> *
download_dirs(void)
{
	NSMutableArray<NSString *> *dirs = [NSMutableArray array];
	MCMContainer *safari = [NSClassFromString(@"MCMAppDataContainer") containerWithIdentifier:@"com.apple.mobilesafari"
	                                                                         createIfNecessary:NO existed:NULL error:NULL];

	[dirs addObject:INBOX];
	add_dirs_in(dirs, @"/private/var/mobile/Containers/Shared/AppGroup", @"File Provider Storage/Downloads");
	if (safari.url != nil) {
		add_dirs_in(dirs, [safari.url.path stringByAppendingPathComponent:@"Library/Safari/Downloads"], nil);
	}
	return dirs;
}

/* whether the file ends with a zip archive's end record: the download is whole */
static BOOL
zip_whole(NSString *path, unsigned long long size)
{
	NSFileHandle *f = [NSFileHandle fileHandleForReadingAtPath:path];
	unsigned long long tail = size < 65557 ? size : 65557;	/* the end record and the longest comment */
	NSData *end;

	if (f == nil || size < 22) {
		return NO;
	}
	[f seekToFileOffset:size - tail];
	end = [f readDataOfLength:(NSUInteger)tail];
	[f closeFile];
	return [end rangeOfData:[NSData dataWithBytes:"PK\x05\x06" length:4] options:NSDataSearchBackwards
	                  range:NSMakeRange(0, end.length)].location != NSNotFound;
}

/*
 * Every second, each .ipa in the download folders that is a whole zip archive is copied out and
 * installed from the copy, once for each size and time it has: Safari gives up on a download it
 * cannot hand to the FileProvider storage and may remove it at any time.
 */
static int
watch(void)
{
	NSFileManager *fm = NSFileManager.defaultManager;
	NSMutableSet<NSString *> *done = [NSMutableSet set];
	NSUInteger folders = NSUIntegerMax;

	setvbuf(stdout, NULL, _IOLBF, 0);
	dlopen("/System/Library/PrivateFrameworks/MobileContainerManager.framework/MobileContainerManager", RTLD_NOW);
	[fm createDirectoryAtPath:INBOX withIntermediateDirectories:YES attributes:@{ NSFilePosixPermissions: @0777 } error:nil];
	for (;;) {
		@autoreleasepool {
			NSArray<NSString *> *dirs = download_dirs();

			if (dirs.count != folders) {
				folders = dirs.count;
				printf("appinst: watching %lu download folder(s)\n", (unsigned long)folders);
			}
			for (NSString *dir in dirs) {
				for (NSString *name in [fm contentsOfDirectoryAtPath:dir error:nil]) {
					NSString *path = [dir stringByAppendingPathComponent:name];
					NSDictionary *attributes = [fm attributesOfItemAtPath:path error:nil];
					unsigned long long size = [attributes[NSFileSize] unsignedLongLongValue];
					NSString *key = [NSString stringWithFormat:@"%@ %llu %@", path, size, attributes[NSFileModificationDate]];
					NSString *copy = [NSString stringWithFormat:@"/private/var/tmp/appinst-%@", name];

					if (![name.pathExtension.lowercaseString isEqualToString:@"ipa"] || [done containsObject:key] ||
					    !zip_whole(path, size)) {
						continue;
					}
					[done addObject:key];
					[fm removeItemAtPath:copy error:nil];
					if (![fm copyItemAtPath:path toPath:copy error:nil]) {
						printf("appinst: %s went away before it was copied\n", name.UTF8String);
						continue;
					}
					printf("appinst: installing %s (%llu bytes) from %s\n", name.UTF8String, size, dir.UTF8String);
					printf("appinst: %s\n", install(copy) == 0 ? "installed" : "NOT INSTALLED");
					[fm removeItemAtPath:copy error:nil];
				}
			}
		}
		sleep(1);
	}
}

static int
remove_app(NSString *identifier)
{
	LSApplicationProxy *proxy = [NSClassFromString(@"LSApplicationProxy") applicationProxyForIdentifier:identifier];

	if (proxy.bundleURL == nil || ![proxy.bundleURL.path hasPrefix:BUNDLES]) {
		fprintf(stderr, "appinst: %s was not installed by appinst\n", identifier.UTF8String);
		return 1;
	}
	[[NSClassFromString(@"LSApplicationWorkspace") defaultWorkspace] unregisterApplication:proxy.bundleURL];
	[NSFileManager.defaultManager removeItemAtPath:proxy.bundleURL.path.stringByDeletingLastPathComponent error:nil];
	printf("appinst: %s removed\n", identifier.UTF8String);
	return 0;
}

static int
list(void)
{
	for (NSString *uuid in [NSFileManager.defaultManager contentsOfDirectoryAtPath:BUNDLES error:nil]) {
		NSString *app = app_in([BUNDLES stringByAppendingPathComponent:uuid]);
		NSDictionary *info = [NSDictionary dictionaryWithContentsOfFile:[app stringByAppendingPathComponent:@"Info.plist"]];

		if (info != nil) {
			printf("%s\t%s\n", [info[@"CFBundleIdentifier"] UTF8String], app.UTF8String);
		}
	}
	return 0;
}

int
main(int argc, char **argv)
{
	@autoreleasepool {
		if (argc == 3 && strcmp(argv[1], "install") == 0) {
			return install(@(argv[2]));
		}
		if (argc == 3 && strcmp(argv[1], "remove") == 0) {
			return remove_app(@(argv[2]));
		}
		if (argc == 2 && strcmp(argv[1], "list") == 0) {
			return list();
		}
		if (argc == 3 && strcmp(argv[1], "open") == 0) {
			BOOL opened = [[NSClassFromString(@"LSApplicationWorkspace") defaultWorkspace] openApplicationWithBundleID:@(argv[2])];
			printf("appinst: %s %s\n", argv[2], opened ? "opened" : "NOT OPENED");
			return opened ? 0 : 1;
		}
		if (argc == 3 && strcmp(argv[1], "url") == 0) {
			BOOL opened = [[NSClassFromString(@"LSApplicationWorkspace") defaultWorkspace] openURL:[NSURL URLWithString:@(argv[2])]];
			printf("appinst: %s %s\n", argv[2], opened ? "opened" : "NOT OPENED");
			return opened ? 0 : 1;
		}
		if (argc == 2 && strcmp(argv[1], "watch") == 0) {
			return watch();
		}
		fprintf(stderr, "usage: appinst install FILE.ipa|DIR.app | remove BUNDLE-ID | list | open BUNDLE-ID | url URL | watch\n");
		return 2;
	}
}
