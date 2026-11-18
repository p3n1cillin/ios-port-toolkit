/*
 * vt_test FILE [SECONDS]: can this iOS decode video? (r5q: video playback was never tried.)
 *   1. VideoToolbox: hardware decode supported for H.264 / HEVC?
 *   2. AVAssetReader: every frame of FILE's video track decoded to BGRA; frames, size, time, errors.
 *   3. AVPlayer + AVPlayerItemVideoOutput: FILE played for SECONDS (default 3); item status, frames handed out.
 */
#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <Foundation/Foundation.h>
#import <VideoToolbox/VideoToolbox.h>

static double
now(void)
{
	return [NSDate timeIntervalSinceReferenceDate];
}

int
main(int argc, char **argv)
{
	@autoreleasepool {
		if (argc < 2) {
			printf("usage: vt_test FILE [SECONDS]\n");
			return 1;
		}
		double seconds = argc > 2 ? atof(argv[2]) : 3;
		NSURL *url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:argv[1]]];
		NSError *error = nil;

		printf("vt_test: hardware decode: H.264 %s, HEVC %s\n", VTIsHardwareDecodeSupported(kCMVideoCodecType_H264) ? "yes" : "no",
		    VTIsHardwareDecodeSupported(kCMVideoCodecType_HEVC) ? "yes" : "no");

		AVURLAsset *asset = [AVURLAsset URLAssetWithURL:url options:nil];
		NSArray<AVAssetTrack *> *tracks = [asset tracksWithMediaType:AVMediaTypeVideo];
		printf("vt_test: %s: %lu video tracks, %.2f s\n", argv[1], (unsigned long)tracks.count, CMTimeGetSeconds(asset.duration));
		if (tracks.count == 0) {
			return 1;
		}
		AVAssetTrack *track = tracks[0];
		CMFormatDescriptionRef fd = (__bridge CMFormatDescriptionRef)track.formatDescriptions.firstObject;
		FourCharCode codec = fd ? CMFormatDescriptionGetMediaSubType(fd) : 0;
		printf("vt_test: track: codec '%c%c%c%c', %.0fx%.0f, %.1f fps\n", (char)(codec >> 24), (char)(codec >> 16), (char)(codec >> 8),
		    (char)codec, track.naturalSize.width, track.naturalSize.height, track.nominalFrameRate);

		AVAssetReader *reader = [AVAssetReader assetReaderWithAsset:asset error:&error];
		AVAssetReaderTrackOutput *out = [AVAssetReaderTrackOutput assetReaderTrackOutputWithTrack:track
		    outputSettings:@{ (id)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA) }];
		if (reader == nil || ![reader canAddOutput:out]) {
			printf("vt_test: reader: %s\n", error.description.UTF8String ?: "cannot add the output");
		} else {
			[reader addOutput:out];
			double t0 = now();
			BOOL started = [reader startReading];
			unsigned frames = 0, w = 0, h = 0, nonblack = 0;

			while (started) {
				CMSampleBufferRef sb = [out copyNextSampleBuffer];
				if (sb == NULL) {
					break;
				}
				CVImageBufferRef ib = CMSampleBufferGetImageBuffer(sb);
				if (ib) {
					w = (unsigned)CVPixelBufferGetWidth(ib);
					h = (unsigned)CVPixelBufferGetHeight(ib);
					if (frames % 10 == 0 && CVPixelBufferLockBaseAddress(ib, kCVPixelBufferLock_ReadOnly) == 0) {
						const uint8_t *p = CVPixelBufferGetBaseAddress(ib);
						size_t stride = CVPixelBufferGetBytesPerRow(ib);
						unsigned lit = 0;
						for (unsigned y = 0; p && y < h; y += h / 8 + 1) {
							for (unsigned x = 0; x < w; x += w / 8 + 1) {
								const uint8_t *q = p + y * stride + 4 * x;
								lit += (q[0] + q[1] + q[2]) > 30;
							}
						}
						nonblack += lit > 0;
						CVPixelBufferUnlockBaseAddress(ib, kCVPixelBufferLock_ReadOnly);
					}
					frames++;
				}
				CFRelease(sb);
			}
			double dt = now() - t0;
			printf("vt_test: reader: %s; %u frames of %ux%u in %.2f s (%.1f fps); sampled frames with picture: %u; status %ld %s\n",
			    started ? "started" : "NOT STARTED", frames, w, h, dt, dt > 0 ? frames / dt : 0, nonblack, (long)reader.status,
			    reader.error.description.UTF8String ?: "");
		}

		AVPlayerItem *item = [AVPlayerItem playerItemWithAsset:asset];
		AVPlayerItemVideoOutput *vout = [[AVPlayerItemVideoOutput alloc] initWithPixelBufferAttributes:
		    @{ (id)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA) }];
		[item addOutput:vout];
		AVPlayer *player = [AVPlayer playerWithPlayerItem:item];
		player.muted = YES;
		[player play];
		double t0 = now();
		unsigned got = 0;
		CMTime last = kCMTimeInvalid;
		while (now() - t0 < seconds) {
			[[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
			CMTime t = [vout itemTimeForHostTime:CACurrentMediaTime()];
			if ([vout hasNewPixelBufferForItemTime:t]) {
				CVPixelBufferRef pb = [vout copyPixelBufferForItemTime:t itemTimeForDisplay:NULL];
				if (pb) {
					got++;
					CVPixelBufferRelease(pb);
				}
			}
			last = item.currentTime;
		}
		printf("vt_test: player: item status %ld (%s), rate %.2f, at %.2f s, %u frames handed out in %.1f s\n", (long)item.status,
		    item.error.description.UTF8String ?: "no error", player.rate, CMTimeGetSeconds(last), got, seconds);
		for (AVPlayerItemErrorLogEvent *ev in item.errorLog.events) {
			printf("vt_test: player error log: %ld %s %s\n", (long)ev.errorStatusCode, ev.errorDomain.UTF8String ?: "",
			    ev.errorComment.UTF8String ?: "");
		}
	}
	return 0;
}
