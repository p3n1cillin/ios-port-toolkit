/*
 * avplay_test FILE [SECONDS]: play an audio file with AVAudioPlayer (the way ringtone previews and many apps do) and
 * say what each step answered. avplay_test --sys ID: AudioServicesPlaySystemSound(ID) instead.
 * avplay_test --find: list the ringtone files where iOS 16 keeps them.
 * (r5q: ringtone previews in Settings play nothing, Test 168.)
 */
#import <AVFoundation/AVFoundation.h>
#import <AudioToolbox/AudioToolbox.h>
#import <Foundation/Foundation.h>

int
main(int argc, char **argv)
{
	@autoreleasepool {
		NSError *error = nil;

		if (argc > 1 && strcmp(argv[1], "--find") == 0) {
			for (NSString *dir in @[ @"/Library/Ringtones", @"/System/Library/Audio/UISounds",
			    @"/System/Library/PrivateFrameworks/ToneLibrary.framework" ]) {
				NSArray *items = [[NSFileManager defaultManager] contentsOfDirectoryAtPath:dir error:&error];
				printf("%s: %lu items%s\n", dir.UTF8String, (unsigned long)items.count, items ? "" : " (cannot list)");
				for (NSUInteger i = 0; i < items.count && i < 8; i++) {
					printf("  %s\n", [items[i] UTF8String]);
				}
			}
			return 0;
		}
		if (argc > 2 && strcmp(argv[1], "--sys") == 0) {
			SystemSoundID sound = (SystemSoundID)atoi(argv[2]);
			dispatch_semaphore_t done = dispatch_semaphore_create(0);

			AudioServicesPlaySystemSoundWithCompletion(sound, ^{ dispatch_semaphore_signal(done); });
			long timed_out = dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC));
			printf("avplay_test: system sound %u %s\n", sound, timed_out ? "did not complete in 5 s" : "completed");
			return 0;
		}
		if (argc < 2) {
			printf("usage: avplay_test FILE [SECONDS] | --sys ID | --find\n");
			return 1;
		}
		AVAudioSession *session = [AVAudioSession sharedInstance];
		BOOL ok = [session setCategory:AVAudioSessionCategoryPlayback error:&error];
		printf("avplay_test: session category playback: %s %s\n", ok ? "yes" : "NO", error ? error.description.UTF8String : "");
		error = nil;
		ok = [session setActive:YES error:&error];
		printf("avplay_test: session active: %s %s\n", ok ? "yes" : "NO", error ? error.description.UTF8String : "");
		printf("avplay_test: route: %s\n", session.currentRoute.description.UTF8String);
		error = nil;
		AVAudioPlayer *player = [[AVAudioPlayer alloc] initWithContentsOfURL:[NSURL fileURLWithPath:@(argv[1])] error:&error];
		if (player == nil) {
			printf("avplay_test: no player: %s\n", error.description.UTF8String);
			return 1;
		}
		printf("avplay_test: duration %.2f s, %lu channels, format %s\n", player.duration, (unsigned long)player.numberOfChannels,
		    player.format.description.UTF8String);
		printf("avplay_test: prepareToPlay %s, play %s\n", [player prepareToPlay] ? "yes" : "NO", [player play] ? "yes" : "NO");
		double seconds = argc > 2 ? atof(argv[2]) : 3;
		[[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:seconds]];
		printf("avplay_test: after %.1f s: playing %s, at %.2f s\n", seconds, player.playing ? "yes" : "no", player.currentTime);
	}
	return 0;
}
