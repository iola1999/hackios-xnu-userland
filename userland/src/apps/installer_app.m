/*
 * RedwoodInstaller: Native iOS application manager and IPA installer for HackiOS.
 *
 * Provides a graphical interface to:
 * 1. View all installed third-party applications with bundle ID, version, and disk size.
 * 2. Launch installed applications or uninstall them with complete bundle and data container cleanup.
 * 3. Scan download directories (/private/var/tmp/ipa, Safari downloads, AppGroup downloads) for .ipa packages.
 * 4. Install selected .ipa packages or handle .ipa files shared via URL.
 * 5. Clean up leftover test applications with one click.
 */
#import <UIKit/UIKit.h>
#import <Foundation/Foundation.h>
#include <dlfcn.h>
#include <notify.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

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
+ (instancetype)containerWithIdentifier:(NSString *)identifier createIfNecessary:(BOOL)create existed:(BOOL *)existed error:(NSError **)error;
- (NSURL *)url;
@end

@interface AppRecord : NSObject
@property (nonatomic, copy) NSString *name;
@property (nonatomic, copy) NSString *bundleID;
@property (nonatomic, copy) NSString *version;
@property (nonatomic, copy) NSString *bundlePath;
@property (nonatomic, copy) NSString *containerPath;
@property (nonatomic, strong) UIImage *icon;
@property (nonatomic, assign) unsigned long long sizeBytes;
@end

@implementation AppRecord
@end

@interface IPARecord : NSObject
@property (nonatomic, copy) NSString *filename;
@property (nonatomic, copy) NSString *filePath;
@property (nonatomic, copy) NSString *modDate;
@property (nonatomic, assign) unsigned long long sizeBytes;
@end

@implementation IPARecord
@end

static NSString *const BUNDLES_DIR = @"/private/var/containers/Bundle/Application";
static NSString *const INBOX_DIR = @"/private/var/tmp/ipa";

static unsigned long long calculate_directory_size(NSString *path)
{
	unsigned long long total = 0;
	NSFileManager *fm = NSFileManager.defaultManager;
	NSDirectoryEnumerator *enumerator = [fm enumeratorAtPath:path];
	for (NSString *sub in enumerator) {
		NSString *full = [path stringByAppendingPathComponent:sub];
		NSDictionary *attrs = [fm attributesOfItemAtPath:full error:nil];
		if (attrs && [attrs[NSFileType] isEqualToString:NSFileTypeRegular]) {
			total += [attrs[NSFileSize] unsignedLongLongValue];
		}
	}
	return total;
}

static NSString *format_size_string(unsigned long long bytes)
{
	if (bytes >= 1024ULL * 1024ULL * 1024ULL) {
		return [NSString stringWithFormat:@"%.2f GB", (double)bytes / (1024.0 * 1024.0 * 1024.0)];
	} else if (bytes >= 1024ULL * 1024ULL) {
		return [NSString stringWithFormat:@"%.1f MB", (double)bytes / (1024.0 * 1024.0)];
	} else if (bytes >= 1024ULL) {
		return [NSString stringWithFormat:@"%.1f KB", (double)bytes / 1024.0];
	} else {
		return [NSString stringWithFormat:@"%llu B", bytes];
	}
}

static BOOL is_test_identifier(NSString *bid)
{
	if ([bid isEqualToString:@"com.redwood.installer"]) {
		return NO;
	}
	if ([bid isEqualToString:@"com.redwood.hello"] ||
	    [bid hasPrefix:@"com.example."] ||
	    [bid hasPrefix:@"com.test."] ||
	    [bid rangeOfString:@"test" options:NSCaseInsensitiveSearch].location != NSNotFound ||
	    [bid rangeOfString:@"demo" options:NSCaseInsensitiveSearch].location != NSNotFound ||
	    [bid rangeOfString:@"sample" options:NSCaseInsensitiveSearch].location != NSNotFound) {
		return YES;
	}
	return NO;
}

/* libsystem's private persona spawn calls (spawn_private.h) */
#define POSIX_SPAWN_PERSONA_FLAGS_OVERRIDE 1
int posix_spawnattr_set_persona_np(const posix_spawnattr_t *, uid_t, uint32_t);
int posix_spawnattr_set_persona_uid_np(const posix_spawnattr_t *, uid_t);
int posix_spawnattr_set_persona_gid_np(const posix_spawnattr_t *, gid_t);

/*
 * appinst writes under /private/var/containers/Bundle/Application, which only root may write, and
 * this application runs as mobile. It is started in the system persona (99) with uid and gid 0, the
 * way package managers start their root helpers; the kernel's spawn_validate_persona accepts that
 * without the persona-mgmt entitlement.
 */
static int run_external_appinst(const char *action, const char *arg, NSString **output)
{
	const char *candidates[] = {
		"/redwood/bin/appinst",
		"/bin/appinst",
		"/usr/bin/appinst",
		NULL
	};
	const char *bin_path = NULL;
	for (int i = 0; candidates[i] != NULL; i++) {
		if (access(candidates[i], X_OK) == 0) {
			bin_path = candidates[i];
			break;
		}
	}
	if (bin_path == NULL) {
		bin_path = "/redwood/bin/appinst";
	}

	int pipe_fd[2];
	if (pipe(pipe_fd) != 0) {
		return -1;
	}

	posix_spawn_file_actions_t actions;
	posix_spawn_file_actions_init(&actions);
	posix_spawn_file_actions_adddup2(&actions, pipe_fd[1], STDOUT_FILENO);
	posix_spawn_file_actions_adddup2(&actions, pipe_fd[1], STDERR_FILENO);
	posix_spawn_file_actions_addclose(&actions, pipe_fd[0]);

	char *const argv[] = {
		(char *)bin_path,
		(char *)action,
		(char *)arg,
		NULL
	};

	posix_spawnattr_t attr;
	posix_spawnattr_init(&attr);
	posix_spawnattr_set_persona_np(&attr, 99, POSIX_SPAWN_PERSONA_FLAGS_OVERRIDE);
	posix_spawnattr_set_persona_uid_np(&attr, 0);
	posix_spawnattr_set_persona_gid_np(&attr, 0);

	extern char **environ;
	pid_t pid;
	int status = posix_spawn(&pid, bin_path, &actions, &attr, argv, environ);
	posix_spawnattr_destroy(&attr);
	posix_spawn_file_actions_destroy(&actions);
	close(pipe_fd[1]);

	if (status != 0) {
		close(pipe_fd[0]);
		return status;
	}

	NSMutableData *data = [NSMutableData data];
	char buf[512];
	ssize_t n;
	while ((n = read(pipe_fd[0], buf, sizeof(buf))) > 0) {
		[data appendBytes:buf length:n];
	}
	close(pipe_fd[0]);

	int wait_stat = 0;
	waitpid(pid, &wait_stat, 0);

	if (output != NULL) {
		*output = [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
	}
	return WEXITSTATUS(wait_stat);
}

/* The application's primary icon: the asset catalog entry first, then the loose PNGs Info.plist names. */
static UIImage *bundle_icon(NSString *appPath, NSDictionary *info)
{
	NSBundle *bundle = [NSBundle bundleWithPath:appPath];
	NSDictionary *primary = info[@"CFBundleIcons"][@"CFBundlePrimaryIcon"];
	NSMutableArray<NSString *> *names = [NSMutableArray array];
	if ([primary[@"CFBundleIconName"] isKindOfClass:NSString.class]) {
		[names addObject:primary[@"CFBundleIconName"]];
	}
	for (NSString *name in [[primary[@"CFBundleIconFiles"] reverseObjectEnumerator] allObjects]) {
		if ([name isKindOfClass:NSString.class]) {
			[names addObject:name];
		}
	}
	if ([info[@"CFBundleIconFile"] isKindOfClass:NSString.class]) {
		[names addObject:info[@"CFBundleIconFile"]];
	}
	for (NSString *name in names) {
		UIImage *image = [UIImage imageNamed:name inBundle:bundle compatibleWithTraitCollection:nil];
		if (image != nil) {
			return image;
		}
		for (NSString *suffix in @[ @"@3x.png", @"@2x.png", @".png" ]) {
			image = [UIImage imageWithContentsOfFile:[appPath stringByAppendingPathComponent:[name stringByAppendingString:suffix]]];
			if (image != nil) {
				return image;
			}
		}
	}
	return nil;
}

static UIButton *capsule_button(NSString *title, SEL action, id target, NSInteger tag)
{
	UIButtonConfiguration *config = [UIButtonConfiguration grayButtonConfiguration];
	config.title = title;
	config.cornerStyle = UIButtonConfigurationCornerStyleCapsule;
	config.contentInsets = NSDirectionalEdgeInsetsMake(5, 14, 5, 14);
	config.titleTextAttributesTransformer = ^NSDictionary *(NSDictionary *attributes) {
		NSMutableDictionary *result = [attributes mutableCopy];
		result[NSFontAttributeName] = [UIFont systemFontOfSize:15 weight:UIFontWeightSemibold];
		return result;
	};
	UIButton *button = [UIButton buttonWithConfiguration:config primaryAction:nil];
	button.tag = tag;
	[button addTarget:target action:action forControlEvents:UIControlEventTouchUpInside];
	[button sizeToFit];
	return button;
}

@interface InstallerViewController : UIViewController <UITableViewDataSource, UITableViewDelegate>
@property (nonatomic, strong) UISegmentedControl *segmentedControl;
@property (nonatomic, strong) UITableView *tableView;
@property (nonatomic, strong) UIRefreshControl *refreshControl;
@property (nonatomic, strong) NSMutableArray<AppRecord *> *installedApps;
@property (nonatomic, strong) NSMutableArray<IPARecord *> *availableIPAs;
- (void)handleIncomingFile:(NSURL *)fileURL;
@end

@implementation InstallerViewController

- (void)viewDidLoad
{
	[super viewDidLoad];
	self.title = @"IPA 管理器";
	self.navigationController.navigationBar.prefersLargeTitles = YES;
	self.navigationItem.largeTitleDisplayMode = UINavigationItemLargeTitleDisplayModeAlways;

	self.installedApps = [NSMutableArray array];
	self.availableIPAs = [NSMutableArray array];

	UIAction *refresh = [UIAction actionWithTitle:@"刷新" image:[UIImage systemImageNamed:@"arrow.clockwise"]
	                                   identifier:nil handler:^(__kindof UIAction *action) { [self reloadCurrentData]; }];
	UIAction *clean = [UIAction actionWithTitle:@"清理测试应用" image:[UIImage systemImageNamed:@"trash"]
	                                 identifier:nil handler:^(__kindof UIAction *action) { [self cleanTestAppsClicked]; }];
	clean.attributes = UIMenuElementAttributesDestructive;
	self.navigationItem.rightBarButtonItem = [[UIBarButtonItem alloc] initWithImage:[UIImage systemImageNamed:@"ellipsis.circle"]
	                                                                           menu:[UIMenu menuWithChildren:@[ refresh, clean ]]];

	self.tableView = [[UITableView alloc] initWithFrame:self.view.bounds style:UITableViewStyleInsetGrouped];
	self.tableView.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
	self.tableView.dataSource = self;
	self.tableView.delegate = self;
	self.tableView.rowHeight = 68;
	[self.view addSubview:self.tableView];

	self.segmentedControl = [[UISegmentedControl alloc] initWithItems:@[ @"已安装", @"安装包" ]];
	self.segmentedControl.selectedSegmentIndex = 0;
	[self.segmentedControl addTarget:self action:@selector(segmentChanged:) forControlEvents:UIControlEventValueChanged];
	UIView *header = [[UIView alloc] initWithFrame:CGRectMake(0, 0, self.view.bounds.size.width, 48)];
	self.segmentedControl.frame = CGRectInset(header.bounds, 20, 8);
	self.segmentedControl.autoresizingMask = UIViewAutoresizingFlexibleWidth;
	[header addSubview:self.segmentedControl];
	self.tableView.tableHeaderView = header;

	self.refreshControl = [[UIRefreshControl alloc] init];
	[self.refreshControl addTarget:self action:@selector(reloadCurrentData) forControlEvents:UIControlEventValueChanged];
	self.tableView.refreshControl = self.refreshControl;

	[self reloadCurrentData];
}

- (void)viewWillAppear:(BOOL)animated
{
	[super viewWillAppear:animated];
	[self reloadCurrentData];
}

- (void)segmentChanged:(UISegmentedControl *)sender
{
	[self reloadCurrentData];
}

- (void)reloadCurrentData
{
	if (self.segmentedControl.selectedSegmentIndex == 0) {
		[self scanInstalledApplications];
	} else {
		[self scanAvailableIPAPackages];
	}
	[self.refreshControl endRefreshing];
	[self.tableView reloadData];
	[self updateEmptyState];
}

- (void)updateEmptyState
{
	BOOL apps = self.segmentedControl.selectedSegmentIndex == 0;
	if ((apps ? self.installedApps.count : self.availableIPAs.count) > 0) {
		self.tableView.backgroundView = nil;
		return;
	}
	UIContentUnavailableConfiguration *empty = [UIContentUnavailableConfiguration emptyConfiguration];
	empty.image = [UIImage systemImageNamed:apps ? @"square.dashed" : @"arrow.down.doc"];
	empty.text = apps ? @"没有已安装的应用" : @"没有安装包";
	empty.secondaryText = apps ? @"在「安装包」里选择 .ipa 安装" : @"用 Safari 下载的 .ipa 会出现在这里";
	self.tableView.backgroundView = [[UIContentUnavailableView alloc] initWithConfiguration:empty];
}

- (void)scanInstalledApplications
{
	[self.installedApps removeAllObjects];
	NSFileManager *fm = NSFileManager.defaultManager;
	NSArray<NSString *> *uuids = [fm contentsOfDirectoryAtPath:BUNDLES_DIR error:nil];

	for (NSString *uuid in uuids) {
		NSString *bundleRoot = [BUNDLES_DIR stringByAppendingPathComponent:uuid];
		for (NSString *sub in [fm contentsOfDirectoryAtPath:bundleRoot error:nil]) {
			if ([sub.pathExtension.lowercaseString isEqualToString:@"app"]) {
				NSString *appPath = [bundleRoot stringByAppendingPathComponent:sub];
				NSDictionary *info = [NSDictionary dictionaryWithContentsOfFile:[appPath stringByAppendingPathComponent:@"Info.plist"]];
				if (info != nil) {
					AppRecord *record = [[AppRecord alloc] init];
					NSString *name = info[@"CFBundleDisplayName"] ?: info[@"CFBundleName"] ?: sub.stringByDeletingPathExtension;
					record.name = name;
					record.bundleID = info[@"CFBundleIdentifier"] ?: @"unknown";
					record.version = info[@"CFBundleShortVersionString"] ?: info[@"CFBundleVersion"] ?: @"1.0";
					record.bundlePath = appPath;
					record.sizeBytes = calculate_directory_size(bundleRoot);
					record.icon = bundle_icon(appPath, info);
					[self.installedApps addObject:record];
				}
			}
		}
	}
}

- (void)scanAvailableIPAPackages
{
	[self.availableIPAs removeAllObjects];
	NSFileManager *fm = NSFileManager.defaultManager;
	NSMutableArray<NSString *> *dirs = [NSMutableArray array];

	[dirs addObject:INBOX_DIR];
	[dirs addObject:@"/private/var/mobile/Downloads"];
	[dirs addObject:@"/private/var/tmp"];

	NSString *groupParent = @"/private/var/mobile/Containers/Shared/AppGroup";
	for (NSString *sub in [fm contentsOfDirectoryAtPath:groupParent error:nil]) {
		NSString *down = [groupParent stringByAppendingPathComponent:[NSString stringWithFormat:@"%@/File Provider Storage/Downloads", sub]];
		BOOL isDir = NO;
		if ([fm fileExistsAtPath:down isDirectory:&isDir] && isDir) {
			[dirs addObject:down];
		}
	}

	dlopen("/System/Library/PrivateFrameworks/MobileContainerManager.framework/MobileContainerManager", RTLD_NOW);
	Class mcmClass = NSClassFromString(@"MCMAppDataContainer");
	if (mcmClass != Nil) {
		MCMContainer *safari = [mcmClass containerWithIdentifier:@"com.apple.mobilesafari" createIfNecessary:NO existed:NULL error:NULL];
		if (safari.url != nil) {
			NSString *safariDown = [safari.url.path stringByAppendingPathComponent:@"Library/Safari/Downloads"];
			for (NSString *item in [fm contentsOfDirectoryAtPath:safariDown error:nil]) {
				NSString *itemPath = [safariDown stringByAppendingPathComponent:item];
				BOOL isDir = NO;
				if ([fm fileExistsAtPath:itemPath isDirectory:&isDir] && isDir) {
					[dirs addObject:itemPath];
				}
			}
		}
	}

	NSDateFormatter *dateFormatter = [[NSDateFormatter alloc] init];
	dateFormatter.dateFormat = @"yyyy-MM-dd HH:mm";

	for (NSString *dir in dirs) {
		for (NSString *file in [fm contentsOfDirectoryAtPath:dir error:nil]) {
			if ([file.pathExtension.lowercaseString isEqualToString:@"ipa"]) {
				NSString *full = [dir stringByAppendingPathComponent:file];
				NSDictionary *attrs = [fm attributesOfItemAtPath:full error:nil];
				if (attrs != nil) {
					IPARecord *record = [[IPARecord alloc] init];
					record.filename = file;
					record.filePath = full;
					record.sizeBytes = [attrs[NSFileSize] unsignedLongLongValue];
					record.modDate = [dateFormatter stringFromDate:attrs[NSFileModificationDate] ?: [NSDate date]];
					[self.availableIPAs addObject:record];
				}
			}
		}
	}
}

- (void)cleanTestAppsClicked
{
	UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"清理残留测试应用"
	                                                               message:@"将扫描并清理开发测试过程中遗留的应用（如 com.example.*、com.test.*、com.redwood.hello 等）。是否继续？"
	                                                        preferredStyle:UIAlertControllerStyleAlert];

	[alert addAction:[UIAlertAction actionWithTitle:@"取消" style:UIAlertActionStyleCancel handler:nil]];
	[alert addAction:[UIAlertAction actionWithTitle:@"开始清理" style:UIAlertActionStyleDestructive handler:^(UIAlertAction *action) {
		int count = 0;
		NSString *cliOut = nil;
		int rc = run_external_appinst("clean", NULL, &cliOut);
		if (rc != 0) {
			for (AppRecord *rec in [self.installedApps copy]) {
				if (is_test_identifier(rec.bundleID)) {
					run_external_appinst("remove", rec.bundleID.UTF8String, NULL);
					count++;
				}
			}
		} else {
			[self scanInstalledApplications];
			count = (int)self.installedApps.count;
		}

		[self reloadCurrentData];
		NSString *msg = [NSString stringWithFormat:@"清理完成，当前剩余 %lu 个应用。", (unsigned long)self.installedApps.count];
		UIAlertController *doneAlert = [UIAlertController alertControllerWithTitle:@"清理结果"
		                                                                   message:msg
		                                                            preferredStyle:UIAlertControllerStyleAlert];
		[doneAlert addAction:[UIAlertAction actionWithTitle:@"确定" style:UIAlertActionStyleDefault handler:nil]];
		[self presentViewController:doneAlert animated:YES completion:nil];
	}]];

	[self presentViewController:alert animated:YES completion:nil];
}

- (void)handleIncomingFile:(NSURL *)fileURL
{
	if (!fileURL || !fileURL.isFileURL) {
		return;
	}
	NSFileManager *fm = NSFileManager.defaultManager;
	[fm createDirectoryAtPath:INBOX_DIR withIntermediateDirectories:YES attributes:nil error:nil];
	NSString *dest = [INBOX_DIR stringByAppendingPathComponent:fileURL.lastPathComponent];
	[fm removeItemAtPath:dest error:nil];
	[fm copyItemAtURL:fileURL toURL:[NSURL fileURLWithPath:dest] error:nil];

	self.segmentedControl.selectedSegmentIndex = 1;
	[self reloadCurrentData];

	UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"检测到安装包"
	                                                               message:[NSString stringWithFormat:@"已接收 %@，是否立即安装？", fileURL.lastPathComponent]
	                                                        preferredStyle:UIAlertControllerStyleAlert];
	[alert addAction:[UIAlertAction actionWithTitle:@"稍后" style:UIAlertActionStyleCancel handler:nil]];
	[alert addAction:[UIAlertAction actionWithTitle:@"立即安装" style:UIAlertActionStyleDefault handler:^(UIAlertAction *action) {
		[self executeInstallIPA:dest];
	}]];
	[self presentViewController:alert animated:YES completion:nil];
}

- (void)executeInstallIPA:(NSString *)ipaPath
{
	UIAlertController *progress = [UIAlertController alertControllerWithTitle:@"正在安装"
	                                                                  message:ipaPath.lastPathComponent
	                                                           preferredStyle:UIAlertControllerStyleAlert];
	[self presentViewController:progress animated:YES completion:nil];

	dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
		NSString *output = nil;
		int res = run_external_appinst("install", ipaPath.fileSystemRepresentation, &output);

		dispatch_async(dispatch_get_main_queue(), ^{
			[progress dismissViewControllerAnimated:YES completion:^{
				if (res == 0) {
					self.segmentedControl.selectedSegmentIndex = 0;
					[self reloadCurrentData];
					return;
				}
				UIAlertController *failed = [UIAlertController alertControllerWithTitle:@"安装失败"
				                                                                message:output.length ? output : [NSString stringWithFormat:@"appinst 退出码 %d", res]
				                                                         preferredStyle:UIAlertControllerStyleAlert];
				[failed addAction:[UIAlertAction actionWithTitle:@"好" style:UIAlertActionStyleDefault handler:nil]];
				[self presentViewController:failed animated:YES completion:nil];
			}];
		});
	});
}

#pragma mark - UITableViewDataSource & UITableViewDelegate

- (NSInteger)numberOfSectionsInTableView:(UITableView *)tableView
{
	return 1;
}

- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section
{
	return self.segmentedControl.selectedSegmentIndex == 0 ? self.installedApps.count : self.availableIPAs.count;
}

- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath
{
	UITableViewCell *cell = [tableView dequeueReusableCellWithIdentifier:@"Row"];
	if (cell == nil) {
		cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault reuseIdentifier:@"Row"];
		cell.selectionStyle = UITableViewCellSelectionStyleNone;
	}
	UIListContentConfiguration *content = [UIListContentConfiguration subtitleCellConfiguration];
	content.textProperties.font = [UIFont systemFontOfSize:17 weight:UIFontWeightSemibold];
	content.textProperties.numberOfLines = 1;
	content.secondaryTextProperties.color = UIColor.secondaryLabelColor;
	content.imageProperties.maximumSize = CGSizeMake(44, 44);
	content.imageProperties.reservedLayoutSize = CGSizeMake(44, 44);
	content.imageProperties.cornerRadius = 10;

	if (self.segmentedControl.selectedSegmentIndex == 0) {
		AppRecord *record = self.installedApps[indexPath.row];
		content.text = record.name;
		content.secondaryText = [NSString stringWithFormat:@"版本 %@ · %@", record.version, format_size_string(record.sizeBytes)];
		content.image = record.icon ?: [UIImage systemImageNamed:@"app.dashed"];
		content.imageProperties.tintColor = UIColor.tertiaryLabelColor;
		cell.accessoryView = capsule_button(@"打开", @selector(openAppClicked:), self, indexPath.row);
	} else {
		IPARecord *record = self.availableIPAs[indexPath.row];
		content.text = record.filename.stringByDeletingPathExtension;
		content.secondaryText = [NSString stringWithFormat:@"%@ · %@", format_size_string(record.sizeBytes), record.modDate];
		content.image = [UIImage systemImageNamed:@"shippingbox.fill"];
		content.imageProperties.tintColor = UIColor.systemBlueColor;
		cell.accessoryView = capsule_button(@"安装", @selector(installIPAClicked:), self, indexPath.row);
	}
	cell.contentConfiguration = content;
	return cell;
}

- (UISwipeActionsConfiguration *)tableView:(UITableView *)tableView
    trailingSwipeActionsConfigurationForRowAtIndexPath:(NSIndexPath *)indexPath
{
	BOOL apps = self.segmentedControl.selectedSegmentIndex == 0;
	UIContextualAction *remove = [UIContextualAction contextualActionWithStyle:UIContextualActionStyleDestructive
	                                                                     title:apps ? @"卸载" : @"删除"
	                                                                   handler:^(UIContextualAction *action, UIView *view, void (^done)(BOOL)) {
		if (apps) {
			[self confirmUninstallAtIndex:indexPath.row];
		} else {
			[self confirmDeleteIPAAtIndex:indexPath.row];
		}
		done(YES);
	}];
	return [UISwipeActionsConfiguration configurationWithActions:@[ remove ]];
}

- (NSString *)tableView:(UITableView *)tableView titleForFooterInSection:(NSInteger)section
{
	if (self.segmentedControl.selectedSegmentIndex == 0 || self.availableIPAs.count == 0) {
		return nil;
	}
	return @"左滑可删除安装包";
}

- (void)openAppClicked:(UIButton *)sender
{
	NSInteger idx = sender.tag;
	if (idx >= 0 && idx < (NSInteger)self.installedApps.count) {
		[[NSClassFromString(@"LSApplicationWorkspace") defaultWorkspace] openApplicationWithBundleID:self.installedApps[idx].bundleID];
	}
}

- (void)confirmUninstallAtIndex:(NSInteger)idx
{
	if (idx < 0 || idx >= (NSInteger)self.installedApps.count) {
		return;
	}
	AppRecord *rec = self.installedApps[idx];
	UIAlertController *alert = [UIAlertController alertControllerWithTitle:[NSString stringWithFormat:@"卸载「%@」？", rec.name]
	                                                               message:@"应用及其数据都会被删除。"
	                                                        preferredStyle:UIAlertControllerStyleAlert];
	[alert addAction:[UIAlertAction actionWithTitle:@"取消" style:UIAlertActionStyleCancel handler:nil]];
	[alert addAction:[UIAlertAction actionWithTitle:@"卸载" style:UIAlertActionStyleDestructive handler:^(UIAlertAction *action) {
		run_external_appinst("remove", rec.bundleID.UTF8String, NULL);
		[self reloadCurrentData];
	}]];
	[self presentViewController:alert animated:YES completion:nil];
}

- (void)installIPAClicked:(UIButton *)sender
{
	NSInteger idx = sender.tag;
	if (idx >= 0 && idx < (NSInteger)self.availableIPAs.count) {
		[self executeInstallIPA:self.availableIPAs[idx].filePath];
	}
}

- (void)confirmDeleteIPAAtIndex:(NSInteger)idx
{
	if (idx < 0 || idx >= (NSInteger)self.availableIPAs.count) {
		return;
	}
	IPARecord *rec = self.availableIPAs[idx];
	UIAlertController *alert = [UIAlertController alertControllerWithTitle:[NSString stringWithFormat:@"删除「%@」？", rec.filename]
	                                                               message:nil
	                                                        preferredStyle:UIAlertControllerStyleAlert];
	[alert addAction:[UIAlertAction actionWithTitle:@"取消" style:UIAlertActionStyleCancel handler:nil]];
	[alert addAction:[UIAlertAction actionWithTitle:@"删除" style:UIAlertActionStyleDestructive handler:^(UIAlertAction *action) {
		[NSFileManager.defaultManager removeItemAtPath:rec.filePath error:nil];
		[self reloadCurrentData];
	}]];
	[self presentViewController:alert animated:YES completion:nil];
}

@end

@interface InstallerAppDelegate : UIResponder <UIApplicationDelegate>
@property (strong, nonatomic) UIWindow *window;
@property (strong, nonatomic) InstallerViewController *rootController;
@end

@implementation InstallerAppDelegate

- (BOOL)application:(UIApplication *)application didFinishLaunchingWithOptions:(NSDictionary *)options
{
	self.window = [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
	self.rootController = [[InstallerViewController alloc] init];
	UINavigationController *nav = [[UINavigationController alloc] initWithRootViewController:self.rootController];
	self.window.rootViewController = nav;
	[self.window makeKeyAndVisible];
	return YES;
}

- (BOOL)application:(UIApplication *)app openURL:(NSURL *)url options:(NSDictionary<UIApplicationOpenURLOptionsKey,id> *)options
{
	[self.rootController handleIncomingFile:url];
	return YES;
}

@end

int main(int argc, char *argv[])
{
	@autoreleasepool {
		return UIApplicationMain(argc, argv, nil, NSStringFromClass([InstallerAppDelegate class]));
	}
}
