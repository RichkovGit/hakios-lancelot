/* A2: run the existing scanner in a foreground UIKit process, not a CLI child. */
#import <UIKit/UIKit.h>
#import <os/log.h>
#include <stdio.h>
#include <unistd.h>

extern int bt_test_main(int argc, char **argv);

@interface BTDiagnosticApp : UIResponder <UIApplicationDelegate>
@property (nonatomic, strong) UIWindow *window;
@property (nonatomic, strong) UITextView *report;
@property (nonatomic) BOOL started;
@end

@implementation BTDiagnosticApp
- (BOOL)application:(UIApplication *)app didFinishLaunchingWithOptions:(NSDictionary *)options
{
	(void)app;
	(void)options;
	UIViewController *controller = [UIViewController new];
	self.report = [UITextView new];
	self.report.editable = NO;
	self.report.font = [UIFont monospacedSystemFontOfSize:13 weight:UIFontWeightRegular];
	self.report.text = @"Redwood BLE Test\nIf iOS asks for Bluetooth access, choose Allow to test scanning.";
	controller.view = self.report;
	self.window = [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
	self.window.rootViewController = controller;
	[self.window makeKeyAndVisible];
	return YES;
}

- (void)applicationDidBecomeActive:(UIApplication *)app
{
	/* Keep the display awake while the app scans: SpringBoard's own idleTimerDisabled did not
	 * hold the screen on this port (run 94), and a sleeping screen re-locks and eats the Allow tap. */
	app.idleTimerDisabled = YES;
	if (self.started) return;
	self.started = YES; /* The permission alert can make the app active again. */
	os_log(OS_LOG_DEFAULT, "bt_a2: UIKit active, pid %d, bundle %{public}@", getpid(), NSBundle.mainBundle.bundleIdentifier);
	dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
		/* Capture the unchanged scanner's stdout for on-screen, file and unified-log evidence. */
		NSString *path = [NSTemporaryDirectory() stringByAppendingPathComponent:@"bt-a2.log"];
		if (freopen(path.fileSystemRepresentation, "w", stdout) == NULL) {
			os_log_error(OS_LOG_DEFAULT, "bt_a2: could not capture scanner stdout");
			dispatch_async(dispatch_get_main_queue(), ^{
				self.report.text = @"A2 could not open its report file; the scan did not run.";
			});
			return;
		}
		char name[] = "bt_test", seconds[] = "20";
		char *argv[] = { name, seconds, NULL };
		bt_test_main(2, argv);
		NSString *report = [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:NULL];
		for (NSString *line in [report componentsSeparatedByString:@"\n"]) {
			if (line.length) os_log(OS_LOG_DEFAULT, "%{public}@", line);
		}
		dispatch_async(dispatch_get_main_queue(), ^{
			self.report.text = report ?: @"A2 scan returned, but its report could not be read.";
		});
	});
}
@end

int main(int argc, char **argv)
{
	@autoreleasepool {
		return UIApplicationMain(argc, argv, nil, NSStringFromClass(BTDiagnosticApp.class));
	}
}
