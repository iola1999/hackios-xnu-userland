/*
 * HelloK90: the smallest third-party application, built for arm64 as App Store applications are, to
 * test TrollStore (trollstorehelper): an installed bundle outside /Applications, launched from the home
 * screen, drawing through UIKit. It shows its own name, the time it was opened, and a coloured bar
 * that moves with a finger, so a screenshot tells it ran and touches reached it.
 */
#import <UIKit/UIKit.h>

@interface HelloView : UIView
@end

@implementation HelloView {
	UIView *_bar;
}

- (instancetype)initWithFrame:(CGRect)frame
{
	if ((self = [super initWithFrame:frame])) {
		UILabel *label = [[UILabel alloc] initWithFrame:CGRectInset(frame, 24, frame.size.height / 3)];
		NSDateFormatter *format = [[NSDateFormatter alloc] init];

		format.dateFormat = @"HH:mm:ss";
		self.backgroundColor = UIColor.systemBackgroundColor;
		label.numberOfLines = 0;
		label.textAlignment = NSTextAlignmentCenter;
		label.font = [UIFont systemFontOfSize:28 weight:UIFontWeightSemibold];
		label.text = [NSString stringWithFormat:@"HelloK90\n%@", [format stringFromDate:NSDate.date]];
		[self addSubview:label];
		_bar = [[UIView alloc] initWithFrame:CGRectMake(0, frame.size.height * 0.75, frame.size.width, 24)];
		_bar.backgroundColor = UIColor.systemOrangeColor;
		[self addSubview:_bar];
	}
	return self;
}

- (void)touchesMoved:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event
{
	CGRect frame = _bar.frame;

	frame.origin.y = [touches.anyObject locationInView:self].y;
	_bar.frame = frame;
}
@end

@interface HelloDelegate : UIResponder <UIApplicationDelegate>
@property (strong, nonatomic) UIWindow *window;
@end

@implementation HelloDelegate
- (BOOL)application:(UIApplication *)application didFinishLaunchingWithOptions:(NSDictionary *)options
{
	UIViewController *controller = [[UIViewController alloc] init];

	self.window = [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
	controller.view = [[HelloView alloc] initWithFrame:self.window.bounds];
	self.window.rootViewController = controller;
	[self.window makeKeyAndVisible];
	return YES;
}
@end

int
main(int argc, char *argv[])
{
	@autoreleasepool {
		return UIApplicationMain(argc, argv, nil, NSStringFromClass([HelloDelegate class]));
	}
}
