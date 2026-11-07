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
#include <arpa/inet.h>
#include <CommonCrypto/CommonDigest.h>
#include <dlfcn.h>
#include <mach-o/loader.h>
#include <notify.h>
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
- (BOOL)uninstallApplication:(NSString *)bundleIdentifier withOptions:(NSDictionary *)options;
- (BOOL)openApplicationWithBundleID:(NSString *)identifier;
- (BOOL)openURL:(NSURL *)url;
- (NSArray *)allApplications;
- (NSArray *)allInstalledApplications;
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
/* where tools/k90auto/install-ipa.py writes an .ipa over USB */
static NSString *const INBOX = @"/private/var/db/appinst-inbox";
static NSString *const LEGACY_INBOX = @"/private/var/tmp/ipa";

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

static BOOL
sign_macho(NSString *path, NSDictionary *extraEntitlements)
{
	NSMutableData *data = [NSMutableData dataWithContentsOfFile:path];
	if (data == nil || data.length < sizeof(struct mach_header_64)) {
		return NO;
	}
	uint32_t magic = *(const uint32_t *)data.bytes;
	if (magic != MH_MAGIC_64) {
		return NO;
	}
	struct mach_header_64 *mh = (struct mach_header_64 *)data.mutableBytes;
	uint32_t at = sizeof(struct mach_header_64);
	uint32_t cs_cmd_offset = 0;
	uint32_t linkedit_cmd_offset = 0;
	uint32_t first_fileoff = (uint32_t)data.length;
	uint32_t orig_dataoff = 0;
	uint32_t orig_datasize = 0;

	for (uint32_t i = 0; i < mh->ncmds; i++) {
		struct load_command *lc = (struct load_command *)(data.mutableBytes + at);
		if (lc->cmd == LC_SEGMENT_64) {
			struct segment_command_64 *seg = (struct segment_command_64 *)lc;
			if (strcmp(seg->segname, "__LINKEDIT") == 0) {
				linkedit_cmd_offset = at;
			}
			if (seg->fileoff > 0 && seg->fileoff < first_fileoff) {
				first_fileoff = (uint32_t)seg->fileoff;
			}
		} else if (lc->cmd == 0x1d) { /* LC_CODE_SIGNATURE */
			cs_cmd_offset = at;
			struct linkedit_data_command *cs = (struct linkedit_data_command *)lc;
			orig_dataoff = cs->dataoff;
			orig_datasize = cs->datasize;
		}
		at += lc->cmdsize;
	}
	if (linkedit_cmd_offset == 0) {
		return NO;
	}

	NSMutableDictionary *ent = [NSMutableDictionary dictionary];
	if (cs_cmd_offset != 0 && orig_dataoff + orig_datasize <= data.length && orig_datasize >= 12) {
		const uint8_t *sb_bytes = (const uint8_t *)data.bytes + orig_dataoff;
		uint32_t sb_magic = ntohl(*(const uint32_t *)sb_bytes);
		uint32_t sb_count = ntohl(*(const uint32_t *)(sb_bytes + 8));
		if (sb_magic == 0xfade0cc0 && 12 + sb_count * 8 <= orig_datasize) {
			for (uint32_t i = 0; i < sb_count; i++) {
				uint32_t stype = ntohl(*(const uint32_t *)(sb_bytes + 12 + i * 8));
				uint32_t soff = ntohl(*(const uint32_t *)(sb_bytes + 16 + i * 8));
				if (stype == 5 && soff + 8 <= orig_datasize) {
					uint32_t emagic = ntohl(*(const uint32_t *)(sb_bytes + soff));
					uint32_t elen = ntohl(*(const uint32_t *)(sb_bytes + soff + 4));
					if (emagic == 0xfade7171 && elen >= 8 && soff + elen <= orig_datasize) {
						NSData *exData = [NSData dataWithBytes:sb_bytes + soff + 8 length:elen - 8];
						NSDictionary *existing = [NSPropertyListSerialization propertyListWithData:exData
						                                                                   options:0
						                                                                    format:NULL
						                                                                     error:nil];
						if ([existing isKindOfClass:[NSDictionary class]]) {
							[ent addEntriesFromDictionary:existing];
						}
					}
				}
			}
		}
	}

	ent[@"get-task-allow"] = @YES;
	ent[@"com.apple.private.security.no-sandbox"] = @YES;

	NSMutableSet<NSString *> *groups = [NSMutableSet setWithObjects:
		@"group.com.SideStore.SideStore",
		@"group.com.rileytestut.AltStore", nil];
	id existingGroups = ent[@"com.apple.security.application-groups"];
	if ([existingGroups isKindOfClass:[NSArray class]]) {
		for (id g in (NSArray *)existingGroups) {
			if ([g isKindOfClass:[NSString class]]) {
				[groups addObject:g];
			}
		}
	}
	ent[@"com.apple.security.application-groups"] = [groups allObjects];
	if (extraEntitlements != nil) {
		[ent addEntriesFromDictionary:extraEntitlements];
	}

	NSData *xml = [NSPropertyListSerialization dataWithPropertyList:ent
	                                                         format:NSPropertyListXMLFormat_v1_0
	                                                        options:0
	                                                          error:nil];
	if (xml == nil) return NO;

	uint32_t ent_blob_len = 8 + (uint32_t)xml.length;
	NSMutableData *ent_blob = [NSMutableData dataWithLength:ent_blob_len];
	uint32_t *eh = (uint32_t *)ent_blob.mutableBytes;
	eh[0] = htonl(0xfade7171);
	eh[1] = htonl(ent_blob_len);
	memcpy(ent_blob.mutableBytes + 8, xml.bytes, xml.length);

	uint8_t ent_hash[32];
	CC_SHA256(ent_blob.bytes, (CC_LONG)ent_blob.length, ent_hash);

	uint32_t req_blob[3] = { htonl(0xfade0c01), htonl(12), 0 };
	uint8_t req_hash[32];
	CC_SHA256(req_blob, 12, req_hash);

	uint32_t sig_blob[2] = { htonl(0xfade0b01), htonl(8) };

	uint32_t codeLimit = (cs_cmd_offset != 0) ? orig_dataoff : (uint32_t)data.length;
	if (data.length > codeLimit) {
		[data setLength:codeLimit];
	}
	while (codeLimit % 16 != 0) {
		[data appendBytes:"\0" length:1];
		codeLimit++;
	}

	uint32_t nCodeSlots = (codeLimit + 4095) / 4096;
	uint32_t nSpecialSlots = 5;
	const char *ident = [path.lastPathComponent.stringByDeletingPathExtension UTF8String];
	if (!ident || strlen(ident) == 0) ident = "binary";
	uint32_t ident_offset = 44;
	uint32_t hash_offset = ident_offset + (uint32_t)strlen(ident) + 1;
	while (hash_offset % 4 != 0) hash_offset++;
	uint32_t actual_hash_offset = hash_offset + nSpecialSlots * 32;
	uint32_t cd_length = actual_hash_offset + nCodeSlots * 32;

	NSMutableData *cd_data = [NSMutableData dataWithLength:cd_length];
	uint32_t *cd_words = (uint32_t *)cd_data.mutableBytes;
	cd_words[0] = htonl(0xfade0c02);
	cd_words[1] = htonl(cd_length);
	cd_words[2] = htonl(0x20100);
	cd_words[3] = htonl(0x00000006);
	cd_words[4] = htonl(actual_hash_offset);
	cd_words[5] = htonl(ident_offset);
	cd_words[6] = htonl(nSpecialSlots);
	cd_words[7] = htonl(nCodeSlots);
	cd_words[8] = htonl(codeLimit);
	uint8_t *cd_bytes = (uint8_t *)cd_data.mutableBytes;
	cd_bytes[36] = 32;
	cd_bytes[37] = 2;
	cd_bytes[38] = 0;
	cd_bytes[39] = 12;
	memcpy(cd_bytes + ident_offset, ident, strlen(ident) + 1);

	memcpy(cd_bytes + actual_hash_offset - 2 * 32, req_hash, 32);
	memcpy(cd_bytes + actual_hash_offset - 5 * 32, ent_hash, 32);

	for (uint32_t p = 0; p < nCodeSlots; p++) {
		uint32_t pstart = p * 4096;
		uint32_t plen = (pstart + 4096 <= codeLimit) ? 4096 : (codeLimit - pstart);
		CC_SHA256((const uint8_t *)data.bytes + pstart, plen, cd_bytes + actual_hash_offset + p * 32);
	}

	uint32_t sb_index_size = 12 + 4 * 8;
	uint32_t cd_offset = (sb_index_size + 15) & ~15;
	uint32_t req_offset = (cd_offset + (uint32_t)cd_data.length + 15) & ~15;
	uint32_t ent_offset_val = (req_offset + 12 + 15) & ~15;
	uint32_t sig_offset = (ent_offset_val + (uint32_t)ent_blob.length + 15) & ~15;
	uint32_t sb_total_size = (sig_offset + 8 + 15) & ~15;

	NSMutableData *sb = [NSMutableData dataWithLength:sb_total_size];
	uint32_t *sb_words = (uint32_t *)sb.mutableBytes;
	sb_words[0] = htonl(0xfade0cc0);
	sb_words[1] = htonl(sb_total_size);
	sb_words[2] = htonl(4);
	sb_words[3] = htonl(0);
	sb_words[4] = htonl(cd_offset);
	sb_words[5] = htonl(2);
	sb_words[6] = htonl(req_offset);
	sb_words[7] = htonl(5);
	sb_words[8] = htonl(ent_offset_val);
	sb_words[9] = htonl(0x10000);
	sb_words[10] = htonl(sig_offset);

	memcpy(sb.mutableBytes + cd_offset, cd_data.bytes, cd_data.length);
	memcpy(sb.mutableBytes + req_offset, req_blob, 12);
	memcpy(sb.mutableBytes + ent_offset_val, ent_blob.bytes, ent_blob.length);
	memcpy(sb.mutableBytes + sig_offset, sig_blob, 8);

	[data appendData:sb];

	mh = (struct mach_header_64 *)data.mutableBytes;
	struct segment_command_64 *linkedit_cmd = (struct segment_command_64 *)(data.mutableBytes + linkedit_cmd_offset);

	if (cs_cmd_offset != 0) {
		struct linkedit_data_command *cs_cmd = (struct linkedit_data_command *)(data.mutableBytes + cs_cmd_offset);
		cs_cmd->dataoff = codeLimit;
		cs_cmd->datasize = sb_total_size;
	} else {
		if (first_fileoff < sizeof(struct mach_header_64) + mh->sizeofcmds + sizeof(struct linkedit_data_command)) {
			return NO;
		}
		struct linkedit_data_command *new_cs = (struct linkedit_data_command *)(data.mutableBytes + sizeof(struct mach_header_64) + mh->sizeofcmds);
		memset(new_cs, 0, sizeof(*new_cs));
		new_cs->cmd = 0x1d;
		new_cs->cmdsize = sizeof(*new_cs);
		new_cs->dataoff = codeLimit;
		new_cs->datasize = sb_total_size;

		mh->ncmds += 1;
		mh->sizeofcmds += sizeof(*new_cs);
	}

	linkedit_cmd->filesize = (codeLimit + sb_total_size) - linkedit_cmd->fileoff;
	linkedit_cmd->vmsize = (linkedit_cmd->filesize + 0x3fff) & ~0x3fff;

	if (![data writeToFile:path atomically:YES]) {
		return NO;
	}
	chmod(path.fileSystemRepresentation, 0755);
	return YES;
}

static void
collect_machos(NSString *dir, NSMutableArray<NSString *> *machos)
{
	NSFileManager *fm = NSFileManager.defaultManager;
	for (NSString *name in [fm contentsOfDirectoryAtPath:dir error:nil]) {
		NSString *path = [dir stringByAppendingPathComponent:name];
		BOOL isDir = NO;
		if ([fm fileExistsAtPath:path isDirectory:&isDir]) {
			if (isDir) {
				collect_machos(path, machos);
			} else {
				NSFileHandle *fh = [NSFileHandle fileHandleForReadingAtPath:path];
				if (fh) {
					NSData *magicData = [fh readDataOfLength:4];
					[fh closeFile];
					if (magicData.length == 4) {
						uint32_t m = *(const uint32_t *)magicData.bytes;
						if (m == 0xfeedfacf || m == 0xfeedface || m == 0xcafebabe || m == 0xcafebabf) {
							[machos addObject:path];
						}
					}
				}
			}
		}
	}
}

static void
sign_all_machos(NSString *appDir, NSDictionary *extraEntitlements)
{
	NSMutableArray<NSString *> *machos = [NSMutableArray array];
	collect_machos(appDir, machos);
	printf("appinst: found %lu Mach-O binary/binaries to sign in %s\n", (unsigned long)machos.count, appDir.UTF8String);
	for (NSString *macho in machos) {
		chmod(macho.fileSystemRepresentation, 0755);
		BOOL ok = sign_macho(macho, extraEntitlements);
		printf("appinst: ad-hoc signed %s: %s\n", macho.lastPathComponent.UTF8String, ok ? "OK" : "FAILED");
	}
}

static NSDictionary<NSString *, NSString *> *
setup_app_groups(NSArray<NSString *> *groups)
{
	NSFileManager *fm = NSFileManager.defaultManager;
	NSMutableDictionary<NSString *, NSString *> *containers = [NSMutableDictionary dictionary];
	NSString *baseShared = @"/private/var/mobile/Containers/Shared/AppGroup";
	[fm createDirectoryAtPath:baseShared withIntermediateDirectories:YES attributes:@{ NSFilePosixPermissions: @0775 } error:nil];

	for (NSString *groupId in groups) {
		NSString *containerPath = nil;
		MCMContainer *group = [NSClassFromString(@"MCMSharedDataContainer") containerWithIdentifier:groupId
		                                                                          createIfNecessary:YES
		                                                                                    existed:NULL
		                                                                                      error:NULL];
		if (group.url != nil && group.url.path != nil) {
			containerPath = group.url.path;
		} else {
			NSString *uuid = [[NSUUID UUID] UUIDString];
			containerPath = [baseShared stringByAppendingPathComponent:uuid];
			NSDictionary *meta = @{
				@"MCMMetadataIdentifier": groupId,
				@"MCMMetadataUUID": uuid,
				@"MCMMetadataContentClass": @7,
			};
			[fm createDirectoryAtPath:containerPath withIntermediateDirectories:YES attributes:@{ NSFilePosixPermissions: @0775 } error:nil];
			NSString *metaPath = [containerPath stringByAppendingPathComponent:@".com.apple.mobile_container_manager.metadata.plist"];
			[meta writeToFile:metaPath atomically:YES];
		}

		for (NSString *sub in @[@"Library", @"Library/Preferences", @"Library/Caches", @"Documents"]) {
			NSString *subPath = [containerPath stringByAppendingPathComponent:sub];
			[fm createDirectoryAtPath:subPath withIntermediateDirectories:YES attributes:@{ NSFilePosixPermissions: @0775 } error:nil];
		}
		containers[groupId] = containerPath;
		printf("appinst: configured AppGroup %s at %s\n", groupId.UTF8String, containerPath.UTF8String);
	}
	return containers;
}

/* the application extensions inside, as LaunchServices wants them listed with their bundle */
static NSDictionary *
plugins_of(NSString *app, NSDictionary<NSString *, NSString *> *groupContainers)
{
	NSMutableDictionary *plugins = [NSMutableDictionary dictionary];
	NSString *dir = [app stringByAppendingPathComponent:@"PlugIns"];

	for (NSString *name in [NSFileManager.defaultManager contentsOfDirectoryAtPath:dir error:nil]) {
		NSString *path = [dir stringByAppendingPathComponent:name];
		NSDictionary *info = [NSDictionary dictionaryWithContentsOfFile:[path stringByAppendingPathComponent:@"Info.plist"]];
		NSString *identifier = info[@"CFBundleIdentifier"];
		NSString *executable = info[@"CFBundleExecutable"];

		if (identifier == nil) {
			continue;
		}
		if (executable != nil) {
			chmod([path stringByAppendingPathComponent:executable].fileSystemRepresentation, 0755);
		}

		NSMutableDictionary *pRecord = [NSMutableDictionary dictionaryWithDictionary:@{
			@"ApplicationType": @"PluginKitPlugin",
			@"CFBundleIdentifier": identifier,
			@"CodeInfoIdentifier": identifier,
			@"Path": path,
			@"IsAdHocSigned": @YES,
			@"SignerOrganization": @"Apple Inc.",
			@"SignerIdentity": @"Apple iPhone OS Application Signing",
			@"PluginOwnerBundleID": [NSDictionary dictionaryWithContentsOfFile:
			    [app stringByAppendingPathComponent:@"Info.plist"]][@"CFBundleIdentifier"] ?: @""
		}];

		MCMContainer *pdata = [NSClassFromString(@"MCMPluginKitPluginDataContainer") containerWithIdentifier:identifier
		                                                                                    createIfNecessary:YES
		                                                                                              existed:NULL
		                                                                                                error:NULL];
		if (pdata.url.path != nil) {
			pRecord[@"Container"] = pdata.url.path;
		}
		if (groupContainers.count > 0) {
			pRecord[@"GroupContainers"] = groupContainers;
		}
		plugins[identifier] = pRecord;
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

	NSDictionary<NSString *, NSString *> *groupContainers = setup_app_groups(@[
		@"group.com.SideStore.SideStore",
		@"group.com.rileytestut.AltStore"
	]);
	if (access(execPath.fileSystemRepresentation, W_OK) == 0) {
		sign_all_machos(app, nil);
	}

	NSMutableDictionary *record = [NSMutableDictionary dictionaryWithDictionary:@{
		@"ApplicationType": @"System",
		@"CFBundleIdentifier": identifier,
		@"CodeInfoIdentifier": identifier,
		@"CompatibilityState": @0,
		@"Container": home,
		@"GroupContainers": groupContainers,
		@"IsContainerized": @YES,
		@"EnvironmentVariables": @{ @"CFFIXED_USER_HOME": home, @"HOME": home, @"TMPDIR": [home stringByAppendingPathComponent:@"tmp"] },
		@"IsDeletable": @YES,
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
		@"_LSBundlePlugins": plugins_of(app, groupContainers),
	}];
	if (info[@"CFBundleName"]) record[@"CFBundleName"] = info[@"CFBundleName"];
	if (info[@"CFBundleDisplayName"]) record[@"CFBundleDisplayName"] = info[@"CFBundleDisplayName"];
	if (info[@"CFBundlePackageType"]) record[@"CFBundlePackageType"] = info[@"CFBundlePackageType"];
	if (info[@"CFBundleShortVersionString"]) record[@"CFBundleShortVersionString"] = info[@"CFBundleShortVersionString"];
	if (info[@"CFBundleVersion"]) record[@"CFBundleVersion"] = info[@"CFBundleVersion"];
	if (info[@"CFBundleIcons"]) record[@"CFBundleIcons"] = info[@"CFBundleIcons"];
	if (info[@"CFBundleIcons~ipad"]) record[@"CFBundleIcons~ipad"] = info[@"CFBundleIcons~ipad"];
	if (info[@"CFBundleIconFiles"]) record[@"CFBundleIconFiles"] = info[@"CFBundleIconFiles"];
	if (info[@"CFBundleIconFile"]) record[@"CFBundleIconFile"] = info[@"CFBundleIconFile"];
	if (![[NSClassFromString(@"LSApplicationWorkspace") defaultWorkspace] registerApplicationDictionary:record]) {
		fprintf(stderr, "appinst: LaunchServices refused %s\n", identifier.UTF8String);
		return 1;
	}
	printf("appinst: %s installed at %s, data in %s\n", identifier.UTF8String, app.UTF8String, home.UTF8String);

	notify_post("com.apple.LaunchServices.ApplicationsChanged");
	notify_post("com.apple.LaunchServices.applicationRegistered");
	notify_post("com.redwood.var_save");
	printf("appinst: posted com.redwood.var_save for %s\n", identifier.UTF8String);
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

/*
 * Registers a pre-installed system application (such as RedwoodInstaller.app) into LaunchServices.
 * Creates an AppData container if necessary and sets ApplicationType = System.
 */
static int
register_system_app(NSString *appPath)
{
	NSFileManager *fm = NSFileManager.defaultManager;
	NSString *app = appPath;
	BOOL isDir = NO;

	if (![fm fileExistsAtPath:app isDirectory:&isDir] || !isDir) {
		NSString *fallback = [@"/Applications" stringByAppendingPathComponent:appPath.lastPathComponent];
		if ([fm fileExistsAtPath:fallback isDirectory:&isDir] && isDir) {
			app = fallback;
		} else {
			fprintf(stderr, "appinst: system app not found at %s (or %s)\n", appPath.UTF8String, fallback.UTF8String);
			return 1;
		}
	}

	NSDictionary *info = [NSDictionary dictionaryWithContentsOfFile:[app stringByAppendingPathComponent:@"Info.plist"]];
	if (info[@"CFBundleIdentifier"] == nil || info[@"CFBundleExecutable"] == nil) {
		fprintf(stderr, "appinst: %s missing CFBundleIdentifier or CFBundleExecutable\n", app.UTF8String);
		return 1;
	}
	NSString *identifier = info[@"CFBundleIdentifier"];
	NSString *execPath = [app stringByAppendingPathComponent:info[@"CFBundleExecutable"]];
	chmod(execPath.fileSystemRepresentation, 0755);

	dlopen("/System/Library/PrivateFrameworks/MobileContainerManager.framework/MobileContainerManager", RTLD_NOW);
	NSError *error = nil;
	MCMContainer *data = [NSClassFromString(@"MCMAppDataContainer") containerWithIdentifier:identifier createIfNecessary:YES
	                                                                               existed:NULL error:&error];
	NSString *home = data.url.path ?: @"/private/var/mobile";

	if (access(execPath.fileSystemRepresentation, W_OK) == 0) {
		sign_all_machos(app, nil);
	}

	/* Unregister existing stale LaunchServices proxy if present to force fresh registration */
	LSApplicationWorkspace *ws = [NSClassFromString(@"LSApplicationWorkspace") defaultWorkspace];
	LSApplicationProxy *old = [NSClassFromString(@"LSApplicationProxy") applicationProxyForIdentifier:identifier];
	if (old != nil && old.bundleURL != nil) {
		[ws unregisterApplication:old.bundleURL];
	}

	NSMutableDictionary *record = [NSMutableDictionary dictionaryWithDictionary:@{
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
		@"CFBundleName": info[@"CFBundleName"] ?: @"RedwoodInstaller",
		@"CFBundleDisplayName": info[@"CFBundleDisplayName"] ?: @"IPA 管理器",
		@"CFBundlePackageType": info[@"CFBundlePackageType"] ?: @"APPL",
		@"CFBundleShortVersionString": info[@"CFBundleShortVersionString"] ?: @"1.0",
		@"CFBundleVersion": info[@"CFBundleVersion"] ?: @"1",
		@"_LSBundlePlugins": plugins_of(app, nil),
	}];

	if (info[@"CFBundleIcons"]) {
		record[@"CFBundleIcons"] = info[@"CFBundleIcons"];
	} else {
		record[@"CFBundleIcons"] = @{
			@"CFBundlePrimaryIcon": @{
				@"CFBundleIconFiles": @[ @"AppIcon60x60", @"AppIcon29x29", @"AppIcon40x40" ],
				@"CFBundleIconName": @"AppIcon"
			}
		};
	}
	if (info[@"CFBundleIcons~ipad"]) {
		record[@"CFBundleIcons~ipad"] = info[@"CFBundleIcons~ipad"];
	} else {
		record[@"CFBundleIcons~ipad"] = @{
			@"CFBundlePrimaryIcon": @{
				@"CFBundleIconFiles": @[ @"AppIcon60x60", @"AppIcon29x29", @"AppIcon40x40", @"AppIcon76x76", @"AppIcon83.5x83.5" ],
				@"CFBundleIconName": @"AppIcon"
			}
		};
	}
	if (info[@"CFBundleIconFiles"]) {
		record[@"CFBundleIconFiles"] = info[@"CFBundleIconFiles"];
	} else {
		record[@"CFBundleIconFiles"] = @[ @"AppIcon60x60", @"AppIcon29x29", @"AppIcon40x40" ];
	}
	if (info[@"CFBundleIconFile"]) {
		record[@"CFBundleIconFile"] = info[@"CFBundleIconFile"];
	} else {
		record[@"CFBundleIconFile"] = @"AppIcon60x60";
	}

	if (![ws registerApplicationDictionary:record]) {
		fprintf(stderr, "appinst: LaunchServices refused system app %s\n", identifier.UTF8String);
		return 1;
	}
	printf("appinst: %s registered as system app at %s (data in %s)\n", identifier.UTF8String, app.UTF8String, home.UTF8String);
	notify_post("com.apple.LaunchServices.ApplicationsChanged");
	notify_post("com.apple.LaunchServices.applicationRegistered");
	notify_post("com.redwood.var_save");
	return 0;
}

/*
 * Watch INBOX (/private/var/db/appinst-inbox) and legacy USB transfer directory (/private/var/tmp/ipa).
 * Auto-polling of Safari Downloads and AppGroup Downloads has been removed per §8.1 item 8
 * to prevent repeated app decompression, re-signing, and installation on boot.
 */
static NSArray<NSString *> *
download_dirs(void)
{
	NSMutableArray<NSString *> *dirs = [NSMutableArray array];
	[dirs addObject:INBOX];
	if (![INBOX isEqualToString:LEGACY_INBOX]) {
		[dirs addObject:LEGACY_INBOX];
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
 * Every second, each .ipa in the INBOX that is a whole zip archive is copied out,
 * installed, and removed upon success.
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
	[fm createDirectoryAtPath:LEGACY_INBOX withIntermediateDirectories:YES attributes:@{ NSFilePosixPermissions: @0777 } error:nil];

	/* Ensure RedwoodInstaller.app is registered in LaunchServices so it appears on home screen even with existing darwin_var */
	register_system_app(@"/redwood/Applications/RedwoodInstaller.app");

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
					int install_status = install(copy);
					printf("appinst: %s\n", install_status == 0 ? "installed" : "NOT INSTALLED");
					[fm removeItemAtPath:copy error:nil];
					if (install_status == 0) {
						/* Remove source file after successful installation to save ramfs space and avoid re-install */
						[fm removeItemAtPath:path error:nil];
					}
				}
			}
		}
		sleep(1);
	}
}

static int
remove_app(NSString *identifier)
{
	NSFileManager *fm = NSFileManager.defaultManager;
	LSApplicationWorkspace *ws = [NSClassFromString(@"LSApplicationWorkspace") defaultWorkspace];
	LSApplicationProxy *proxy = [NSClassFromString(@"LSApplicationProxy") applicationProxyForIdentifier:identifier];
	BOOL found = NO;

	/* 1. Explicit uninstallation via LaunchServices API */
	if ([ws respondsToSelector:sel_registerName("uninstallApplication:withOptions:")]) {
		if ([ws uninstallApplication:identifier withOptions:nil]) {
			found = YES;
		}
	}

	if (proxy != nil && proxy.bundleURL != nil) {
		[ws unregisterApplication:proxy.bundleURL];
		if ([proxy.bundleURL.path hasPrefix:BUNDLES]) {
			NSString *bundleContainer = proxy.bundleURL.path.stringByDeletingLastPathComponent;
			[fm removeItemAtPath:bundleContainer error:nil];
			found = YES;
		}
	}

	/* 2. Remove bundle container directory */
	for (NSString *uuid in [fm contentsOfDirectoryAtPath:BUNDLES error:nil]) {
		NSString *dir = [BUNDLES stringByAppendingPathComponent:uuid];
		NSString *app = app_in(dir);
		if (app != nil) {
			NSDictionary *info = [NSDictionary dictionaryWithContentsOfFile:[app stringByAppendingPathComponent:@"Info.plist"]];
			if ([info[@"CFBundleIdentifier"] isEqualToString:identifier]) {
				[ws unregisterApplication:[NSURL fileURLWithPath:app]];
				[fm removeItemAtPath:dir error:nil];
				found = YES;
			}
		}
	}

	/* 3. Remove data container */
	dlopen("/System/Library/PrivateFrameworks/MobileContainerManager.framework/MobileContainerManager", RTLD_NOW);
	Class mcmClass = NSClassFromString(@"MCMAppDataContainer");
	if (mcmClass != Nil) {
		MCMContainer *data = [mcmClass containerWithIdentifier:identifier createIfNecessary:NO existed:NULL error:NULL];
		if (data != nil && data.url != nil) {
			[fm removeItemAtPath:data.url.path error:nil];
		}
	}

	NSString *dataRoot = @"/private/var/mobile/Containers/Data/Application";
	for (NSString *duuid in [fm contentsOfDirectoryAtPath:dataRoot error:nil]) {
		NSString *dpath = [dataRoot stringByAppendingPathComponent:duuid];
		NSString *meta = [dpath stringByAppendingPathComponent:@".com.apple.mobile_container_manager.metadata.plist"];
		NSDictionary *mdict = [NSDictionary dictionaryWithContentsOfFile:meta];
		if ([mdict[@"MCMMetadataIdentifier"] isEqualToString:identifier]) {
			[fm removeItemAtPath:dpath error:nil];
		}
	}

	notify_post("com.apple.LaunchServices.ApplicationsChanged");
	notify_post("com.redwood.var_save");
	if (found) {
		printf("appinst: %s removed (posted com.redwood.var_save)\n", identifier.UTF8String);
		return 0;
	} else {
		fprintf(stderr, "appinst: %s was not found in bundle containers (cleaned any data containers)\n", identifier.UTF8String);
		return 1;
	}
}

static BOOL
is_test_app_identifier(NSString *identifier)
{
	if ([identifier isEqualToString:@"com.redwood.installer"]) {
		return NO;
	}
	if ([identifier isEqualToString:@"com.redwood.hello"] ||
	    [identifier hasPrefix:@"com.example."] ||
	    [identifier hasPrefix:@"com.test."] ||
	    [identifier hasPrefix:@"com.nssurge."] ||
	    [identifier hasPrefix:@"com.sina.weibo"] ||
	    [identifier isEqualToString:@"cilicili"] ||
	    [identifier isEqualToString:@"Kumone"] ||
	    [identifier isEqualToString:@"weibo"] ||
	    [identifier rangeOfString:@"weibo" options:NSCaseInsensitiveSearch].location != NSNotFound ||
	    [identifier rangeOfString:@"surge" options:NSCaseInsensitiveSearch].location != NSNotFound ||
	    [identifier rangeOfString:@"cilicili" options:NSCaseInsensitiveSearch].location != NSNotFound ||
	    [identifier rangeOfString:@"kumone" options:NSCaseInsensitiveSearch].location != NSNotFound ||
	    [identifier rangeOfString:@"test" options:NSCaseInsensitiveSearch].location != NSNotFound ||
	    [identifier rangeOfString:@"demo" options:NSCaseInsensitiveSearch].location != NSNotFound ||
	    [identifier rangeOfString:@"sample" options:NSCaseInsensitiveSearch].location != NSNotFound) {
		return YES;
	}
	return NO;
}

static int
clean_test_apps(void)
{
	int cleaned_count = 0;
	NSFileManager *fm = NSFileManager.defaultManager;
	LSApplicationWorkspace *ws = [NSClassFromString(@"LSApplicationWorkspace") defaultWorkspace];

	/* 1. Explicitly uninstall via LaunchServices workspace for any registered test applications */
	if ([ws respondsToSelector:sel_registerName("allApplications")]) {
		NSArray *all = [ws allApplications];
		for (id proxy in all) {
			NSString *bid = [proxy respondsToSelector:sel_registerName("applicationIdentifier")] ?
				[proxy applicationIdentifier] : nil;
			if (bid != nil && is_test_app_identifier(bid)) {
				printf("appinst: uninstalling registered test app %s via LaunchServices...\n", bid.UTF8String);
				remove_app(bid);
				cleaned_count++;
			}
		}
	}
	if ([ws respondsToSelector:sel_registerName("allInstalledApplications")]) {
		NSArray *all = [ws allInstalledApplications];
		for (id proxy in all) {
			NSString *bid = [proxy respondsToSelector:sel_registerName("applicationIdentifier")] ?
				[proxy applicationIdentifier] : nil;
			if (bid != nil && is_test_app_identifier(bid)) {
				printf("appinst: uninstalling installed test app %s via LaunchServices...\n", bid.UTF8String);
				remove_app(bid);
				cleaned_count++;
			}
		}
	}

	/* 2. Scan and remove any bundle directories under /private/var/containers/Bundle/Application */
	NSArray<NSString *> *uuids = [fm contentsOfDirectoryAtPath:BUNDLES error:nil];
	for (NSString *uuid in uuids) {
		NSString *bundle_dir = [BUNDLES stringByAppendingPathComponent:uuid];
		NSString *app = app_in(bundle_dir);
		if (app == nil) {
			[fm removeItemAtPath:bundle_dir error:nil];
			continue;
		}
		NSDictionary *info = [NSDictionary dictionaryWithContentsOfFile:[app stringByAppendingPathComponent:@"Info.plist"]];
		NSString *bid = info[@"CFBundleIdentifier"];
		if (bid != nil && is_test_app_identifier(bid)) {
			printf("appinst: cleaning leftover test app bundle %s (%s)...\n", bid.UTF8String, app.UTF8String);
			remove_app(bid);
			cleaned_count++;
		}
	}

	/* 3. Clean LaunchServices & Icons caches */
	NSString *cachesDir = @"/private/var/mobile/Library/Caches";
	NSArray<NSString *> *cacheItems = [fm contentsOfDirectoryAtPath:cachesDir error:nil];
	for (NSString *item in cacheItems) {
		if ([item hasPrefix:@"com.apple.LaunchServices"] ||
		    [item hasPrefix:@"com.apple.IconsCache"]) {
			NSString *p = [cachesDir stringByAppendingPathComponent:item];
			printf("appinst: removing cache %s\n", p.UTF8String);
			[fm removeItemAtPath:p error:nil];
		}
	}

	/* 4. Ensure RedwoodInstaller is registered cleanly with LaunchServices */
	register_system_app(@"/redwood/Applications/RedwoodInstaller.app");

	notify_post("com.apple.LaunchServices.ApplicationsChanged");
	notify_post("com.redwood.var_save");

	printf("appinst: cleaned %d test app(s) and cleared LaunchServices/Icons caches\n", cleaned_count);
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
		if ((argc == 2 || argc == 3) && strcmp(argv[1], "install") == 0) {
			const char *target = (argc == 3) ? argv[2] : (getenv("IPA_PATH") ?: getenv("K90_IPA"));
			if (target != NULL) {
				return install(@(target));
			}
			fprintf(stderr, "appinst install requires an IPA or app path argument or IPA_PATH/K90_IPA env var\n");
			return 2;
		}
		if (argc == 3 && strcmp(argv[1], "remove") == 0) {
			return remove_app(@(argv[2]));
		}
		if (argc == 2 && (strcmp(argv[1], "clean") == 0 || strcmp(argv[1], "cleanup") == 0)) {
			return clean_test_apps();
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
		if ((argc == 2 || argc == 3) && (strcmp(argv[1], "register-system") == 0 || strcmp(argv[1], "register") == 0)) {
			NSString *target = (argc == 3) ? @(argv[2]) : @"/redwood/Applications/RedwoodInstaller.app";
			return register_system_app(target);
		}
		fprintf(stderr, "usage: appinst install FILE.ipa|DIR.app | remove BUNDLE-ID | list | open BUNDLE-ID | url URL | clean | watch | register-system [APP_PATH]\n");
		return 2;
	}
}
