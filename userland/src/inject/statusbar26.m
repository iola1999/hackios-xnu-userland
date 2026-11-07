/*
 * Part of libredwood-ax.dylib (tools/k90auto/package.py AX): the status bar's time and battery on
 * 23G90's iPhone 17 canvas (1206x2622, 402 points) placed 12 points closer to the screen edges than
 * iOS places them. Each process lays out its own status bar, so only the processes the library is
 * inserted into get the new place.
 *
 * On that canvas iOS lays the status bar out for the Dynamic Island. The time is centred in the
 * region from the left edge + 17 points to the island - 8, the battery in the region from the
 * island + 8 to the right edge - 18: centres at 73.7 and 327.7 points with the island at
 * 138.3..263.3. The K90 has no island. After -setupInContainerView: this sets the gap between each
 * region and the island to CUTOUT_GAP; the outer edges stay, so each centre moves outward by half
 * of the added gap, to 61.7 and 339.7 (docs/analysis/2026-11-02-status-bar-layout.md).
 *
 * SpringBoard's own status bar uses SystemStatusUI's STUIStatusBarVisualProvider_DynamicSplit; an
 * application's is taken to use UIKitCore's _UIStatusBarVisualProvider_DynamicSplit. In
 * kernel-12377-rate10-w1 Settings showed the time and battery where SpringBoard did, but no line of
 * Settings' reached the log, so which class drew them there is not known. The library changes each
 * of the two classes that is loaded. Both
 * build the two constraints as region.trailing == cutout.leading - 8 and region.leading ==
 * cutout.trailing + 8 and activate them before returning; only constraints of exactly that form are
 * changed. UIKitCore's -cutoutEdgeInset is left at 8 on purpose:
 * -avoidanceFrameUpdatedFromFrame:withAnimationSettings:options: computes the regions' width from it
 * to pick their item scale, and with 32 that width (89.5) is under 0.94 x 96.7, so every item would
 * be drawn at 0.94 from the first time the island changes size. SystemStatusUI has the 8 as a
 * constant in the same computation and keeps full scale.
 */
#import <UIKit/UIKit.h>
#include <mach-o/dyld.h>
#include <objc/runtime.h>
#include <stdio.h>
#include <string.h>

#define IOS_CUTOUT_GAP 8.0
#define CUTOUT_GAP 32.0

@interface NSObject (StatusBar26)
- (UILayoutGuide *)cutoutLayoutGuide;
@end

typedef id (*setup_function)(id, SEL, UIView *);

static setup_function original_stui, original_uikit;

static void
widen_cutout_gap(id provider, UIView *container)
{
	UILayoutGuide *cutout = [provider cutoutLayoutGuide];
	int moved = 0;

	for (NSLayoutConstraint *constraint in container.constraints) {
		if (constraint.secondItem != cutout) {
			continue;
		}
		if (constraint.firstAttribute == NSLayoutAttributeTrailing &&
		    constraint.secondAttribute == NSLayoutAttributeLeading && constraint.constant == -IOS_CUTOUT_GAP) {
			constraint.constant = -CUTOUT_GAP;
			moved++;
		} else if (constraint.firstAttribute == NSLayoutAttributeLeading &&
		    constraint.secondAttribute == NSLayoutAttributeTrailing && constraint.constant == IOS_CUTOUT_GAP) {
			constraint.constant = CUTOUT_GAP;
			moved++;
		}
	}
	fprintf(stderr, "statusbar26: %s %s: %d of 2 region constraints moved\n", getprogname(),
	        object_getClassName(provider), moved);
}

static id
stui_setup(id self, SEL selector, UIView *container)
{
	id regions = original_stui(self, selector, container);

	widen_cutout_gap(self, container);
	return regions;
}

static id
uikit_setup(id self, SEL selector, UIView *container)
{
	id regions = original_uikit(self, selector, container);

	widen_cutout_gap(self, container);
	return regions;
}

static void
install(const char *class_name, IMP replacement, setup_function *original)
{
	Class cls = objc_getClass(class_name);
	Method method = cls ? class_getInstanceMethod(cls, @selector(setupInContainerView:)) : NULL;
	const char *encoding = method ? method_getTypeEncoding(method) : NULL;

	if (cls == Nil) {
		return;
	}
	if (encoding == NULL || strcmp(encoding, "@24@0:8@16") != 0 ||
	    class_getInstanceMethod(cls, @selector(cutoutLayoutGuide)) == NULL) {
		fprintf(stderr, "statusbar26: %s %s: setupInContainerView: is %s, left alone\n", getprogname(),
		        class_name, encoding ? encoding : "missing");
		return;
	}
	*original = (setup_function)method_setImplementation(method, replacement);
	fprintf(stderr, "statusbar26: %s %s: cutout gap %.0f\n", getprogname(), class_name, CUTOUT_GAP);
}

/* again whenever an image is added: SystemStatusUI may be loaded after this library */
static void
install_both(const struct mach_header *header, intptr_t slide)
{
	(void)header;
	(void)slide;
	if (original_stui == NULL) {
		install("STUIStatusBarVisualProvider_DynamicSplit", (IMP)stui_setup, &original_stui);
	}
	if (original_uikit == NULL) {
		install("_UIStatusBarVisualProvider_DynamicSplit", (IMP)uikit_setup, &original_uikit);
	}
}

__attribute__((constructor)) static void
statusbar26_start(void)
{
	_dyld_register_func_for_add_image(install_both);
}
