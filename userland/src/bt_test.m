/*
 * bt_test: Bluetooth as an application sees it: CoreBluetooth's central manager -- its state, then a scan for what advertises around
 * (names, RSSI, services) for SECONDS (default 30). The way from here to the radio is bluetoothd's: XPC to it, its HCI over
 * /dev/uart.bluetooth's Skywalk channel, and the MediaTek controller behind CONNSYS (bsd/dev/redwood_connsys.c, connsys_bt=2).
 *
 * Everything said here is held in a buffer and printed once, at the end: the console the host reads keeps only its last ~261 KB,
 * and bluetoothd's own log fills that in a few seconds -- lines said 20 s before the run's end (the state, the scan) were lost
 * in runs 108-112, leaving only "done". The delegate runs on a queue of this program's own, not the main one: a command-line tool
 * on iOS does not service the main dispatch queue the way an app does, and a scan's reports arrive as blocks on it.
 *
 * usage: bt_test [SECONDS [SERVICE-UUID]]   (a service to scan for; without it every advertiser)
 */
#import <CoreBluetooth/CoreBluetooth.h>
#import <Foundation/Foundation.h>
#import <IOKit/IOKitLib.h>
#include <dlfcn.h>
#include <objc/runtime.h>
#include <mach/mach.h>
#include <stdarg.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>
extern mach_port_t bootstrap_port;
extern kern_return_t bootstrap_look_up(mach_port_t, const char *, mach_port_t *);

static NSMutableString *logbuf;
static dispatch_queue_t btq;

static void
say(const char *format, ...)
{
	va_list ap;
	char line[1024];

	va_start(ap, format);
	vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	@synchronized (logbuf) {
		[logbuf appendFormat:@"%s\n", line];
	}
}

/* Check the name matches the legacy client may need, without opening a user client. */
static void
report_bt_services(const char *phase)
{
	const char *names[] = { "marconi-bt", "bluetooth" };

	for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		io_iterator_t matches = IO_OBJECT_NULL;
		kern_return_t kr = IOServiceGetMatchingServices(MACH_PORT_NULL, IOServiceNameMatching(names[i]), &matches);
		io_service_t service = kr == KERN_SUCCESS ? IOIteratorNext(matches) : IO_OBJECT_NULL;

		say("bt_test: IORegistry %s %s: kr=0x%x found=%d", phase, names[i], (unsigned)kr, service != IO_OBJECT_NULL);
		if (service != IO_OBJECT_NULL) IOObjectRelease(service);
		if (matches != IO_OBJECT_NULL) IOObjectRelease(matches);
	}
}

@interface Scanner : NSObject <CBCentralManagerDelegate>
@property (nonatomic) unsigned seen;
@property (nonatomic, copy) NSArray<CBUUID *> *services;
@property (nonatomic, strong) CBCentralManager *central;
@end

@implementation Scanner
- (void)centralManagerDidUpdateState:(CBCentralManager *)central
{
	say("bt_test: central manager state %ld (5 is powered on), authorization %ld",
	    (long)central.state, (long)CBManager.authorization);
	if (central.state == CBManagerStatePoweredOn) {
		[central scanForPeripheralsWithServices:self.services options:@{ CBCentralManagerScanOptionAllowDuplicatesKey : @YES }];
		say("bt_test: scanning (isScanning %d)", (int)central.isScanning);
	}
}

- (void)centralManager:(CBCentralManager *)central didDiscoverPeripheral:(CBPeripheral *)peripheral
     advertisementData:(NSDictionary<NSString *, id> *)advertisementData RSSI:(NSNumber *)RSSI
{
	unsigned n;

	@synchronized (self) {
		n = ++_seen;
	}
	if (n <= 40) {
		say("bt_test: #%u %s name=%s rssi=%d adv=%s", n, peripheral.identifier.UUIDString.UTF8String,
		    peripheral.name != nil ? peripheral.name.UTF8String : "-", RSSI.intValue,
		    [[advertisementData description] stringByReplacingOccurrencesOfString:@"\n" withString:@" "].UTF8String);
	}
}

- (void)centralManager:(CBCentralManager *)central didDisconnectPeripheral:(CBPeripheral *)peripheral error:(NSError *)error
{
	say("bt_test: disconnected %s (%s)", peripheral.identifier.UUIDString.UTF8String, error.localizedDescription.UTF8String);
}
@end

int
main(int argc, char **argv)
{
	@autoreleasepool {
		double seconds = argc > 1 ? atof(argv[1]) : 30;
		Scanner *scanner = [Scanner new];

		logbuf = [NSMutableString new];
		btq = dispatch_queue_create("com.redwood.bt_test.central", DISPATCH_QUEUE_SERIAL);
		if (argc > 2) {
			scanner.services = @[ [CBUUID UUIDWithString:[NSString stringWithUTF8String:argv[2]]] ];
			say("bt_test: for service %s", argv[2]);
		}
		{
			NSString *bundle = [[NSBundle mainBundle] bundleIdentifier] ?: @"(none)";

			say("bt_test: pid %d, bundle %s", (int)getpid(), bundle.UTF8String);
		}
		CBCentralManager *manager = [[CBCentralManager alloc] initWithDelegate:scanner queue:btq];
		scanner.central = manager;

		/* what MobileGestalt says (BluetoothManager asks "bluetooth" before it attaches: no, and Settings' switch stays grey) */
		{
			typedef bool (*bool_answer)(CFStringRef);
			bool_answer answer = (bool_answer)dlsym(RTLD_DEFAULT, "MGGetBoolAnswer");

			for (NSString *key in @[ @"bluetooth", @"BluetoothCapability", @"wifi", @"telephony" ]) {
				say("bt_test: MobileGestalt %s = %d", key.UTF8String, answer != NULL ? (int)answer((__bridge CFStringRef)key) : -1);
			}
		}
		/* the services of bluetoothd, as launchd gives them out */
		for (NSString *service in @[ @"com.apple.bluetooth.xpc", @"com.apple.server.bluetooth.general.xpc", @"com.apple.server.bluetooth", @"com.apple.server.bluetooth.classic.xpc" ]) {
			mach_port_t port = MACH_PORT_NULL;
			kern_return_t kr = bootstrap_look_up(bootstrap_port, service.UTF8String, &port);

			say("bt_test: %s: bootstrap_look_up %d", service.UTF8String, kr);
		}
		say("bt_test: authorization %ld, state %ld", (long)CBManager.authorization, (long)manager.state);
		/* what Settings uses: BluetoothManager (the old BTSession way to bluetoothd, "bluetooth" asked of MobileGestalt first) */
		void *handle = dlopen("/System/Library/PrivateFrameworks/BluetoothManager.framework/BluetoothManager", RTLD_NOW);
		Class bmclass = NSClassFromString(@"BluetoothManager");

		say("bt_test: dlopen %s, class %s", handle != NULL ? "ok" : dlerror(), bmclass != nil ? "found" : "missing");
		report_bt_services("before-attach");
		id bm = [bmclass performSelector:NSSelectorFromString(@"sharedInstance")];

		[[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:6]];
		report_bt_services("after-wait");
		if (bm != nil) {
			say("bt_test: BluetoothManager available %d powered %d powerState %ld",
			    (int)[bm performSelector:NSSelectorFromString(@"available")] & 1,
			    (int)[bm performSelector:NSSelectorFromString(@"powered")] & 1,
			    (long)[bm performSelector:NSSelectorFromString(@"powerState")]);
		}
		[[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:seconds]];
		say("bt_test: scan over: isScanning %d", (int)manager.isScanning);
		[manager stopScan];
		say("bt_test: done, %u devices seen", scanner.seen);

		/* one write, at the end: what the console keeps */
		NSString *all = nil;
		@synchronized (logbuf) {
			all = [logbuf copy];
		}
		printf("%s", all.UTF8String);
		fflush(stdout);
		/* also into host_keep line by line, so the report survives even when the console ring wraps
		 * (the HCI byte trace is silenced with bt_hci_trace=0 for these app tests). */
		for (NSString *line in [all componentsSeparatedByString:@"\n"]) {
			const char *utf8;

			if (line.length == 0) {
				continue;
			}
			utf8 = line.UTF8String;
			sysctlbyname("kern.redwood.host_keep", NULL, NULL, (void *)utf8, strlen(utf8));
		}
	}
	return 0;
}