# Driving iOS on the r5q twin by touch (EMULATION ONLY, 2026-11-16)

Setup: twin iOS mode with the TrustZone layer, 4 cores, loader-twin-ios.efi, source XNU, iOS 16.5, and a
virtual touch panel plus buttons (`TWIN_INPUT=1`: QEMU virtio-tablet/keyboard at 0x0A000000, passed through
by stage 2 in iOS mode; the donor kernel polls them with `virtio_input=1`). Driven with
`twin/twin_ctl.py` (tap/swipe/key/shot over QMP and HMP). 42 screenshots are in
`port/build/twin-ios/loader-twin-ios-secure-smp4/drive/` and the overview is in `drive-session.png` there.
Home screen about 3.3 min after power-on; driving from about 4 min to 26 min.

| Area | Result |
|---|---|
| Touch: tap, swipe/scroll with momentum, slow edge swipes | works |
| Buttons: Home, Lock/wake | work; swipe-up unlock (no passcode) works |
| Settings: navigation, General → About | works; About: iOS 16.5, "iPhone X" (the donor userland's identity), capacity 2 GB, **available Zero KB** |
| Display & Brightness → Dark | works system-wide, live (apps, home screen, widgets, dock) |
| Calendar | works: first-run sheet, day view with live "now" line, month view, momentum scroll into 2027; "+" greyed |
| Notes | first-run sheet, iCloud prompt, compose, typing "hello from the twin" on the on-screen keyboard works; **the note is not kept** (back in the list: "No Notes"; no writable storage) |
| Calculator (from Control Center) | works: 770 × 8 = 6,160 |
| Control Center | works (blur, toggles; Wi-Fi/Bluetooth greyed) |
| App switcher | works; flicking a card up closes the app |
| Spotlight | opens, Siri Suggestions, typing works; no app results (no search index without storage) |
| Lock screen | works (date, time, camera shortcut) |
| Safari | opens to Favorites; pages: "not connected to the Internet" (the run has no network) |
| Phone | keypad opens (no modem; no call attempted) |
| Messages | first-run sheet, then empty inbox |
| Maps | first-run sheets, then the UI with "No Internet connection" and no map |
| Reminders | first-run sheet |
| Clock | **never draws** (black); its log shows lookups for missing services (sleepd, healthd, nanoregistry) |
| Photos | **crashes at launch** (abort after `tccd`, the privacy daemon, is missing) |

Rendering is all software (backboardd "Software renderer"), so app launches take 15-40 s, and the on-screen
keyboard's blurred backdrop shows artifacts. The home-screen widgets stay empty.

## What it means

- Inside the twin, iOS is **interactively usable**: touch, buttons, typing, gestures, system settings, and the
  built-in apps that need neither storage, network nor missing daemons.
- The gaps are in the userland/environment, not the r5q boot path: **no writable storage** (nothing saves),
  **no network**, **no GPU**, and a few daemons absent from this userland (`tccd`, `sleepd`, `healthd`,
  `mobileactivationd`). There is no Setup Assistant or activation; this image goes straight to SpringBoard.
- For the phone: the virtual touch panel stands in for the S10 Lite's real touchscreen, which needs its own
  driver (on SM8150 the panel's touch controller sits on an I2C/SPI bus behind QUP), and on-device storage needs a
  UFS path. Those, plus the hand-off fix (Tests 19-31), separate this demo from the same thing on the phone.
