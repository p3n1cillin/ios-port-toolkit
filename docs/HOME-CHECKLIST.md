# When you're home: S10 Lite test checklist

The phone is stuck on **Test 18** (a flickering green/blue screen, or a black screen). Nothing
permanent is wrong: only the recovery partition was changed, and Android's boot partition was never
touched. A watcher (restarted 13:59, waits until about 01:59) restores the original recovery the moment Android is back.

## 1. Bring the phone back

1. Hold **Volume Down + Side (power)** together until the screen goes black, then let go of both.
2. Let it boot. One of two things happens:
   - **Android starts** → good. The watcher restores the original recovery on its own within a minute.
     You don't have to do anything.
   - **It shows the green/blue test screen again, or keeps restarting** → get it into **Download Mode**
     (power off, then hold **Volume Down + Side** and plug in USB, or follow the on-screen prompt), and
     tell me "it's in Download Mode". I'll flash only the original recovery, the same fix used after Test 15.

## 2. Confirm it's recovered (I do this; you just tell me the phone is back)

I check that the recovery, boot and VBMeta partitions all match the originals, and that Android is
running with root. Until that check passes, no new test will start.

> **Update 2026-11-16 ~17:00:** Test 19 **hung** and Test 31 **passed** on the phone: the first full hand-off
> round trip on hardware. The cache-clean loop was the culprit. Next: a Test 32 that enters the real kernel
> with the Test 31 hand-off: **built and checked** (Unicorn: the real kernel reaches checkpoint 1 and returns;
> twin: passes in every mode). Needs your OK to run.
> **Test 32 PASSED (~16:58): the real iOS kernel ran on the phone to checkpoint 1 and returned.**
> Next: **Test 33** (built, checked, guarded) stops just after the kernel turns on its own MMU and caches.

## 3. The test plan: a decision tree (each run needs your OK; I run them, you watch the phone)

None of these boots iOS; they probe the firmware hand-off that keeps hanging. All are built, checked in
the emulator and on the virtual twin, and blocked until you say go.

**Step 1: Test 19.** It runs the loader's firmware-wide cache-cleaning loop, then resets *before* the
risky MMU switch. That loop is the one thing every hung test did and no passing test ever did.

- **19 comes back**: the loop is fine; the problem is the MMU switch and return. Go to step 2.
- **19 hangs**: the loop itself is the culprit. Run **Test 31** (skips that loop and flushes the whole
  cache the bootloader's way instead). If 31 comes back, that's the fix.

**Step 2: Test 30** (copies exactly how the phone's own bootloader switches caches and the MMU off), then
**Test 29** (flushes just the saved CPU state). If either comes back where 18 hung, we're past the blocker.

**Step 3, only if 30 and 29 both hang: localize it.** Run **24** first (can the phone's watchdog reboot it
by itself?). If yes, use **25-28**, which reboot by themselves on a hang; otherwise **20-23**. Stop at the
first that doesn't come back; its last marker names the failing step.

| Test | What it tells us |
|---|---|
| 19 | Is the firmware-wide cache-cleaning loop safe? |
| 31 | Fix if 19 hangs: skip that loop, flush everything instead |
| 30 | Fix: the phone's own bootloader shutdown sequence |
| 29 | Fix: flush the saved CPU state before the switch |
| 24 | Can the watchdog make future tests self-recovering? |
| 20-23 / 25-28 | Which exact step of the hand-off fails |

## What we already know

- The source-built iOS kernel boots all the way to the iOS home screen in the PC emulator (QEMU).
- The phone's hypervisor is **not** what's blocking the hand-off (confirmed by decompiling the phone's
  own firmware and by an emulated model). The suspect is cache behaviour, which these tests pin down.
- Test 17 passed on the phone and recovered on its own, so the logging and self-reset both work.
- A virtual twin of the phone (`work/ios-r5q/twin`) reproduces its logs almost exactly. With a cache model
  it reproduces the suspected hang mechanism and shows Tests 29, 30 and 31 fixing it.
- The twin also boots **iOS to the home screen** with the production loader on the phone's memory map,
  GIC addresses and screen size. Apart from the interrupt controller, iOS touched no phone hardware there.
  So once the hand-off is fixed, the next blocker on the phone is unlikely to be a missing driver
  (`work/ios-r5q/research/r5q-twin-ios-boot.md`).

Full technical detail is in `work/ios-r5q/SESSION.md` and `work/ios-r5q/research/`.
