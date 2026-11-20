/*
 * heic_test [WIDTH HEIGHT] (2026-11-20): encode a generated image as HEIC through ImageIO (CGImageDestination,
 * "public.heic"), the way the photo pipeline does, and say what happens: the encoder's time, the bytes out, the
 * process's peak memory. The Camera app saves JPEG on this port because "HEIC's software encoder runs out of memory";
 * this is that path without the app. Default size 4000x3000 (a 12 MP photo); try 2000x1500 too.
 */
#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>
#include <mach/mach.h>
#include <sys/resource.h>
#include <time.h>

static double
now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

int
main(int argc, char **argv)
{
	@autoreleasepool {
		size_t w = argc > 2 ? (size_t)atoi(argv[1]) : 4000, h = argc > 2 ? (size_t)atoi(argv[2]) : 3000;
		CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
		CGContextRef ctx = CGBitmapContextCreate(NULL, w, h, 8, w * 4, cs, kCGImageAlphaNoneSkipLast);
		uint8_t *px = CGBitmapContextGetData(ctx);
		for (size_t y = 0; y < h; y++) {
			for (size_t x = 0; x < w; x++) {
				uint8_t *p = px + (y * w + x) * 4;
				p[0] = (uint8_t)(x * 255 / w);
				p[1] = (uint8_t)(y * 255 / h);
				p[2] = (uint8_t)((x ^ y) & 0xff);
			}
		}
		CGImageRef img = CGBitmapContextCreateImage(ctx);
		NSMutableData *out = [NSMutableData data];
		CGImageDestinationRef dst = CGImageDestinationCreateWithData((__bridge CFMutableDataRef)out,
		    CFSTR("public.heic"), 1, NULL);
		printf("heic_test: %zux%zu, destination %s\n", w, h, dst ? "made" : "NOT made (no HEIC encoder?)");
		if (dst == NULL) {
			return 1;
		}
		NSDictionary *opts = @{ (__bridge NSString *)kCGImageDestinationLossyCompressionQuality: @0.8 };
		CGImageDestinationAddImage(dst, img, (__bridge CFDictionaryRef)opts);
		double t0 = now();
		bool ok = CGImageDestinationFinalize(dst);
		double t1 = now();
		struct rusage ru;
		getrusage(RUSAGE_SELF, &ru);
		printf("heic_test: finalize %s in %.2f s, %lu bytes; peak resident %ld MB\n", ok ? "OK" : "FAILED", t1 - t0,
		    (unsigned long)out.length, ru.ru_maxrss / (1024 * 1024));
		CFRelease(dst);
		CGImageRelease(img);
		CGContextRelease(ctx);
		CGColorSpaceRelease(cs);
		return ok ? 0 : 2;
	}
}
