/*
 * libannibale-ax.dylib: what an application shows, by name and place, for driving the phone from the
 * host without guessing coordinates (tools/k90auto/ui.py).
 *
 * On the darwin notification com.annibale.ax every process this library is in writes
 * /tmp/ax-NAME.txt on its main thread, unless it is an application in the background:
 *
 *   # NAME pid PID screen W H
 *   x y w h <tab> class <tab> label <tab> value
 *
 * one line per view that shows text or is an accessibility element (including the elements of
 * accessibility containers, which is how SwiftUI and table cells list theirs), frames in screen
 * points. Hidden and fully transparent subtrees are left out; covered ones are not detected.
 *
 * SwiftUI views (Settings' lists, Weather) list nothing until the applications load their
 * accessibility bundles. com.annibale.ax.on, taken by SpringBoard, turns application accessibility on
 * for the system (libAccessibility's _AXSApplicationAccessibilitySetEnabled, as an accessibility
 * setting does); applications started afterwards load the bundles. It stays on until the next boot.
 */
#import <UIKit/UIKit.h>
#include <dlfcn.h>
#include <notify.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_DEPTH 64

static NSString *
one_line(NSString *text)
{
	if (![text isKindOfClass:NSString.class] || text.length == 0) {
		return @"";
	}
	text = [text stringByReplacingOccurrencesOfString:@"\t" withString:@" "];
	return [text stringByReplacingOccurrencesOfString:@"\n" withString:@" "];
}

static void
line(NSMutableString *out, CGRect frame, id object, NSString *label, NSString *value)
{
	label = one_line(label);
	value = one_line(value);
	if (label.length == 0 && value.length == 0) {
		return;
	}
	if (CGRectIsEmpty(frame) || CGRectIsNull(frame)) {
		return;
	}
	[out appendFormat:@"%.0f %.0f %.0f %.0f\t%s\t%@\t%@\n", frame.origin.x, frame.origin.y, frame.size.width,
	                  frame.size.height, object_getClassName(object), label, value];
}

/* the elements of an accessibility container, which need not be views */
static void
elements(NSMutableString *out, id container, int depth)
{
	NSInteger count = [container accessibilityElementCount];

	if (depth > MAX_DEPTH || count <= 0 || count == NSNotFound) {
		return;
	}
	for (NSInteger i = 0; i < count && i < 512; i++) {
		id element = [container accessibilityElementAtIndex:i];

		if (element == nil || [element isKindOfClass:UIView.class]) {
			continue;
		}
		if ([element isAccessibilityElement]) {
			line(out, [element accessibilityFrame], element, [element accessibilityLabel], [element accessibilityValue]);
		} else {
			elements(out, element, depth + 1);
		}
	}
}

static void
walk(NSMutableString *out, UIView *view, int depth)
{
	if (depth > MAX_DEPTH || view.hidden || view.alpha < 0.01) {
		return;
	}
	if (view.isAccessibilityElement) {
		line(out, view.accessibilityFrame, view, view.accessibilityLabel, view.accessibilityValue);
	} else if ([view isKindOfClass:UILabel.class]) {
		line(out, [view convertRect:view.bounds toView:nil], view, ((UILabel *)view).text, nil);
	} else if ([view respondsToSelector:@selector(icon)]) {
		/* SpringBoard's icons (SBIconView) are no accessibility elements until the accessibility bundles load */
		id icon = [view performSelector:@selector(icon)];

		if ([icon respondsToSelector:@selector(displayName)]) {
			line(out, [view convertRect:view.bounds toView:nil], view, [icon performSelector:@selector(displayName)], nil);
		}
	} else {
		elements(out, view, depth);
	}
	for (UIView *child in view.subviews) {
		walk(out, child, depth + 1);
	}
}

static void
dump(void)
{
	UIApplication *app = UIApplication.sharedApplication;
	NSMutableString *out;
	NSString *path;
	CGRect screen = CGRectZero;

	if (app == nil) {
		return;
	}
	if (app.applicationState == UIApplicationStateBackground && strcmp(getprogname(), "SpringBoard") != 0) {
		return;
	}
	for (UIScene *scene in app.connectedScenes) {
		if ([scene isKindOfClass:UIWindowScene.class]) {
			screen = ((UIWindowScene *)scene).screen.bounds;
			break;
		}
	}
	out = [NSMutableString stringWithFormat:@"# %s pid %d screen %.0f %.0f\n", getprogname(), getpid(),
	                                        screen.size.width, screen.size.height];
	for (UIScene *scene in app.connectedScenes) {
		if (![scene isKindOfClass:UIWindowScene.class]) {
			continue;
		}
		for (UIWindow *window in ((UIWindowScene *)scene).windows) {
			[out appendFormat:@"# window %s level %.0f%s\n", object_getClassName(window), window.windowLevel,
			                  window.isKeyWindow ? " key" : ""];
			walk(out, window, 0);
		}
	}
	path = [NSString stringWithFormat:@"/tmp/ax-%s.txt", getprogname()];
	[out writeToFile:path atomically:YES encoding:NSUTF8StringEncoding error:nil];
}

static void
accessibility_on(void)
{
	void (*set_enabled)(Boolean) = dlsym(dlopen("/usr/lib/libAccessibility.dylib", RTLD_NOW),
	                                     "_AXSApplicationAccessibilitySetEnabled");

	if (set_enabled != NULL && strcmp(getprogname(), "SpringBoard") == 0) {
		set_enabled(true);
		NSLog(@"ax: application accessibility on");
	}
}

__attribute__((constructor)) static void
ax_start(void)
{
	int token;

	notify_register_dispatch("com.annibale.ax.on", &token, dispatch_get_main_queue(), ^(int t) {
		(void)t;
		accessibility_on();
	});

	notify_register_dispatch("com.annibale.ax", &token, dispatch_get_main_queue(), ^(int t) {
		(void)t;
		@autoreleasepool {
			dump();
		}
	});
}
