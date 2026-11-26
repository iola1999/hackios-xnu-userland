/*
 * photoimport: insert image files into the system photo library through PhotoKit.
 *
 * Two modes, one binary:
 *
 *   photoimport [--probe] FILE...   the command-line tool. --probe only prints the process
 *                                   credentials, bundle and code-signing identity, PhotoKit
 *                                   authorization states and library availability. This mode
 *                                   stays as the control group: run from ssh it is born in
 *                                   the system session, so its TCC preflight never reaches
 *                                   the user-session tccd.
 *   photoimport                     the application. UIApplicationMain, a window with one
 *                                   status label, and one action per launch. SpringBoard
 *                                   spawns it in the mobile user session, which is the one
 *                                   position where an XPC lookup of com.apple.tccd resolves
 *                                   at all.
 *
 * WHY THE APPLICATION EXISTS. out/goal-1124/batch6/tcc-session-domain.md pinned the 3311
 * refusal to the TCC.framework client: when xpc_connection_create_mach_service cannot
 * resolve com.apple.tccd, tcc_authorization_preflight_with_endpoint returns its byref
 * initial value 0, which is denied in the TCC enumeration. launchd hides user-session
 * endpoints from system-session processes, so every sshd-descended probe -- root or
 * setuid mobile -- measures the same synthetic denied. The only birth channel into the
 * user session on stock iOS is SpringBoard/frontboard launching an application, and
 * AnnibaleTriangle.app is this port's working precedent for exactly that shape.
 *
 * ACTIONS, chosen by the launch URL since a SpringBoard launch carries no argv:
 *
 *   photoimport://probe             print the probe readings, then -- when either access
 *                                   level is notDetermined -- request read-write
 *                                   authorization and record what the callback says. A
 *                                   system prompt appearing is a positive signal: it means
 *                                   the request reached the authorization layer at all.
 *   photoimport://import/<name>     import files. <name> resolves inside
 *                                   /var/mobile/face-import/: a file is imported by itself,
 *                                   a directory contributes its regular files (not
 *                                   recursive). A bare photoimport://import uses the
 *                                   directory /var/mobile/face-import/inbox.
 *   (launched without a URL, e.g.   the first line of /var/mobile/face-import/action.txt
 *   openApplicationWithBundleID:)   when it exists -- "probe" or "import <name>", read and
 *                                   unlinked once -- and the probe when it does not. The
 *                                   file is the fallback channel: it does not depend on
 *                                   LaunchServices resolving the scheme.
 *
 * EVIDENCE, because a SpringBoard launch has neither stdout nor an exit status:
 * everything the CLI would print goes to /var/mobile/face-import/out/<action>.txt, and
 * when the action finishes a marker /var/mobile/face-import/out/<action>.done appears with
 * the result code. The done file is removed when the action starts, so a waiter that sees
 * it knows the run it belongs to. One process performs one action and then exits: the
 * in-process PLPrivacy authorization cache makes a second measurement in the same process
 * a stale one. The label on screen mirrors the state for screenshots.
 */
#import <Foundation/Foundation.h>
#import <Photos/Photos.h>
#import <UIKit/UIKit.h>
#include <dispatch/dispatch.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

typedef const struct __SecTask *SecTaskRef;
extern SecTaskRef SecTaskCreateFromSelf(CFAllocatorRef allocator);
extern CFTypeRef SecTaskCopyValueForEntitlement(SecTaskRef task, CFStringRef entitlement, CFErrorRef *error);
extern CFStringRef SecTaskCopySigningIdentifier(SecTaskRef task, CFErrorRef *error);
extern uint32_t SecTaskGetCodeSignStatus(SecTaskRef task);

#define CS_ENTITLEMENTS_VALIDATED 0x00004000U
#define CS_PLATFORM_BINARY 0x04000000U
#define CS_DEBUGGED 0x10000000U
#define CS_SIGNED 0x20000000U

static const char *
authorization_name(PHAuthorizationStatus status)
{
	switch (status) {
	case PHAuthorizationStatusNotDetermined: return "not-determined";
	case PHAuthorizationStatusRestricted: return "restricted";
	case PHAuthorizationStatusDenied: return "denied";
	case PHAuthorizationStatusAuthorized: return "authorized";
	case PHAuthorizationStatusLimited: return "limited";
	}
	return "unknown";
}

static void
print_cf_value(const char *label, CFTypeRef value)
{
	if (!value) {
		printf("photoimport: %s (null)\n", label);
		return;
	}
	if (CFGetTypeID(value) == CFBooleanGetTypeID()) {
		printf("photoimport: %s %s\n", label, CFBooleanGetValue(value) ? "true" : "false");
		CFRelease(value);
		return;
	}
	CFStringRef description = CFCopyDescription(value);
	NSString *text = (__bridge_transfer NSString *)description;
	printf("photoimport: %s %s\n", label, text.UTF8String ?: "(no description)");
	CFRelease(value);
}

static void
print_task_identity(void)
{
	SecTaskRef task = SecTaskCreateFromSelf(kCFAllocatorDefault);
	if (!task) {
		printf("photoimport: SecTaskCreateFromSelf returned null\n");
		return;
	}

	uint32_t csflags = SecTaskGetCodeSignStatus(task);
	printf("photoimport: codesign status 0x%08x signed %s entitlements-validated %s platform-binary %s debugged %s\n",
	    csflags,
	    (csflags & CS_SIGNED) ? "yes" : "no",
	    (csflags & CS_ENTITLEMENTS_VALIDATED) ? "yes" : "no",
	    (csflags & CS_PLATFORM_BINARY) ? "yes" : "no",
	    (csflags & CS_DEBUGGED) ? "yes" : "no");
	print_cf_value("signing identifier", SecTaskCopySigningIdentifier(task, NULL));
	print_cf_value("entitlement application-identifier",
	    SecTaskCopyValueForEntitlement(task, CFSTR("application-identifier"), NULL));
	print_cf_value("entitlement platform-application",
	    SecTaskCopyValueForEntitlement(task, CFSTR("platform-application"), NULL));
	print_cf_value("entitlement no-container",
	    SecTaskCopyValueForEntitlement(task, CFSTR("com.apple.private.security.no-container"), NULL));
	print_cf_value("entitlement tcc.allow",
	    SecTaskCopyValueForEntitlement(task, CFSTR("com.apple.private.tcc.allow"), NULL));
	CFRelease(task);
}

static void
print_probe(void)
{
	PHAuthorizationStatus add = [PHPhotoLibrary authorizationStatusForAccessLevel:PHAccessLevelAddOnly];
	PHAuthorizationStatus readwrite = [PHPhotoLibrary authorizationStatusForAccessLevel:PHAccessLevelReadWrite];
	PHPhotoLibrary *library = [PHPhotoLibrary sharedPhotoLibrary];
	NSError *unavailable = library.unavailabilityReason;
	NSBundle *bundle = NSBundle.mainBundle;

	printf("photoimport: uid %u gid %u home %s\n", getuid(), getgid(), getenv("HOME") ?: "(unset)");
	printf("photoimport: bundle path %s, identifier %s\n",
	    bundle.bundlePath.UTF8String ?: "(null)", bundle.bundleIdentifier.UTF8String ?: "(null)");
	print_task_identity();
	printf("photoimport: authorization add-only %ld %s, read-write %ld %s\n",
	    (long)add, authorization_name(add), (long)readwrite, authorization_name(readwrite));
	if (unavailable) {
		printf("photoimport: library unavailable: domain %s code %ld: %s\n",
		    unavailable.domain.UTF8String, (long)unavailable.code,
		    unavailable.localizedDescription.UTF8String);
	} else {
		printf("photoimport: library available\n");
	}
}

/*
 * The notDetermined branch of the probe. requestAuthorization is the one call that can
 * raise a system prompt, and it is only made here in the application -- the command-line
 * mode keeps its original behaviour of never asking, because in the system session the
 * request cannot reach tccd anyway. The wait is unbounded on purpose: if a prompt is up,
 * the answer arrives whenever someone taps it, and the process staying resident meanwhile
 * is exactly the triangle application's proven shape.
 */
static void
request_authorization_if_undetermined(void)
{
	PHAuthorizationStatus add = [PHPhotoLibrary authorizationStatusForAccessLevel:PHAccessLevelAddOnly];
	PHAuthorizationStatus readwrite = [PHPhotoLibrary authorizationStatusForAccessLevel:PHAccessLevelReadWrite];

	if (add != PHAuthorizationStatusNotDetermined && readwrite != PHAuthorizationStatusNotDetermined) {
		printf("photoimport: authorization already determined, no request made\n");
		return;
	}
	printf("photoimport: authorization not determined, requesting read-write authorization; "
	    "a system prompt is a positive signal here\n");
	fflush(stdout);
	dispatch_semaphore_t answered_gate = dispatch_semaphore_create(0);

	[PHPhotoLibrary requestAuthorizationForAccessLevel:PHAccessLevelReadWrite
	                                           handler:^(PHAuthorizationStatus status) {
		printf("photoimport: requestAuthorization callback: status %ld %s\n",
		    (long)status, authorization_name(status));
		fflush(stdout);
		dispatch_semaphore_signal(answered_gate);
	}];
	dispatch_semaphore_wait(answered_gate, DISPATCH_TIME_FOREVER);
	PHAuthorizationStatus final_add = [PHPhotoLibrary authorizationStatusForAccessLevel:PHAccessLevelAddOnly];
	PHAuthorizationStatus final_rw = [PHPhotoLibrary authorizationStatusForAccessLevel:PHAccessLevelReadWrite];

	printf("photoimport: after request: add-only %ld %s, read-write %ld %s\n",
	    (long)final_add, authorization_name(final_add), (long)final_rw, authorization_name(final_rw));
}

static void
print_error(NSString *path, NSError *error)
{
	NSString *reason = error.localizedFailureReason;
	NSString *suggestion = error.localizedRecoverySuggestion;

	printf("photoimport: %s: import failed domain %s code %ld: %s\n", path.UTF8String,
	    error.domain.UTF8String, (long)error.code, error.localizedDescription.UTF8String);
	if (reason.length) {
		printf("photoimport: %s: failure reason: %s\n", path.UTF8String, reason.UTF8String);
	}
	if (suggestion.length) {
		printf("photoimport: %s: recovery suggestion: %s\n", path.UTF8String, suggestion.UTF8String);
	}
	/* The full userInfo is evidence: PhotoKit denials carry their origin in keys the
	 * localized text drops. */
	printf("photoimport: %s: error userInfo %s\n", path.UTF8String,
	    error.userInfo.description.UTF8String ?: "(empty)");
}

static int
import_file(NSString *path)
{
	BOOL directory = NO;
	NSError *read_error = nil;
	NSError *change_error = nil;
	NSData *data;
	__block NSString *local_identifier = nil;
	BOOL ok;

	if (![NSFileManager.defaultManager fileExistsAtPath:path isDirectory:&directory] || directory) {
		printf("photoimport: %s: not a regular file\n", path.UTF8String);
		return 1;
	}
	data = [NSData dataWithContentsOfFile:path options:0 error:&read_error];
	if (data.length == 0) {
		printf("photoimport: %s: read failed: %s\n", path.UTF8String,
		    read_error.localizedDescription.UTF8String ?: "empty file");
		return 1;
	}

	@try {
		ok = [[PHPhotoLibrary sharedPhotoLibrary] performChangesAndWait:^{
			PHAssetCreationRequest *request = [PHAssetCreationRequest creationRequestForAsset];
			PHAssetResourceCreationOptions *options = [PHAssetResourceCreationOptions new];

			options.originalFilename = path.lastPathComponent;
			/* Passing bytes avoids a second filesystem access decision inside assetsd. */
			[request addResourceWithType:PHAssetResourceTypePhoto data:data options:options];
			local_identifier = [request.placeholderForCreatedAsset.localIdentifier copy];
		} error:&change_error];
	} @catch (NSException *exception) {
		printf("photoimport: %s: PhotoKit exception %s: %s\n", path.UTF8String,
		    exception.name.UTF8String, exception.reason.UTF8String ?: "");
		return 2;
	}

	if (!ok) {
		if (change_error) {
			print_error(path, change_error);
		} else {
			printf("photoimport: %s: import failed without an error object\n", path.UTF8String);
		}
		return 2;
	}
	printf("photoimport: %s: imported %lu bytes as %s\n", path.UTF8String,
	    (unsigned long)data.length, local_identifier.UTF8String ?: "(identifier unavailable)");
	return 0;
}

/* ------------------------------------------------------------------ the application */

#define PI_BASE_DIR @"/var/mobile/face-import"
#define PI_OUT_DIR @"/var/mobile/face-import/out"

static UILabel *pi_label;
static NSMutableArray<NSString *> *pi_lines;
static BOOL pi_action_taken;
static dispatch_queue_t pi_queue;

/*
 * One more line on the label, always on the main thread. The last several lines are kept
 * rather than only the newest, because the label doubles as the photograph of what the
 * run did for anyone who screenshots it after the fact.
 */
static void
pi_status(NSString *line)
{
	dispatch_async(dispatch_get_main_queue(), ^{
		if (!pi_lines) {
			pi_lines = [NSMutableArray new];
		}
		[pi_lines addObject:line];
		while (pi_lines.count > 12) {
			[pi_lines removeObjectAtIndex:0];
		}
		pi_label.text = [pi_lines componentsJoinedByString:@"\n"];
	});
}

/* The run is over: the marker file carries the result code because a terminated
 * application has no exit status anyone can read. */
static void
pi_write_done(NSString *action, int rc, NSString *summary)
{
	NSString *done = [PI_OUT_DIR stringByAppendingPathComponent:[action stringByAppendingString:@".done"]];
	NSString *contents = [NSString stringWithFormat:
	    @"photoimport: done action %@ rc %d %@ at %@\n", action, rc, summary, NSDate.date];
	NSError *error = nil;

	if (![contents writeToFile:done atomically:YES encoding:NSUTF8StringEncoding error:&error]) {
		printf("photoimport: cannot write %s: %s\n", done.UTF8String,
		    error.localizedDescription.UTF8String ?: "?");
	}
}

static void
pi_finish(NSString *action, int rc, NSString *summary)
{
	fflush(stdout);
	pi_write_done(action, rc, summary);
	pi_status([NSString stringWithFormat:@"%@ finished rc=%d", action, rc]);
	/* A short grace so the final state renders at least one frame before the process
	 * goes; the window's last content is what a late screenshot will show. */
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(1.0 * NSEC_PER_SEC)),
	    dispatch_get_main_queue(), ^{
		exit(rc);
	});
}

/*
 * The fallback action channel: the first line of /var/mobile/face-import/action.txt,
 * "probe" or "import <name>". A launch by URL is the primary channel, but it depends on
 * LaunchServices having resolved the scheme, and a boot spent finding out that it did
 * not is a boot lost; this file only needs the directory the evidence already lives in.
 * It is consumed once -- read and unlinked -- so a stale copy cannot hijack later
 * launches. Returns YES when the file supplied the action.
 */
static BOOL
pi_action_from_control_file(NSString **action_out, NSString **argument_out)
{
	NSString *control = [PI_BASE_DIR stringByAppendingPathComponent:@"action.txt"];
	NSString *text = [NSString stringWithContentsOfFile:control encoding:NSUTF8StringEncoding error:nil];

	if (!text.length) {
		return NO;
	}
	[NSFileManager.defaultManager removeItemAtPath:control error:nil];
	NSString *line = [[text componentsSeparatedByCharactersInSet:NSCharacterSet.newlineCharacterSet].firstObject
	    stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
	NSRange space = [line rangeOfCharacterFromSet:NSCharacterSet.whitespaceCharacterSet];
	NSString *verb = space.location == NSNotFound ? line : [line substringToIndex:space.location];
	NSString *rest = space.location == NSNotFound ? @"" :
	    [[line substringFromIndex:space.location] stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];

	if ([verb isEqualToString:@"import"]) {
		*action_out = @"import";
		*argument_out = rest.length ? rest : nil;
		return YES;
	}
	if ([verb isEqualToString:@"probe"]) {
		*action_out = @"probe";
		return YES;
	}
	return NO;
}

/* What the launch asks for. A URL's action is the host, the import target the path:
 * photoimport://import/wiki1.heic parses as host "import", path "/wiki1.heic". A launch
 * without a URL asks the control file, and with neither it is the probe. */
static void
pi_run_action(NSURL *url)
{
	@autoreleasepool {
		NSString *action = @"probe";
		NSString *argument = nil;
		const char *source = "default";

		if (url) {
			NSString *host = url.host.lowercaseString;

			source = "url";
			if ([host isEqualToString:@"import"]) {
				NSString *target = url.path.stringByRemovingPercentEncoding ?: @"";

				action = @"import";
				if ([target hasPrefix:@"/"]) {
					target = [target substringFromIndex:1];
				}
				if (target.length) {
					argument = target;
				}
			}
		} else if (pi_action_from_control_file(&action, &argument)) {
			source = "control file";
		}

		NSFileManager *fm = NSFileManager.defaultManager;
		NSString *outpath = [PI_OUT_DIR stringByAppendingPathComponent:[action stringByAppendingString:@".txt"]];
		NSString *donepath = [PI_OUT_DIR stringByAppendingPathComponent:[action stringByAppendingString:@".done"]];
		NSError *error = nil;

		[fm removeItemAtPath:donepath error:nil];
		if (![fm createDirectoryAtPath:PI_OUT_DIR withIntermediateDirectories:YES
		                    attributes:nil error:&error] || !freopen(outpath.UTF8String, "w", stdout)) {
			/* Nowhere to write is the one failure the files cannot say: it goes to the
			 * console instead, which init leaves world-writable for exactly this. */
			int console = open("/dev/console", O_WRONLY);
			char line[512];
			int n = snprintf(line, sizeof(line),
			    "photoimport: cannot write evidence under %s: %s\n", PI_OUT_DIR.UTF8String,
			    error.localizedDescription.UTF8String ?: "(fopen failed)");

			if (console >= 0) {
				(void)write(console, line, (size_t)n);
				close(console);
			}
			pi_status([NSString stringWithFormat:@"cannot write %@", PI_OUT_DIR]);
			exit(3);
		}
		setvbuf(stdout, NULL, _IOLBF, 0);

		printf("photoimport: application mode, pid %d, action %s%s%s via %s\n", (int)getpid(),
		    action.UTF8String, argument ? " " : "", argument ? argument.UTF8String : "", source);
		printf("photoimport: launch url %s\n", url.absoluteString.UTF8String ?: "(none)");
		print_probe();

		int rc = 0;
		NSString *summary = @"";

		if ([action isEqualToString:@"probe"]) {
			request_authorization_if_undetermined();
			summary = [NSString stringWithFormat:
			    @"add-only %ld read-write %ld",
			    (long)[PHPhotoLibrary authorizationStatusForAccessLevel:PHAccessLevelAddOnly],
			    (long)[PHPhotoLibrary authorizationStatusForAccessLevel:PHAccessLevelReadWrite]];
		} else {
			NSString *target = argument ? [PI_BASE_DIR stringByAppendingPathComponent:argument]
			    : [PI_BASE_DIR stringByAppendingPathComponent:@"inbox"];
			BOOL directory = NO;
			NSArray<NSString *> *files;
			int files_count = 0, failures = 0;

			pi_status([NSString stringWithFormat:@"importing %@", target]);
			if (![fm fileExistsAtPath:target isDirectory:&directory]) {
				printf("photoimport: %s: no such file or directory\n", target.UTF8String);
				files = @[];
				rc = 1;
			} else if (!directory) {
				files = @[ target ];
			} else {
				NSError *list_error = nil;
				NSArray<NSString *> *entries = [fm contentsOfDirectoryAtPath:target error:&list_error];
				NSMutableArray<NSString *> *collected = [NSMutableArray new];

				if (!entries) {
					printf("photoimport: %s: cannot list: %s\n", target.UTF8String,
					    list_error.localizedDescription.UTF8String ?: "?");
				}
				for (NSString *entry in [entries sortedArrayUsingSelector:@selector(compare:)]) {
					NSString *full = [target stringByAppendingPathComponent:entry];
					BOOL entry_is_dir = NO;

					if ([fm fileExistsAtPath:full isDirectory:&entry_is_dir] && !entry_is_dir) {
						[collected addObject:full];
					}
				}
				files = collected;
				printf("photoimport: %s: %lu regular file(s) in directory\n", target.UTF8String,
				    (unsigned long)files.count);
			}
			for (NSString *file in files) {
				files_count++;
				failures += import_file(file) != 0;
			}
			printf("photoimport: done: %d file(s), %d failure(s)\n", files_count, failures);
			if (failures) {
				rc = 2;
			}
			summary = [NSString stringWithFormat:@"files %d failures %d", files_count, failures];
		}
		pi_finish(action, rc, summary);
	}
}

/* The first delivery wins; both didFinishLaunching's options and openURL carry the same
 * URL on a cold launch by URL, so the second delivery is the same action restated. */
static void
pi_schedule(NSURL *url)
{
	if (pi_action_taken) {
		printf("photoimport: ignoring %s, this launch already has its action\n",
		    url.absoluteString.UTF8String ?: "(null url)");
		return;
	}
	pi_action_taken = YES;
	pi_status(url ? [NSString stringWithFormat:@"action %@", url.absoluteString] : @"action probe");
	NSURL *copy = [url copy];

	dispatch_async(pi_queue, ^{
		pi_run_action(copy);
	});
}

@interface PIStatusController : UIViewController
@end

@implementation PIStatusController

- (void)loadView
{
	UIView *view = [[UIView alloc] initWithFrame:[UIScreen mainScreen].bounds];
	CGRect bounds = view.bounds;
	UILabel *label = [[UILabel alloc] initWithFrame:CGRectInset(bounds, 24, 60)];

	/* Dark blue-grey, far from anything SpringBoard draws itself, so a screenshot that
	 * shows it can only be this application. */
	view.backgroundColor = [UIColor colorWithRed:0.09 green:0.13 blue:0.22 alpha:1.0];
	label.textColor = [UIColor whiteColor];
	label.font = [UIFont systemFontOfSize:16];
	label.numberOfLines = 0;
	label.textAlignment = NSTextAlignmentCenter;
	label.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
	label.text = @"PhotoImport launched";
	[view addSubview:label];
	pi_label = label;
	self.view = view;
}

- (BOOL)prefersStatusBarHidden
{
	return YES;
}

- (BOOL)prefersHomeIndicatorAutoHidden
{
	return YES;
}

@end

@interface PIAppDelegate : NSObject <UIApplicationDelegate>
@property (nonatomic, strong) UIWindow *window;
@end

@implementation PIAppDelegate

- (BOOL)application:(UIApplication *)application
    didFinishLaunchingWithOptions:(NSDictionary<UIApplicationLaunchOptionsKey, id> *)options
{
	UIWindow *w = [[UIWindow alloc] initWithFrame:[UIScreen mainScreen].bounds];

	w.rootViewController = [[PIStatusController alloc] init];
	self.window = w;
	[w makeKeyAndVisible];
	pi_schedule(options[UIApplicationLaunchOptionsURLKey]);
	return YES;
}

- (BOOL)application:(UIApplication *)application
            openURL:(NSURL *)url
            options:(NSDictionary<UIApplicationOpenURLOptionsKey, id> *)options
{
	pi_schedule(url);
	return YES;
}

@end

/* ------------------------------------------------------------------------------- main */

int
main(int argc, char **argv)
{
	@autoreleasepool {
		BOOL probe = NO;
		int first = 1, failures = 0, files = 0;

		setvbuf(stdout, NULL, _IOLBF, 0);
		for (; first < argc; first++) {
			if (strcmp(argv[first], "--") == 0) {
				first++;
				break;
			}
			if (strcmp(argv[first], "--probe") == 0) {
				probe = YES;
				continue;
			}
			if (strcmp(argv[first], "--help") == 0) {
				printf("usage: photoimport [--probe] FILE...\n");
				printf("with no arguments at all the binary is the application: UIApplicationMain,\n");
				printf("one action per launch chosen by the launch URL, evidence in\n");
				printf("/var/mobile/face-import/out/ -- see the file header comment.\n");
				return 0;
			}
			break;
		}
		if (first >= argc && !probe) {
			/*
			 * No arguments: the application. SpringBoard launches exactly this way, so
			 * argc == 1 cannot also stay the CLI's usage-error case. The work runs on a
			 * serial queue of its own -- performChangesAndWait blocks, and blocking the
			 * main thread before the first frame is how launch watchdogs get interested.
			 */
			pi_queue = dispatch_queue_create("com.annibale.photoimport.action", NULL);
			return UIApplicationMain(argc, argv, nil,
			    NSStringFromClass([PIAppDelegate class]));
		}
		if (probe) {
			print_probe();
		}
		for (int i = first; i < argc; i++) {
			files++;
			failures += import_file([NSString stringWithUTF8String:argv[i]]) != 0;
		}
		if (files) {
			printf("photoimport: done: %d file(s), %d failure(s)\n", files, failures);
		}
		return failures ? 2 : 0;
	}
}
