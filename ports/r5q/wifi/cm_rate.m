/*
 * cm_rate SENSOR INTERVAL SECONDS: how many samples one CoreMotion sensor alone delivers to an app (Test 243: apps got
 * a fraction of what the virtual services send). SENSOR: a (accelerometer), g (gyroscope), m (magnetometer);
 * (2026-11-21) d (device motion), n (device motion in the magnetic-north frame, as the Compass app: its heading and
 * accuracy printed every 25th sample).
 */
#import <CoreMotion/CoreMotion.h>
#import <Foundation/Foundation.h>

int
main(int argc, char **argv)
{
	@autoreleasepool {
		char s = argc > 1 ? argv[1][0] : 'a';
		double interval = argc > 2 ? atof(argv[2]) : 0.04, seconds = argc > 3 ? atof(argv[3]) : 5;
		CMMotionManager *m = [CMMotionManager new];
		NSOperationQueue *q = [NSOperationQueue new];
		__block unsigned n = 0;
		__block double first = 0, last = 0;
		void (^tick)(NSTimeInterval) = ^(NSTimeInterval t) {
			if (n++ == 0) {
				first = t;
			}
			last = t;
		};

		if (s == 'a') {
			m.accelerometerUpdateInterval = interval;
			[m startAccelerometerUpdatesToQueue:q withHandler:^(CMAccelerometerData *d, NSError *e) { if (d) tick(d.timestamp); }];
		} else if (s == 'g') {
			m.gyroUpdateInterval = interval;
			[m startGyroUpdatesToQueue:q withHandler:^(CMGyroData *d, NSError *e) { if (d) tick(d.timestamp); }];
		} else if (s == 'd' || s == 'n') {
			m.deviceMotionUpdateInterval = interval;
			[m startDeviceMotionUpdatesUsingReferenceFrame:s == 'n' ? CMAttitudeReferenceFrameXMagneticNorthZVertical :
			    CMAttitudeReferenceFrameXArbitraryZVertical toQueue:q withHandler:^(CMDeviceMotion *d, NSError *e) {
				if (d) {
					if (n % 25 == 0) {
						printf("cm_rate: %u: heading %.1f, field accuracy %d, yaw %.1f\n", n, d.heading,
						    (int)d.magneticField.accuracy, d.attitude.yaw * 180 / M_PI);
					}
					tick(d.timestamp);
				}
			}];
		} else {
			m.magnetometerUpdateInterval = interval;
			[m startMagnetometerUpdatesToQueue:q withHandler:^(CMMagnetometerData *d, NSError *e) { if (d) tick(d.timestamp); }];
		}
		[[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:seconds]];
		printf("cm_rate: %c at %.3f s for %.0f s: %u samples (%.1f/s), sample times span %.2f s\n", s, interval, seconds, n,
		    n / seconds, last - first);
	}
	return 0;
}
