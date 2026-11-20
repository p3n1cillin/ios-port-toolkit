# Memory on the S10 Lite iOS port

The phone has 6 GB. iOS on it ran at **13% memory free** (kern.memorystatus_level), and jetsam killed processes for
memory. The visible case: mediaserverd was killed (ReportMemoryException) while encoding a 12 MP HEIC photo, with
no per-process limit set. This page records where the memory went and what was done about it (2026-11-20).

## Tools (phone side, `pipeline/build_phone_tools.sh`)

| tool | what it shows |
|---|---|
| `memq [N]` | memsize, memorystatus level, VM page counts (free/active/inactive/wired/compressed/file/anon), the mounted file systems' sizes, the N biggest process footprints |
| `wiredq [MIN_MB]` | the kernel's own account of wired memory (`kern.redwood.wired`, kernel t393+): totals (wired, reserved at boot, kernelcache), every VM tag and allocation site, every zone |
| `r5qfs du PATH [DEPTH]` | megabytes under a directory: what fills the /private/var RAM filesystem |
| `blkbench A B [N]` | two block devices compared at the same random places, and each one's read speed |
| `jetsam_limit NAME [MB]` | a process's jetsam limits, or set them |

`kern.redwood.wired` exists because the stock interface (`mach_memory_info`, what zprint uses) needs the host
privileged port and `com.apple.private.memoryinfo`, which nothing of this port's own has. The sysctl returns the same
`mach_memory_info_t` records from `vm_page_diagnose()` (bsd/dev/redwood_ufs.c), root only.

## Where the 6 GB went (Test 393, kernel t393, the standard boot, idle on the home screen)

```
memsize 6144 MB, memorystatus_level 27%
total managed (all pages)                 3670 MB
total reserved (wired at boot)            2461 MB
total wired                               3663 MB
total kernelcache                           19 MB
   926 MB  tag 20 file       (the /private/var RAM filesystem's pages: ramfs uses VM_KERN_MEMORY_FILE)
    93 MB  tag  3 iokit      (IOSurfaces, drivers' buffers)
    57 MB  tag 12 zone
    52 MB  tag 29 kalloc_data
    40 MB  tag 11 pte
```

- **2.46 GB never became usable memory.** Everything below the loader's `topOfKernelData` is the kernel's static
  region, and the loader puts the **resident system image (md0, 2376 MB)** there. That is the iOS root volume the
  firmware copies from USERDATA block 0 into RAM at every start (R5qUfsLoad, CRC-checked). `rd=md0` boots from it.
- **0.93 GB is /private/var**, a RAM filesystem (ramfs) saved to flash by var_store. Leftover files removed on
  2026-11-20: /var/r5q/bin/locationd.orig and locationd.caps (31 MB each, copies kept on the PC) and
  /var/r5q/bin/fairplayd.H2 (19 MB, unused). Used: 1013 → 924 MB.
- Processes' footprints together: about 0.8 GB.

So the system image in RAM is the single biggest user: 40% of the phone.

## The system image from the flash (Test 394)

The flash already holds byte-for-byte what md0 holds: `blkbench /dev/rmd0 /dev/rufs_darwin 300` found 300 random
16 KB pieces and the first 64 MB the same. Reads through the loader's mailbox (redwood_ufs.c):

| | 16 KB at a random place | 1 MB in a row |
|---|---|---|
| md0 (RAM) | 5 µs | 8566 MB/s |
| darwin (flash, through the loader) | 405 µs | 264 MB/s |

Slower than RAM, but what iOS reads stays in the page cache, so only the first read of a page pays.

The change (`bsd/dev/memdev.c` in the r5q overlay, the donor-patched file plus this):
- md0 keeps reading its memory while the system starts.
- `mdev_to_flash()` first compares the first and last megabyte and 256 random 16 KB pieces of the flash with the
  memory. Anything different, and nothing changes.
- Then, under the device's lock (so no read of the memory is in flight), md0 is marked `mdFlash`. From then on its
  strategy routine reads `darwin` from the same offset (`redwood_ufs_read_named`).
- Then the 2376 MB go back to the VM with `ml_static_mfree`, the call the kernel uses for its boot-only segments.
- `mdevgetkernelrange()` refuses a flash disk, so the drivers' in-place firmware reads (`redwood_image_file`) skip
  md0. Their files are in md1, the small image (the boot log says so, e.g. "fw/a640_zap.mdt ... /dev/md1").
- Boot-arg `md_flash=S`: the switch S seconds after the flash can be read. `kern.redwood.md_flash`: read says
  whether it happened, writing 1 does it now (root).

### Result (Test 394, kernel t394, 2026-11-20)

- **By hand** (`r5q_sysctl kern.redwood.md_flash 1`, iOS idle on the home screen): "mdev: md0 is read from the flash
  (darwin, the same in 258 places) from now on; its 2376 MB of memory given back".

  | | before | after |
  |---|---|---|
  | memorystatus_level | 27% | 65% |
  | free | 744 MB | 3112 MB |
  | wired | 3690 MB | 1322 MB |
  | managed pages | 3670 MB | 6047 MB |

  Then Settings, Safari, Photos, Weather, Notes, Calendar, Maps and Camera all opened and drew; iOS kept running.
- **By itself** (boot-arg `md_flash=90`, a full cycle): the same, with no hands. Then a `/var` save (40 s of
  flash writes, interleaved with md0's reads) and six more app launches; 60% free afterwards; `kern.redwood.md_flash`
  reads 1.
- **Now standard:** `md_flash=90` is in `phone-args.txt`; the standard image is Test 394's (pipeline/README.md).

## mediaserverd and the Camera: 39 MB a callback (found and fixed 2026-11-20, Tests 394-395)

With the Camera open, mediaserverd grows by one 39 MB block about 40 times a second. It reached 2.7 to 3.7 GB
resident, and 56 GB of address space, which is why its phys_footprint read 56 GB. Closing or backgrounding the Camera
frees all of it. This, not the HEIC encoder, is what got mediaserverd killed taking photos: the encoder ran with
almost no memory left.

How it was found:
- `vmq mediaserverd` (proc_pidinfo PROC_PIDREGIONINFO; task_for_pid gives an unusable port on this port): 1511
  MALLOC_LARGE regions of 39 MB.
- `libr5q-bigmalloc.dylib` (wifi/inject_bigmalloc.c), injected with `r5qd_client.py reload`. It sets libmalloc's
  `malloc_logger` and `__syscall_logger` and prints a backtrace for every allocation of 30 MB or more on the console
  (`phone.py live`). Wrapping the zone functions alone saw nothing.
  - Note: re-`put` a dylib under a **new name**. A replaced file of the same name kept loading the old code.
- The allocation: `CMCapture+0x37b90 → CMBlockBufferCreateWithMemoryBlock → malloc(41343518)` on a dispatch queue,
  called from `CMCapture+0xda64` (the camera graph's audio path; `toolkit/tools/ios/dsc_sel.py` on the shared cache).
- The size is `ASBD.mBytesPerFrame × ([buf+0x38] + [buf+0x14])`, read from the stack by the tracer. The format is
  44.1 kHz mono 16-bit. `[+0x14]` = 941 frames, the real callback. `[+0x38]` = **20,670,818 frames** (468.7 s),
  constant across callbacks. The caller adds `[+0x38]` frames to the buffer's PTS as a duration, so it is a
  leading-silence or offset field. `[+0x40]` (the sample time) counts up by 941 per callback; the 8 bytes after it
  (host time) are 0.
- Ruled out: the RedwoodAudio plug-in reports latency 0, safety offset 0, and a correct zero-timestamp clock
  (userland/src/audio_plugin.c).

Next: find what sets `[+0x38]` in CMCapture's audio source node, the frames-to-PTS bookkeeping of the
`VirtualAudioDevice_DecoupledInput` input that the Camera's session (`PlayAndRecord_NoBluetooth_DefaultToSpeaker/LivePhoto`)
starts. The phone has no working microphone, so the camera graph's audio is silence either way.
Workarounds until then:
- Live Photo off. This only helps photos; video recording starts audio too.
- Keep Camera sessions short.

The earlier HEIC kill and the purple-looking preview should be retried once this is fixed.

### The cause and the fix (Test 395)

The writer of `[+0x38]` is CMCapture+0xab3c, in the audio source node's render handler (`+0xa900..`). It computes
**(host time elapsed since the node started) − (sample time elapsed since it started)**:
- the host time comes from the callback's AudioTimeStamp (flags: sample and host time valid);
- the start values are ivars at slots 0x1dd72abe4 (host) and 0x1dd72abe8 (sample).

If the difference is past a threshold (the ivar at slot 0x1dd72ac60), it is made into frames and the buffer is padded
with that much silence. That branch is skipped when the ivar `_silenceFramesFillingDisabled` (slot 0x1dd72ac0c) is
set. One of BWAudioSourceNode's init paths sets it and logs "Audio source node created with silence frame filling
disbaled". On this port the two clocks are 468.7 s apart from the first callback on, so every callback was padded.

Found with `toolkit/tools/ios/dsc_ivar_users.py` (new): every adrp+ldrsw that reads one ivar-offset slot in a range
of the cache. The users of the audio queue's slot led to the producer.

**Fix:** `libr5q-media.dylib` (wifi/inject_media.c), injected into mediaserverd. It wraps `-[BWAudioSourceNode
start:]` and sets `_silenceFramesFillingDisabled` (ivar offset 584 at run time) before the node's audio flows. It is
in root test395 (`/redwood/lib`, etc/inject.conf) and in build_root_app.sh.

| with the Camera open | before | after |
|---|---|---|
| mediaserverd resident | 2.7-3.7 GB within seconds | 75 MB, flat |
| after a photo | killed (or no photo) | 102 MB, `IMG_0004.HEIC` saved |
| after a 6 s video | (not tried) | 425 MB, `IMG_0005.MOV` saved; 56 MB once the Camera closes |

Still open: where the 468.7 s between the audio host clock and the sample clock comes from. With no working
microphone it changes nothing heard (the input is silence); it matters once the microphone works, for A/V sync.


### The 468.7 s, a little further (2026-11-20, after Test 398)

- `clockq` (wifi/clockq.c): mach_continuous_time equals mach_absolute_time (no sleep is ever counted), timebase
  19.2 MHz. So it is not a continuous-vs-absolute mix-up.
- `libr5q-audiodump.dylib` (wifi/inject_audiodump.c, debugging only) lists every BWAudioSourceNode ivar 1 s and 4 s
  after `-start:`. The fields the silence fill uses:
  - `_nextExpectedHostTime` (Q), `_nextExpectedSampleTime` (CMTime), `_ioprocTimeStampDeltaLimit` (CMTime, the
    threshold);
  - `_prevPTS`, `_prevNumFrames` (941), `_latencyOffset`, `_pullDuration` (941), `_useDecoupledIO` (1);
  - `_audioDeviceID` (121, VirtualAudioDevice_DecoupledInput), `_silenceFramesFillingDisabled` (+584).
- With the fill off, `_nextExpectedHostTime` tracks the clock (559.313 s against 559.314 s now).
- The callbacks' own sample times were small (2921, 3862, ...: `[+0x40]` of the queued buffers). So the 468.7 s was
  on the host-time side at the first callback, and stayed constant because both "next expected" values advance by
  each callback afterwards.
- Left: catching the first callback's host time against the node's start values. That needs the fill on again, with
  its 39 MB per callback, so it waits until the microphone works and A/V sync matters.

### The 468.7 s, narrowed to the first callback (2026-11-20)

- The render handler (CMCapture+0xa89c, iOS 16.5; `dsc_dis.py`), per callback, with `ts` the AudioTimeStamp:
  - sample = CMTimeMake(ts->mSampleTime, the format's rate);
  - if `_nextExpectedHostTime` (+296) is not 0 and `_nextExpectedSampleTime` (+304) is valid:
    host gap = HostTimeToNanos(ts->mHostTime − _nextExpectedHostTime), and sample gap = sample −
    _nextExpectedSampleTime;
  - if the host gap is past `_ioprocTimeStampDeltaLimit` (+328), the sample gap is not, and
    `_silenceFramesFillingDisabled` (+584) is clear, the host gap becomes frames (at the clock's rate against the
    host's, `_clock` +224) in the buffer's `[+0x38]`: the silence padding.
- **Measured** (libr5q-audiodump2, Camera open; filling turned ON from 1.1 s to 1.7 s after `-start:`):
  - Twice, no silence was made: `_silenceFramesGeneratedSinceLastAURenderProc` stayed 0, and mediaserverd stayed small.
  - `_nextExpectedHostTime` tracked the clock (307.977 s against 307.993 s now).
  - `_prevPTS` is host time at 44.1 kHz (13,580,827 / 44100 = 307.955 s), and the sample time counts from the
    stream's start (45,253 = 1.03 s).
  - So in steady state the clocks agree. The 468.7 s comes only from the first callback(s): the starting
    `_nextExpectedHostTime` against the first `mHostTime`. With filling on, the padding feeds the next expectations,
    so the gap stays.
- The audio stream is 44.1 kHz with 941-frame callbacks (21.3 ms), ~47 a second.
- **Caution:** the 4 s dump of the first build read a node already freed (the session had restarted): garbage, no
  crash. libr5q-audiodump3 (the same file, rewritten) retains the node. It samples just the clock fields at `-start:`
  (before and after the original) and every ~2 ms for ~0.6 s, to catch the first callback.
