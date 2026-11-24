/*
 * vt_enc_probe.c: Benchmark VCPHEVC speed presets via com.apple.VideoProcessing preferences on-phone.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>
#include <dlfcn.h>
#include <mach/mach_time.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern OSStatus VTRegisterVideoEncoderWithInfo(CMVideoCodecType codecType, CFDictionaryRef encoderInfo, void *createFunc);
extern const CFStringRef kVTVideoEncoderList_EncoderID;
extern const CFStringRef kVTVideoEncoderList_EncoderName;
extern const CFStringRef kVTVideoEncoderList_IsHardwareAccelerated;

static double
now_ms(void)
{
	static mach_timebase_info_data_t tb;
	if (tb.denom == 0) mach_timebase_info(&tb);
	return (double)mach_absolute_time() * tb.numer / tb.denom / 1e6;
}

struct enc_ctx {
	int out_count;
	size_t bytes;
};

static void
enc_cb(void *outputCallbackRefCon, void *sourceFrameRefCon, OSStatus status, VTEncodeInfoFlags infoFlags, CMSampleBufferRef sampleBuffer)
{
	struct enc_ctx *ctx = outputCallbackRefCon;
	(void)sourceFrameRefCon;
	(void)infoFlags;
	if (status == 0 && sampleBuffer != NULL) {
		CMBlockBufferRef bb = CMSampleBufferGetDataBuffer(sampleBuffer);
		if (bb) ctx->bytes += CMBlockBufferGetDataLength(bb);
		ctx->out_count++;
	}
}

static void
set_pref(const char *k, const char *v)
{
	CFStringRef ks = CFStringCreateWithCString(NULL, k, kCFStringEncodingUTF8);
	CFStringRef vs = v ? CFStringCreateWithCString(NULL, v, kCFStringEncodingUTF8) : NULL;
	CFPreferencesSetAppValue(ks, vs, CFSTR("com.apple.VideoProcessing"));
	CFRelease(ks);
	if (vs) CFRelease(vs);
}

static void
clear_prefs(void)
{
	static const char *const keys[] = {
		"threading", "max-threads", "wpp-threads", "global-threadpool", "u",
		"sao", "db", "rdoq", "fast-merge", "fasttu", "qpel-bilinear", "fastprecomp",
		"fastbipred", "me-mode", "subpel-mode", "max-intra-cu-size", "max-inter-cu-size",
		"bipred", "forcedqualitylevel", NULL
	};
	for (int i = 0; keys[i]; i++) set_pref(keys[i], NULL);
	CFPreferencesAppSynchronize(CFSTR("com.apple.VideoProcessing"));
}

static void
bench(int w, int h, int frames, const char *label, int realtime, int usage)
{
	struct enc_ctx ctx = { 0, 0 };
	VTCompressionSessionRef sess = NULL;
	OSStatus err = VTCompressionSessionCreate(NULL, w, h, 'hvc1', NULL, NULL, NULL, enc_cb, &ctx, &sess);
	if (err != 0 || sess == NULL) {
		printf("BENCH %-30s %dx%d: create failed %d\n", label, w, h, (int)err);
		return;
	}
	if (usage >= 0) {
		int u = usage;
		CFNumberRef n = CFNumberCreate(NULL, kCFNumberIntType, &u);
		VTSessionSetProperty(sess, CFSTR("Usage"), n);
		CFRelease(n);
	}
	if (realtime >= 0) {
		VTSessionSetProperty(sess, kVTCompressionPropertyKey_RealTime, realtime ? kCFBooleanTrue : kCFBooleanFalse);
	}
	VTSessionSetProperty(sess, kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse);
	int fps = 30, kf = 30;
	CFNumberRef nfps = CFNumberCreate(NULL, kCFNumberIntType, &fps);
	CFNumberRef nkf = CFNumberCreate(NULL, kCFNumberIntType, &kf);
	VTSessionSetProperty(sess, kVTCompressionPropertyKey_ExpectedFrameRate, nfps);
	VTSessionSetProperty(sess, kVTCompressionPropertyKey_MaxKeyFrameInterval, nkf);
	CFRelease(nfps);
	CFRelease(nkf);
	VTCompressionSessionPrepareToEncodeFrames(sess);

	CVPixelBufferRef pb = NULL;
	if (CVPixelBufferCreate(NULL, w, h, kCVPixelFormatType_420YpCbCr8BiPlanarFullRange, NULL, &pb) != 0 || pb == NULL) {
		/* (review) a NULL buffer crashed the probe below, leaving its VideoProcessing prefs set for the system */
		printf("BENCH %-30s %dx%d: no pixel buffer\n", label, w, h);
		VTCompressionSessionInvalidate(sess);
		CFRelease(sess);
		return;
	}
	CVPixelBufferLockBaseAddress(pb, 0);
	uint8_t *py = CVPixelBufferGetBaseAddressOfPlane(pb, 0);
	uint8_t *puv = CVPixelBufferGetBaseAddressOfPlane(pb, 1);
	size_t sy = CVPixelBufferGetBytesPerRowOfPlane(pb, 0);
	size_t suv = CVPixelBufferGetBytesPerRowOfPlane(pb, 1);
	for (int r = 0; r < h; r++) {
		for (int x = 0; x < w; x++) py[(size_t)r * sy + x] = (uint8_t)((x * 3 + r * 5) & 0xff);
		if ((r & 1) == 0) memset(puv + (size_t)(r / 2) * suv, 128, (size_t)w);
	}
	CVPixelBufferUnlockBaseAddress(pb, 0);

	double t0 = now_ms();
	for (int i = 0; i < frames; i++) {
		/* touch first row lightly so frames aren't trivial constants */
		CVPixelBufferLockBaseAddress(pb, 0);
		py = CVPixelBufferGetBaseAddressOfPlane(pb, 0);
		memset(py, (uint8_t)(i * 19), (size_t)w);
		CVPixelBufferUnlockBaseAddress(pb, 0);
		CMTime pts = CMTimeMake(i, 30);
		CMTime dur = CMTimeMake(1, 30);
		err = VTCompressionSessionEncodeFrame(sess, pb, pts, dur, NULL, NULL, NULL);
		if (err != 0) {
			printf("BENCH %-30s %dx%d: encode err=%d\n", label, w, h, (int)err);
			break;
		}
	}
	VTCompressionSessionCompleteFrames(sess, kCMTimeInvalid);
	double dt = now_ms() - t0;
	CVPixelBufferRelease(pb);
	VTCompressionSessionInvalidate(sess);
	CFRelease(sess);
	printf("BENCH %-30s %dx%d: %d/%d frames (%zu KB) in %6.1f ms = %5.1f ms/f (%5.2f fps)\n",
	    label, w, h, ctx.out_count, frames, ctx.bytes / 1024, dt, dt / frames, frames * 1000.0 / dt);
}

int
main(void)
{
	void *codec_hevc = dlopen("/System/Library/VideoCodecs/VCPHEVC.videocodec", RTLD_NOW | RTLD_LOCAL);
	void *create_hevc = codec_hevc ? dlsym(codec_hevc, "HEVCVideoEncoder_CreateInstance") : NULL;
	if (!create_hevc) { printf("no HEVCVideoEncoder_CreateInstance\n"); return 1; }
	const void *keys[] = { kVTVideoEncoderList_EncoderID, kVTVideoEncoderList_EncoderName, kVTVideoEncoderList_IsHardwareAccelerated };
	const void *vals[] = { CFSTR("com.apple.videotoolbox.videoencoder.hevc.vcp"), CFSTR("HEVC (VCPHEVC, software)"), kCFBooleanFalse };
	CFDictionaryRef info = CFDictionaryCreate(NULL, keys, vals, 3, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	VTRegisterVideoEncoderWithInfo('hvc1', info, create_hevc);
	CFRelease(info);

	clear_prefs();
	set_pref("threading", "auto");
	set_pref("max-threads", "7");
	CFPreferencesAppSynchronize(CFSTR("com.apple.VideoProcessing"));
	bench(1280, 720, 20, "threads7-default-rt1", 1, -1);
	bench(1920, 1080, 10, "threads7-default-rt1", 1, -1);

	set_pref("u", "ernie_faster");
	CFPreferencesAppSynchronize(CFSTR("com.apple.VideoProcessing"));
	bench(1280, 720, 20, "threads7-ernie_faster", 1, -1);
	bench(1920, 1080, 10, "threads7-ernie_faster", 1, -1);

	set_pref("u", "ernie_debug_fast");
	CFPreferencesAppSynchronize(CFSTR("com.apple.VideoProcessing"));
	bench(1280, 720, 20, "threads7-ernie_debug_fast", 1, -1);
	bench(1920, 1080, 10, "threads7-ernie_debug_fast", 1, -1);

	set_pref("u", "facetime");
	CFPreferencesAppSynchronize(CFSTR("com.apple.VideoProcessing"));
	bench(1280, 720, 20, "threads7-facetime", 1, -1);

	clear_prefs();
	set_pref("threading", "auto");
	set_pref("max-threads", "7");
	set_pref("wpp-threads", "4");
	set_pref("sao", "0");
	set_pref("db", "0");
	set_pref("rdoq", "0");
	set_pref("fast-merge", "1");
	set_pref("fasttu", "1");
	set_pref("qpel-bilinear", "1");
	set_pref("fastprecomp", "1");
	set_pref("fastbipred", "1");
	set_pref("bipred", "0");
	set_pref("me-mode", "fast-star");
	set_pref("subpel-mode", "diamond-bilinear");
	CFPreferencesAppSynchronize(CFSTR("com.apple.VideoProcessing"));
	bench(1280, 720, 30, "threads7-custom-fast", 1, -1);
	bench(1920, 1080, 15, "threads7-custom-fast", 1, -1);

	clear_prefs();
	return 0;
}
