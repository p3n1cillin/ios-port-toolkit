/* R5Q Hello: the smallest UIKit application, to find out whether an application built here can be installed and run. */
#import <UIKit/UIKit.h>

@interface R5QDelegate : UIResponder <UIApplicationDelegate>
@property (strong, nonatomic) UIWindow *window;
@end

@implementation R5QDelegate
- (BOOL)application:(UIApplication *)application didFinishLaunchingWithOptions:(NSDictionary *)options
{
	self.window = [[UIWindow alloc] initWithFrame:[UIScreen mainScreen].bounds];
	UIViewController *vc = [UIViewController new];
	vc.view.backgroundColor = [UIColor systemTealColor];
	UILabel *label = [[UILabel alloc] initWithFrame:vc.view.bounds];
	label.text = @"Hello from an app built for the S10 Lite";
	label.numberOfLines = 0;
	label.textAlignment = NSTextAlignmentCenter;
	label.font = [UIFont boldSystemFontOfSize:28];
	label.textColor = [UIColor whiteColor];
	label.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
	[vc.view addSubview:label];
	self.window.rootViewController = vc;
	[self.window makeKeyAndVisible];
	NSLog(@"r5q-hello: launched");
	return YES;
}
@end

int
main(int argc, char *argv[])
{
	@autoreleasepool {
		return UIApplicationMain(argc, argv, nil, NSStringFromClass([R5QDelegate class]));
	}
}
