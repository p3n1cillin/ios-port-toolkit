/*
 * cm_test [SECONDS]: what an application gets from CoreMotion (CMMotionManager): accelerometer, gyroscope, magnetometer,
 * device motion -- availability, then samples for SECONDS (default 5). Over r5qd: do the phone's sensors reach apps?
 */
#import <CoreMotion/CoreMotion.h>
#import <Foundation/Foundation.h>

int
main(int argc, char **argv)
{
	/*
	 * (Test 242) "log" as the third argument: CoreMotion's own messages, info and debug too, on standard error
	 * (OS_ACTIVITY_MODE=debug, OS_ACTIVITY_DT_MODE: read when the process starts, so the program starts itself again)
	 */
	if (argc > 3 && strcmp(argv[3], "log") == 0 && getenv("OS_ACTIVITY_DT_MODE") == NULL) {
		setenv("OS_ACTIVITY_MODE", "debug", 1);
		setenv("OS_ACTIVITY_DT_MODE", "1", 1);
		execv(argv[0], argv);
	}
	if (argc > 1 && strcmp(argv[1], "peek") == 0) {
		/* (Test 242) cm_test peek ADDR...: 32-bit words of the shared cache at unslid addresses (are our patches there?) */
		extern const void *_dyld_get_shared_cache_range(size_t *length);
		size_t length = 0;
		uintptr_t start = (uintptr_t)_dyld_get_shared_cache_range(&length);

		for (int i = 2; i < argc; i++) {
			uintptr_t at = start + (uintptr_t)(strtoull(argv[i], NULL, 16) - 0x180000000ull);

			printf("cm_test: %s: %08x\n", argv[i], *(volatile uint32_t *)at);
		}
		return 0;
	}
	@autoreleasepool {
		double seconds = argc > 1 ? atof(argv[1]) : 5;
		CMMotionManager *m = [CMMotionManager new];
		NSOperationQueue *q = [NSOperationQueue new];
		__block unsigned a = 0, g = 0, mg = 0, dm = 0;

		printf("cm_test: available: accelerometer %d, gyro %d, magnetometer %d, device motion %d\n", m.accelerometerAvailable,
		    m.gyroAvailable, m.magnetometerAvailable, m.deviceMotionAvailable);
		m.accelerometerUpdateInterval = m.gyroUpdateInterval = m.magnetometerUpdateInterval = m.deviceMotionUpdateInterval = 0.1;
		[m startAccelerometerUpdatesToQueue:q withHandler:^(CMAccelerometerData *d, NSError *e) {
			if (a++ < 3 && d) printf("cm_test: accelerometer %.3f %.3f %.3f g\n", d.acceleration.x, d.acceleration.y, d.acceleration.z);
		}];
		[m startGyroUpdatesToQueue:q withHandler:^(CMGyroData *d, NSError *e) {
			if (g++ < 3 && d) printf("cm_test: gyro %.3f %.3f %.3f rad/s\n", d.rotationRate.x, d.rotationRate.y, d.rotationRate.z);
		}];
		[m startMagnetometerUpdatesToQueue:q withHandler:^(CMMagnetometerData *d, NSError *e) {
			if (mg++ < 3 && d) printf("cm_test: magnetometer %.1f %.1f %.1f uT\n", d.magneticField.x, d.magneticField.y, d.magneticField.z);
		}];
		/* (Test 233) "north" as the second argument: the magnetic-north reference frame (heading from the compass) */
		CMAttitudeReferenceFrame frame = argc > 2 && strcmp(argv[2], "north") == 0 ? CMAttitudeReferenceFrameXMagneticNorthZVertical :
		    CMAttitudeReferenceFrameXArbitraryZVertical;
		printf("cm_test: device motion frames available 0x%lx, using 0x%lx\n",
		    (unsigned long)[CMMotionManager availableAttitudeReferenceFrames], (unsigned long)frame);
		[m startDeviceMotionUpdatesUsingReferenceFrame:frame toQueue:q withHandler:^(CMDeviceMotion *d, NSError *e) {
			if ((dm++ < 3 || dm % 25 == 0) && d) {
				printf("cm_test: device motion gravity %.3f %.3f %.3f, user accel %.3f %.3f %.3f, roll %.1f pitch %.1f yaw %.1f deg, "
				    "heading %.1f\n", d.gravity.x, d.gravity.y, d.gravity.z, d.userAcceleration.x, d.userAcceleration.y,
				    d.userAcceleration.z, d.attitude.roll * 57.29578, d.attitude.pitch * 57.29578, d.attitude.yaw * 57.29578, d.heading);
			}
			if (frame != CMAttitudeReferenceFrameXArbitraryZVertical && d && (dm < 3 || dm % 25 == 0)) {
				/* (Test 241) the calibrated field and how sure CoreMotion is of it (-1 uncalibrated .. 2 high) */
				printf("cm_test: calibrated field %.1f %.1f %.1f uT (|%.1f|), accuracy %d\n", d.magneticField.field.x,
				    d.magneticField.field.y, d.magneticField.field.z, sqrt(d.magneticField.field.x * d.magneticField.field.x +
				    d.magneticField.field.y * d.magneticField.field.y + d.magneticField.field.z * d.magneticField.field.z),
				    (int)d.magneticField.accuracy);
			}
			if (e && dm < 3) printf("cm_test: device motion error %s\n", e.description.UTF8String);
		}];
		[[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:seconds]];
		printf("cm_test: in %.0f s: accelerometer %u, gyro %u, magnetometer %u, device motion %u\n", seconds, a, g, mg, dm);
	}
	return 0;
}
