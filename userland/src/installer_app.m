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

static BOOL perform_app_uninstall(NSString *bundleID, NSString *bundlePath)
{
	NSFileManager *fm = NSFileManager.defaultManager;
	LSApplicationWorkspace *ws = [NSClassFromString(@"LSApplicationWorkspace") defaultWorkspace];
	LSApplicationProxy *proxy = [NSClassFromString(@"LSApplicationProxy") applicationProxyForIdentifier:bundleID];

	if (proxy != nil && proxy.bundleURL != nil) {
		[ws unregisterApplication:proxy.bundleURL];
	} else if (bundlePath != nil) {
		[ws unregisterApplication:[NSURL fileURLWithPath:bundlePath]];
	}

	if (bundlePath != nil) {
		NSString *containerDir = [bundlePath stringByDeletingLastPathComponent];
		[fm removeItemAtPath:containerDir error:nil];
	}

	dlopen("/System/Library/PrivateFrameworks/MobileContainerManager.framework/MobileContainerManager", RTLD_NOW);
	Class mcmClass = NSClassFromString(@"MCMAppDataContainer");
	if (mcmClass != Nil) {
		MCMContainer *data = [mcmClass containerWithIdentifier:bundleID createIfNecessary:NO existed:NULL error:NULL];
		if (data != nil && data.url != nil) {
			[fm removeItemAtPath:data.url.path error:nil];
		}
	}

	NSString *dataRoot = @"/private/var/mobile/Containers/Data/Application";
	for (NSString *duuid in [fm contentsOfDirectoryAtPath:dataRoot error:nil]) {
		NSString *dpath = [dataRoot stringByAppendingPathComponent:duuid];
		NSString *meta = [dpath stringByAppendingPathComponent:@".com.apple.mobile_container_manager.metadata.plist"];
		NSDictionary *mdict = [NSDictionary dictionaryWithContentsOfFile:meta];
		if ([mdict[@"MCMMetadataIdentifier"] isEqualToString:bundleID]) {
			[fm removeItemAtPath:dpath error:nil];
		}
	}

	notify_post("com.redwood.var_save");
	return YES;
}

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

	extern char **environ;
	pid_t pid;
	int status = posix_spawn(&pid, bin_path, &actions, NULL, argv, environ);
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

@interface InstallerViewController : UIViewController <UITableViewDataSource, UITableViewDelegate>
@property (nonatomic, strong) UISegmentedControl *segmentedControl;
@property (nonatomic, strong) UITableView *tableView;
@property (nonatomic, strong) UIRefreshControl *refreshControl;
@property (nonatomic, strong) NSMutableArray<AppRecord *> *installedApps;
@property (nonatomic, strong) NSMutableArray<IPARecord *> *availableIPAs;
@property (nonatomic, strong) UIActivityIndicatorView *activityIndicator;
- (void)handleIncomingFile:(NSURL *)fileURL;
@end

@implementation InstallerViewController

- (void)viewDidLoad
{
	[super viewDidLoad];
	self.title = @"IPA 管理器";
	self.view.backgroundColor = UIColor.systemBackgroundColor;

	self.installedApps = [NSMutableArray array];
	self.availableIPAs = [NSMutableArray array];

	self.segmentedControl = [[UISegmentedControl alloc] initWithItems:@[ @"已安装应用", @"可用安装包" ]];
	self.segmentedControl.selectedSegmentIndex = 0;
	[self.segmentedControl addTarget:self action:@selector(segmentChanged:) forControlEvents:UIControlEventValueChanged];
	self.navigationItem.titleView = self.segmentedControl;

	UIBarButtonItem *cleanButton = [[UIBarButtonItem alloc] initWithTitle:@"清理残留"
	                                                                style:UIBarButtonItemStylePlain
	                                                               target:self
	                                                               action:@selector(cleanTestAppsClicked)];
	self.navigationItem.leftBarButtonItem = cleanButton;

	UIBarButtonItem *refreshButton = [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemRefresh
	                                                                                target:self
	                                                                                action:@selector(reloadCurrentData)];
	self.navigationItem.rightBarButtonItem = refreshButton;

	self.tableView = [[UITableView alloc] initWithFrame:self.view.bounds style:UITableViewStyleInsetGrouped];
	self.tableView.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
	self.tableView.dataSource = self;
	self.tableView.delegate = self;
	self.tableView.rowHeight = 84;
	[self.view addSubview:self.tableView];

	self.refreshControl = [[UIRefreshControl alloc] init];
	[self.refreshControl addTarget:self action:@selector(reloadCurrentData) forControlEvents:UIControlEventValueChanged];
	self.tableView.refreshControl = self.refreshControl;

	self.activityIndicator = [[UIActivityIndicatorView alloc] initWithActivityIndicatorStyle:UIActivityIndicatorViewStyleLarge];
	self.activityIndicator.center = self.view.center;
	self.activityIndicator.hidesWhenStopped = YES;
	[self.view addSubview:self.activityIndicator];

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
		int rc = run_external_appinst("clean", "", &cliOut);
		if (rc != 0) {
			for (AppRecord *rec in [self.installedApps copy]) {
				if (is_test_identifier(rec.bundleID)) {
					perform_app_uninstall(rec.bundleID, rec.bundlePath);
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
	[self.activityIndicator startAnimating];
	self.view.userInteractionEnabled = NO;

	dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
		NSString *output = nil;
		int res = run_external_appinst("install", ipaPath.fileSystemRepresentation, &output);

		dispatch_async(dispatch_get_main_queue(), ^{
			[self.activityIndicator stopAnimating];
			self.view.userInteractionEnabled = YES;

			NSString *msg = (res == 0) ? @"应用已成功安装并在主屏注册。" : [NSString stringWithFormat:@"安装失败，错误码：%d\n%@", res, output ?: @""];
			UIAlertController *resultAlert = [UIAlertController alertControllerWithTitle:(res == 0 ? @"安装成功" : @"安装失败")
			                                                                     message:msg
			                                                              preferredStyle:UIAlertControllerStyleAlert];
			[resultAlert addAction:[UIAlertAction actionWithTitle:@"确定" style:UIAlertActionStyleDefault handler:^(UIAlertAction *action) {
				self.segmentedControl.selectedSegmentIndex = 0;
				[self reloadCurrentData];
			}]];
			[self presentViewController:resultAlert animated:YES completion:nil];
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
	if (self.segmentedControl.selectedSegmentIndex == 0) {
		return self.installedApps.count;
	} else {
		return self.availableIPAs.count;
	}
}

- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath
{
	static NSString *CellID = @"InstallerCell";
	UITableViewCell *cell = [tableView dequeueReusableCellWithIdentifier:CellID];
	if (cell == nil) {
		cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle reuseIdentifier:CellID];
		cell.selectionStyle = UITableViewCellSelectionStyleNone;
	}

	if (self.segmentedControl.selectedSegmentIndex == 0) {
		AppRecord *record = self.installedApps[indexPath.row];
		cell.textLabel.text = record.name;
		cell.textLabel.font = [UIFont systemFontOfSize:17 weight:UIFontWeightMedium];
		cell.detailTextLabel.numberOfLines = 2;
		cell.detailTextLabel.text = [NSString stringWithFormat:@"%@ • 版本: %@ • 大小: %@",
		                             record.bundleID, record.version, format_size_string(record.sizeBytes)];

		UIView *actionView = [[UIView alloc] initWithFrame:CGRectMake(0, 0, 140, 36)];
		UIButton *openBtn = [UIButton buttonWithType:UIButtonTypeSystem];
		openBtn.frame = CGRectMake(0, 0, 64, 36);
		[openBtn setTitle:@"打开" forState:UIControlStateNormal];
		openBtn.backgroundColor = UIColor.systemBlueColor;
		[openBtn setTitleColor:UIColor.whiteColor forState:UIControlStateNormal];
		openBtn.layer.cornerRadius = 8;
		openBtn.titleLabel.font = [UIFont systemFontOfSize:14 weight:UIFontWeightMedium];
		openBtn.tag = indexPath.row;
		[openBtn addTarget:self action:@selector(openAppClicked:) forControlEvents:UIControlEventTouchUpInside];
		[actionView addSubview:openBtn];

		UIButton *delBtn = [UIButton buttonWithType:UIButtonTypeSystem];
		delBtn.frame = CGRectMake(72, 0, 64, 36);
		[delBtn setTitle:@"卸载" forState:UIControlStateNormal];
		delBtn.backgroundColor = UIColor.systemRedColor;
		[delBtn setTitleColor:UIColor.whiteColor forState:UIControlStateNormal];
		delBtn.layer.cornerRadius = 8;
		delBtn.titleLabel.font = [UIFont systemFontOfSize:14 weight:UIFontWeightMedium];
		delBtn.tag = indexPath.row;
		[delBtn addTarget:self action:@selector(uninstallAppClicked:) forControlEvents:UIControlEventTouchUpInside];
		[actionView addSubview:delBtn];

		cell.accessoryView = actionView;
	} else {
		IPARecord *record = self.availableIPAs[indexPath.row];
		cell.textLabel.text = record.filename;
		cell.textLabel.font = [UIFont systemFontOfSize:16 weight:UIFontWeightMedium];
		cell.detailTextLabel.numberOfLines = 2;
		cell.detailTextLabel.text = [NSString stringWithFormat:@"大小: %@ • 日期: %@",
		                             format_size_string(record.sizeBytes), record.modDate];

		UIView *actionView = [[UIView alloc] initWithFrame:CGRectMake(0, 0, 140, 36)];
		UIButton *installBtn = [UIButton buttonWithType:UIButtonTypeSystem];
		installBtn.frame = CGRectMake(0, 0, 64, 36);
		[installBtn setTitle:@"安装" forState:UIControlStateNormal];
		installBtn.backgroundColor = UIColor.systemGreenColor;
		[installBtn setTitleColor:UIColor.whiteColor forState:UIControlStateNormal];
		installBtn.layer.cornerRadius = 8;
		installBtn.titleLabel.font = [UIFont systemFontOfSize:14 weight:UIFontWeightMedium];
		installBtn.tag = indexPath.row;
		[installBtn addTarget:self action:@selector(installIPAClicked:) forControlEvents:UIControlEventTouchUpInside];
		[actionView addSubview:installBtn];

		UIButton *delFileBtn = [UIButton buttonWithType:UIButtonTypeSystem];
		delFileBtn.frame = CGRectMake(72, 0, 64, 36);
		[delFileBtn setTitle:@"删除" forState:UIControlStateNormal];
		delFileBtn.backgroundColor = UIColor.systemGrayColor;
		[delFileBtn setTitleColor:UIColor.whiteColor forState:UIControlStateNormal];
		delFileBtn.layer.cornerRadius = 8;
		delFileBtn.titleLabel.font = [UIFont systemFontOfSize:14 weight:UIFontWeightMedium];
		delFileBtn.tag = indexPath.row;
		[delFileBtn addTarget:self action:@selector(deleteIPAClicked:) forControlEvents:UIControlEventTouchUpInside];
		[actionView addSubview:delFileBtn];

		cell.accessoryView = actionView;
	}

	return cell;
}

- (void)openAppClicked:(UIButton *)sender
{
	NSInteger idx = sender.tag;
	if (idx >= 0 && idx < (NSInteger)self.installedApps.count) {
		AppRecord *rec = self.installedApps[idx];
		LSApplicationWorkspace *ws = [NSClassFromString(@"LSApplicationWorkspace") defaultWorkspace];
		[ws openApplicationWithBundleID:rec.bundleID];
	}
}

- (void)uninstallAppClicked:(UIButton *)sender
{
	NSInteger idx = sender.tag;
	if (idx < 0 || idx >= (NSInteger)self.installedApps.count) {
		return;
	}
	AppRecord *rec = self.installedApps[idx];

	UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"卸载应用"
	                                                               message:[NSString stringWithFormat:@"确定彻底卸载 %@ 吗？应用数据与配置将一并删除。", rec.name]
	                                                        preferredStyle:UIAlertControllerStyleAlert];

	[alert addAction:[UIAlertAction actionWithTitle:@"取消" style:UIAlertActionStyleCancel handler:nil]];
	[alert addAction:[UIAlertAction actionWithTitle:@"彻底卸载" style:UIAlertActionStyleDestructive handler:^(UIAlertAction *action) {
		perform_app_uninstall(rec.bundleID, rec.bundlePath);
		[self reloadCurrentData];
	}]];

	[self presentViewController:alert animated:YES completion:nil];
}

- (void)installIPAClicked:(UIButton *)sender
{
	NSInteger idx = sender.tag;
	if (idx < 0 || idx >= (NSInteger)self.availableIPAs.count) {
		return;
	}
	IPARecord *rec = self.availableIPAs[idx];
	[self executeInstallIPA:rec.filePath];
}

- (void)deleteIPAClicked:(UIButton *)sender
{
	NSInteger idx = sender.tag;
	if (idx < 0 || idx >= (NSInteger)self.availableIPAs.count) {
		return;
	}
	IPARecord *rec = self.availableIPAs[idx];

	UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"删除安装包"
	                                                               message:[NSString stringWithFormat:@"确定删除安装包 %@ 吗？", rec.filename]
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
