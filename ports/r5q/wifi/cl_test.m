/*
 * cl_test [SECONDS] [--enable]: what locationd answers (r5q, Test 197: locationd run from /var). Location services on?
 * this tool's authorization? then a location asked for, SECONDS long (default 20): every fix and error said.
 * --enable: +[CLLocationManager setLocationServicesEnabled:YES] first (private; needs locationd's trust).
 * --heading: (Test 237) compass headings instead of locations (the Compass app's source: locationd's CLCompass).
 */
#import <CoreLocation/CoreLocation.h>
#import <Foundation/Foundation.h>

@interface CLLocationManager (r5q)
+ (void)setLocationServicesEnabled:(BOOL)enabled;
+ (void)setAuthorizationStatusByType:(int)status forBundleIdentifier:(NSString *)bundle;
+ (void)setAuthorizationStatus:(BOOL)status forBundleIdentifier:(NSString *)bundle;
@end

@interface Watcher : NSObject <CLLocationManagerDelegate>
@end

@implementation Watcher
- (void)locationManager:(CLLocationManager *)m didUpdateLocations:(NSArray<CLLocation *> *)locations
{
	for (CLLocation *l in locations) {
		printf("cl_test: fix %.5f, %.5f +- %.0f m (age %.0f s)\n", l.coordinate.latitude, l.coordinate.longitude,
		    l.horizontalAccuracy, -l.timestamp.timeIntervalSinceNow);
	}
}
- (void)locationManager:(CLLocationManager *)m didUpdateHeading:(CLHeading *)h
{
	static unsigned n;

	if (n++ < 5 || n % 10 == 0) {
		printf("cl_test: heading %u: magnetic %.1f, true %.1f, accuracy %.1f, raw %.1f %.1f %.1f uT\n", n, h.magneticHeading,
		    h.trueHeading, h.headingAccuracy, h.x, h.y, h.z);
	}
}
- (BOOL)locationManagerShouldDisplayHeadingCalibration:(CLLocationManager *)m
{
	printf("cl_test: locationd asks for a compass calibration\n");
	return NO;
}
- (void)locationManager:(CLLocationManager *)m didFailWithError:(NSError *)error
{
	printf("cl_test: error %s\n", error.description.UTF8String);
}
- (void)locationManagerDidChangeAuthorization:(CLLocationManager *)m
{
	printf("cl_test: authorization now %d\n", (int)m.authorizationStatus);
}
@end

int
main(int argc, char **argv)
{
	@autoreleasepool {
		double seconds = argc > 1 && argv[1][0] != '-' ? atof(argv[1]) : 20;
		bool heading = false;
		for (int i = 1; i < argc; i++) {
			if (strcmp(argv[i], "--enable") == 0) {
				[CLLocationManager setLocationServicesEnabled:YES];
				printf("cl_test: asked for location services on\n");
			}
			if (strcmp(argv[i], "--heading") == 0) {
				heading = true;
			}
		}
		printf("cl_test: location services %s\n", [CLLocationManager locationServicesEnabled] ? "ON" : "off");
		Watcher *w = [Watcher new];
		CLLocationManager *m = [CLLocationManager new];
		m.delegate = w;
		m.desiredAccuracy = kCLLocationAccuracyBest;
		printf("cl_test: authorization %d, accuracy authorization %d\n", (int)m.authorizationStatus, (int)m.accuracyAuthorization);
		if (heading) {
			printf("cl_test: heading available %d\n", [CLLocationManager headingAvailable]);
			m.headingFilter = kCLHeadingFilterNone;
			[m startUpdatingHeading];
		} else {
			[m requestWhenInUseAuthorization];
			[m startUpdatingLocation];
		}
		[[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:seconds]];
		CLLocation *l = m.location;
		printf("cl_test: last known: %s\n", l ? l.description.UTF8String : "none");
	}
	return 0;
}
