# FairPlay on the S10 Lite port: feasibility (started 2026-11-19, not ruled out)

The owner asked not to rule FairPlay out. Until now the port's notes called it impossible ("needs Apple's FairPlay
kernel extension"). This page collects what is known and what a feasibility study has to answer. Nothing here has
been tested yet.

## What FairPlay does for App Store apps

- An App Store app's main executable (and its frameworks) carry `LC_ENCRYPTION_INFO(_64)` with `cryptid = 1`: their
  `__TEXT` pages are encrypted. Apps built here and installed by iOS's own installer are not encrypted, which is why
  they run today.
- At `exec`/`mmap`, XNU asks a "text crypter" for the encrypted range: `text_crypter_create_hook` (open-source XNU,
  `osfmk/kern/page_decrypt.c`, used by `bsd/kern/mach_loader.c` `set_code_unprotect` and `mremap_encrypted`). On
  iOS, Apple's FairPlay kernel extension registers that hook. We run our own XNU build, so no hook is registered and
  encrypted apps cannot start.
- The keys come from the app's `SC_Info` (`.sinf`/`.supp`, bound to the Apple ID that bought it) and the device's
  FairPlay identity, negotiated by user-space daemons: `fairplayd.H2` (`/usr/sbin/fairplayd.H2`, launchd job
  `com.apple.fairplayd.H2`, user mobile) and `fairplaydeviceidentityd` (`/usr/libexec`).

## What is in our extracted iOS 16.5 (iPhone10,3, 20F66)

- `System/Library/LaunchDaemons/com.apple.fairplayd.H2.plist` (program `/usr/sbin/fairplayd.H2`, mobile).
- `System/Library/LaunchDaemons/com.apple.fairplaydeviceidentityd.plist` and `usr/libexec/fairplaydeviceidentityd`.
- **Not extracted:** `usr/sbin/fairplayd.H2` itself (our root is built from a selected subset of the IPSW,
  `select-system.py` / `system.files`), and the FairPlay kext (in Apple's kernelcache, which we do not boot).

## Questions the study has to answer

1. **The kernel side.** What does the FairPlay kext provide besides the text-crypter hook: an IOKit user client that
   `fairplayd.H2` opens (class name, selectors), and what each selector does? Readable from the kernelcache's kext
   (disassembly) and from `fairplayd.H2`'s IOKit calls.
2. **Where the keys live.** Are FairPlay's device keys derived from hardware (the AES engine's UID key, the Secure
   Enclave), or provisioned by Apple's servers and stored in the keychain/files? "H2" in the daemon's name is a
   hardware-generation suffix; if decryption needs the UID key in silicon, it cannot run on a Qualcomm SoC as is.
3. **The account side.** Does an iOS without real activation get FairPlay provisioning from Apple at all (the
   activation records here are not Apple's)?
4. **Within the rules.** Any route must use the owner's own purchases and Apple's own code paths on the phone, with no
   circumvention of DRM for content the owner has no rights to, and no publishing of Apple binaries or keys.

## First steps (cheap, no phone needed)

- Fetch `usr/sbin/fairplayd.H2` from the IPSW into the local extraction (as the other selected files were), list its
  IOKit service names, Mach services and frameworks.
- Locate the FairPlay kext in the iPhone10,3 20F66 kernelcache (kext bundle IDs containing "FairPlay"), list its
  IOUserClient classes and whether it calls into the AES engine or SEP.
- Write the answer to question 2 here; that decides whether the rest is worth doing.

## First look at `fairplayd.H2` (2026-11-19, the owner's OK to fetch it; local only, never in the repository)

- **No download was needed:** the IPSW's system image from the original extraction (`ios165/rootfs.dmg`, 20F66) is
  still local; `7z e` took `usr/sbin/fairplayd.H2` out of it (19,316,144 bytes, Mach-O arm64 PIE) into
  `$IOS_WORK/fairplay/` (local).
- **Linked:** IOKit, CoreFoundation, Security, Foundation, AuthKit (Apple ID), ManagedConfiguration, libMobileGestalt,
  libbsm, libSystem, libobjc. Uses `uniqueDeviceIdentifier` (MobileGestalt) and `gethostuuid`.
- **The kernel side, named:** its entitlements include `com.apple.security.iokit-user-client-class` =
  **`com_apple_driver_FairPlayIOKitUserClient`** and `com.apple.private.FairPlayIOKitUserClient.access`; it calls
  `IOServiceMatching` / `IOServiceGetMatchingServices` / `IOServiceOpen`. That user client is Apple's FairPlay kext's
  (in the kernelcache, absent from our XNU). **That is why it exits at once (Test 198):** the service it opens does
  not exist here.
- **Other services:** Mach services `com.apple.fairplayd`, `com.apple.fairplayd.versioned`, `com.apple.unfreed`
  (launchd plist), and it talks to **`com.apple.adid`** (machine-identity provisioning: notifications
  `midchangedV1/V1.5/V3`). It keeps an "fpkeybag" (`com.apple.fairplayd.resync-fpkeybag`) in the `fpsd` storage
  class (`com.apple.private.security.storage.fpsd`), and embeds the "Apple FairPlay Certification Authority"
  certificates.
- **Obfuscated:** only ~18,300 strings in 19 MB; the service name string for its IOServiceMatching is not in clear.
- **What this says so far:** user space is Apple's daemon plus Apple-provisioned identity (adid, AuthKit). The device
  binding and the decryption itself are in the kext (the text-crypter hook and the user client). Question 2 (are the
  keys bound to silicon: the UID-keyed AES engine or the SEP?) is answered by the kext, so **the next step needs the
  20F66 kernelcache** (not local; another file of the same IPSW).
- **Rules (unchanged):** this is reading Apple's binaries to judge interoperability. Any working route has to be
  Apple's own FairPlay code with the owner's own purchases. Writing our own FairPlay decryption, or faking an Apple
  device identity, would be circumvention and is out.
