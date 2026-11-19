/*
 * bt_test: what iOS's own Bluetooth layer says, from the phone (Bluetooth stage 4, Test 261 on).
 *
 *   bt_test               the state: BluetoothManager (the private framework Settings > Bluetooth uses) available /
 *                         enabled / powered / state, and CoreBluetooth's CBCentralManager state
 *   bt_test on|off        BluetoothManager setPowered: / setEnabled:
 *   bt_test scan [SECS]   BluetoothManager device discovery (classic + LE as Settings does): every device found
 *   bt_test le [SECS]     CoreBluetooth LE scan: every advertiser seen (name, RSSI)
 *
 * BluetoothManager is used through the Objective-C runtime only (no private headers): dlopen, NSClassFromString,
 * performSelector / NSInvocation. Its notifications are logged by name. Output may name nearby devices: it stays on
 * this machine (never in the repository's notes as is).
 * Built by pipeline/build_phone_tools.sh; put on the phone with `python phone.py tools`.
 */
#import <Foundation/Foundation.h>
#import <CoreBluetooth/CoreBluetooth.h>
#include <dlfcn.h>
#include <objc/message.h>
#include <objc/runtime.h>

static id
bm(void)
{
	static id shared;

	if (shared == nil) {
		if (dlopen("/System/Library/PrivateFrameworks/BluetoothManager.framework/BluetoothManager", RTLD_NOW) == NULL) {
			printf("BluetoothManager: dlopen failed: %s\n", dlerror());
			return nil;
		}
		Class c = NSClassFromString(@"BluetoothManager");
		shared = c ? ((id (*)(id, SEL))objc_msgSend)(c, NSSelectorFromString(@"sharedInstance")) : nil;
		printf("BluetoothManager: class %s, sharedInstance %s\n", c ? "found" : "NOT FOUND", shared ? "made" : "nil");
	}
	return shared;
}

static long
call_long(id o, NSString *sel)
{
	SEL s = NSSelectorFromString(sel);

	if (o == nil || ![o respondsToSelector:s]) {
		return -99;
	}
	return ((long (*)(id, SEL))objc_msgSend)(o, s);
}

static void
call_bool_arg(id o, NSString *sel, BOOL v)
{
	SEL s = NSSelectorFromString(sel);

	if (o != nil && [o respondsToSelector:s]) {
		((void (*)(id, SEL, BOOL))objc_msgSend)(o, s, v);
		printf("BluetoothManager %s%s\n", sel.UTF8String, v ? "YES" : "NO");
	} else {
		printf("BluetoothManager has no %s\n", sel.UTF8String);
	}
}

static void
run(double secs)
{
	[[NSRunLoop mainRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:secs]];
}

static void
show(void)
{
	id m = bm();

	printf("BluetoothManager: %s; available %ld, enabled %ld, powered %ld, state %ld, deviceScanningEnabled %ld, "
	    "connected %ld\n", m ? "found" : "MISSING", call_long(m, @"available"), call_long(m, @"enabled"),
	    call_long(m, @"powered"), call_long(m, @"bluetoothState"), call_long(m, @"deviceScanningEnabled"),
	    call_long(m, @"connected"));
	if (m != nil && [m respondsToSelector:NSSelectorFromString(@"localAddress")]) {
		printf("  local address %s\n", [[m valueForKey:@"localAddress"] description].UTF8String);
	}
}

@interface Central : NSObject <CBCentralManagerDelegate>
@property (strong) CBCentralManager *cm;
@property BOOL scan;
@property (strong) NSMutableSet *seen;
@end

@implementation Central
- (void)centralManagerDidUpdateState:(CBCentralManager *)c
{
	printf("CoreBluetooth: state %ld (5 = powered on, 4 = off, 2 = unsupported, 3 = unauthorized)\n", (long)c.state);
	if (self.scan && c.state == CBManagerStatePoweredOn) {
		[c scanForPeripheralsWithServices:nil options:@{CBCentralManagerScanOptionAllowDuplicatesKey: @NO}];
		printf("CoreBluetooth: scanning\n");
	}
}
- (void)centralManager:(CBCentralManager *)c didDiscoverPeripheral:(CBPeripheral *)p advertisementData:(NSDictionary *)ad
    RSSI:(NSNumber *)rssi
{
	if (![self.seen containsObject:p.identifier]) {
		[self.seen addObject:p.identifier];
		NSString *name = p.name ?: ad[CBAdvertisementDataLocalNameKey] ?: @"-";

		printf("  LE %s rssi %d name %s\n", p.identifier.UUIDString.UTF8String, rssi.intValue, name.UTF8String);
	}
}
@end

int
main(int argc, char **argv)
{
	@autoreleasepool {
		NSString *cmd = argc > 1 ? @(argv[1]) : @"state";
		double secs = argc > 2 ? atof(argv[2]) : 15;

		setvbuf(stdout, NULL, _IOLBF, 0);
		for (NSString *name in @[@"BluetoothAvailabilityChangedNotification", @"BluetoothPowerChangedNotification",
		    @"BluetoothStateChangedNotification", @"BluetoothDeviceDiscoveredNotification",
		    @"BluetoothDeviceRemovedNotification", @"BluetoothDiscoveryStateChangedNotification"]) {
			[[NSNotificationCenter defaultCenter] addObserverForName:name object:nil queue:nil
			    usingBlock:^(NSNotification *n) {
				id o = n.object;
				NSString *what = [o respondsToSelector:NSSelectorFromString(@"name")] ?
				    [NSString stringWithFormat:@"%@ %@", [o valueForKey:@"name"],
				    [o respondsToSelector:NSSelectorFromString(@"address")] ? [o valueForKey:@"address"] : @""] :
				    [o description];
				printf("notification %s: %s\n", name.UTF8String, what.UTF8String);
			}];
		}
		Central *central = [Central new];
		central.seen = [NSMutableSet set];
		bm();
		run(2);                 /* (BluetoothManager attaches to bluetoothd asynchronously) */
		show();
		if ([cmd isEqualToString:@"on"] || [cmd isEqualToString:@"off"]) {
			BOOL on = [cmd isEqualToString:@"on"];
			call_bool_arg(bm(), @"setEnabled:", on);
			call_bool_arg(bm(), @"setPowered:", on);
			run(3);
			show();
		} else if ([cmd isEqualToString:@"methods"]) {
			/* (the gate in front of sharedInstance: BluetoothManager's own methods, class and instance) */
			Class c = NSClassFromString(@"BluetoothManager");
			for (int meta = 1; meta >= 0 && c != nil; meta--) {
				unsigned n = 0;
				Method *ms = class_copyMethodList(meta ? object_getClass(c) : c, &n);
				for (unsigned i = 0; i < n; i++) {
					printf("%c%s\n", meta ? '+' : '-', sel_getName(method_getName(ms[i])));
				}
				free(ms);
			}
			id direct = c ? [[c alloc] init] : nil;
			printf("[[BluetoothManager alloc] init] -> %s; +lastInitError %ld\n", direct ? "an object" : "nil",
			    call_long(c, @"lastInitError"));
		} else if ([cmd isEqualToString:@"scan"]) {
			call_bool_arg(bm(), @"setDeviceScanningEnabled:", YES);
			run(secs);
			call_bool_arg(bm(), @"setDeviceScanningEnabled:", NO);
		} else if ([cmd isEqualToString:@"le"]) {
			central.scan = YES;
		}
		central.cm = [[CBCentralManager alloc] initWithDelegate:central queue:nil];
		run([cmd isEqualToString:@"le"] ? secs : 2);
		printf("done (%lu LE advertisers)\n", (unsigned long)central.seen.count);
	}
	return 0;
}
