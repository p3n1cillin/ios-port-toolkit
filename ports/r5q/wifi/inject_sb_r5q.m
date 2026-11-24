/*
 * libr5q-sb.dylib: injected into SpringBoard (Test 443 / 444).
 *
 * 1. Control Center Connectivity buttons (CCUIRoundButton):
 *    normalStateBackgroundView is an untextured solid-white UIView using
 *    layer.filters = @[vibrantColorMatrix]. Under the GLES renderer that draws
 *    as opaque #FFFFFF white, obscuring the white Airplane Mode / Bluetooth
 *    glyphs when unselected. Strip the vibrantColorMatrix filter on that
 *    background view and set translucent white (alpha 0.16) to match iOS.
 *
 * 2. Top-right status bar Wi-Fi bars after cold boot:
 *    SpringBoard can start before en0 finishes associating at boot, leaving
 *    the status bar Wi-Fi icon stuck at 0 active bars (grey). Refresh
 *    SBWiFiManager after association settles so the status bar always shows
 *    real full-contrast signal bars without needing a respring.
 *
 * 3. Control Center Bluetooth button after cold boot (Test 444):
 *    SpringBoard can start before bluetoothd finishes bringing up the HCI
 *    transport, leaving BluetoothManager detached (available=0) and Control
 *    Center's Bluetooth button stuck slashed/off even though Settings and
 *    CoreBluetooth are ON. Retry -[BluetoothManager _attach] once bluetoothd
 *    is ready and refresh CCUIConnectivityBluetoothViewController on open.
 */
#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#import <QuartzCore/QuartzCore.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include <dlfcn.h>

static IMP orig_ccui_layout = NULL;
static Class g_ccui_cls = Nil;
static IMP orig_ccbt_appear = NULL;
static Class g_ccbt_cls = Nil;
static BOOL g_ccbt_hooked = NO;
static __weak id g_ccbt_vc = nil;

static void install_ccbt_hook(void);

static void
r5q_ccui_layout(UIView *self, SEL _cmd)
{
	if (!g_ccbt_hooked) {
		install_ccbt_hook();
	}
	if (orig_ccui_layout) {
		((void (*)(id, SEL))orig_ccui_layout)(self, _cmd);
	} else if (g_ccui_cls) {
		struct objc_super sup = { self, class_getSuperclass(g_ccui_cls) };
		((void (*)(struct objc_super *, SEL))objc_msgSendSuper)(&sup, _cmd);
	}
	for (UIView *sv in self.subviews) {
		if (![sv isMemberOfClass:[UIView class]]) {
			continue;
		}
		NSArray *filters = sv.layer.filters;
		if (filters != nil && filters.count > 0) {
			BOOL has_vcm = NO;
			for (id flt in filters) {
				if ([[flt description] containsString:@"vibrantColorMatrix"]) {
					has_vcm = YES;
					break;
				}
			}
			if (has_vcm) {
				sv.layer.filters = nil;
				sv.backgroundColor = [UIColor colorWithWhite:1.0 alpha:0.16];
			}
		}
	}
}

static void
refresh_wifi_bars(void)
{
	Class wcls = objc_getClass("SBWiFiManager");
	if (!wcls) {
		return;
	}
	SEL s_shared = sel_registerName("sharedInstance");
	if (![wcls respondsToSelector:s_shared]) {
		return;
	}
	id mgr = ((id (*)(Class, SEL))objc_msgSend)(wcls, s_shared);
	if (!mgr) {
		return;
	}
	const char *sels[] = { "_updateCurrentNetwork", "_updateSignalStrength", NULL };
	for (int i = 0; sels[i] != NULL; i++) {
		SEL s = sel_registerName(sels[i]);
		if ([mgr respondsToSelector:s]) {
			((void (*)(id, SEL))objc_msgSend)(mgr, s);
		}
	}
}

static void
refresh_bluetooth_state(BOOL allow_reattach)
{
	Class bmcls = objc_getClass("BluetoothManager");
	if (bmcls == Nil) {
		dlopen("/System/Library/PrivateFrameworks/BluetoothManager.framework/BluetoothManager", RTLD_LAZY);
		bmcls = objc_getClass("BluetoothManager");
	}
	if (bmcls != Nil) {
		SEL s_shared = sel_registerName("sharedInstance");
		if ([bmcls respondsToSelector:s_shared]) {
			id bm = ((id (*)(Class, SEL))objc_msgSend)(bmcls, s_shared);
			if (bm != nil) {
				BOOL avail = NO;
				SEL s_avail = sel_registerName("available");
				if ([bm respondsToSelector:s_avail]) {
					avail = ((BOOL (*)(id, SEL))objc_msgSend)(bm, s_avail);
				}
				if (!avail && allow_reattach) {
					SEL s_clean = sel_registerName("_cleanup:");
					if ([bm respondsToSelector:s_clean]) {
						((void (*)(id, SEL, BOOL))objc_msgSend)(bm, s_clean, NO);
					}
					SEL s_attach = sel_registerName("_attach");
					if ([bm respondsToSelector:s_attach]) {
						((void (*)(id, SEL))objc_msgSend)(bm, s_attach);
					}
				}
				SEL s_upd = sel_registerName("_updateBluetoothState");
				if ([bm respondsToSelector:s_upd]) {
					((void (*)(id, SEL))objc_msgSend)(bm, s_upd);
				}
			}
		}
	}
	id vc = g_ccbt_vc;
	if (vc != nil) {
		SEL s_upd = sel_registerName("_updateState");
		if ([vc respondsToSelector:s_upd]) {
			((void (*)(id, SEL))objc_msgSend)(vc, s_upd);
		}
	}
}

static void
r5q_ccbt_appear(id self, SEL _cmd, BOOL animated)
{
	g_ccbt_vc = self;
	if (orig_ccbt_appear) {
		((void (*)(id, SEL, BOOL))orig_ccbt_appear)(self, _cmd, animated);
	} else if (g_ccbt_cls) {
		struct objc_super sup = { self, class_getSuperclass(g_ccbt_cls) };
		((void (*)(struct objc_super *, SEL, BOOL))objc_msgSendSuper)(&sup, _cmd, animated);
	}
	SEL s_upd = sel_registerName("_updateState");
	if ([self respondsToSelector:s_upd]) {
		((void (*)(id, SEL))objc_msgSend)(self, s_upd);
	}
}

static void
install_ccbt_hook(void)
{
	if (g_ccbt_hooked) {
		return;
	}
	Class btcls = objc_getClass("CCUIConnectivityBluetoothViewController");
	if (btcls != Nil) {
		g_ccbt_cls = btcls;
		g_ccbt_hooked = YES;
		SEL s_app = sel_registerName("viewWillAppear:");
		Method m = class_getInstanceMethod(btcls, s_app);
		const char *types = m ? method_getTypeEncoding(m) : "v@:B";
		if (!class_addMethod(btcls, s_app, (IMP)r5q_ccbt_appear, types)) {
			orig_ccbt_appear = method_setImplementation(m, (IMP)r5q_ccbt_appear);
		}
	}
}

__attribute__((constructor))
static void
r5q_sb_init(void)
{
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
		Class cls = objc_getClass("CCUIRoundButton");
		if (cls != Nil) {
			g_ccui_cls = cls;
			Method m = class_getInstanceMethod(cls, @selector(layoutSubviews));
			const char *types = m ? method_getTypeEncoding(m) : "v@:";
			if (!class_addMethod(cls, @selector(layoutSubviews), (IMP)r5q_ccui_layout, types)) {
				orig_ccui_layout = method_setImplementation(m, (IMP)r5q_ccui_layout);
			}
		}
		install_ccbt_hook();
	});
	int wifi_delays[] = { 10, 20, 35 };
	for (int i = 0; i < 3; i++) {
		dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)wifi_delays[i] * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
			refresh_wifi_bars();
		});
	}
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 45 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
		refresh_bluetooth_state(YES);
	});
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 60 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
		refresh_bluetooth_state(NO);
	});
}
