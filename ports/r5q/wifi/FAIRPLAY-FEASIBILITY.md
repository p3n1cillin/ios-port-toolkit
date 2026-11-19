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

## The FairPlay kext (2026-11-19): the device keys are Apple silicon's UID and GID keys

- **Fetched** (the owner's OK): `kernelcache.release.iphone10b` (18,183,395 bytes) from the 20F66 IPSW on Apple's
  CDN, one member by HTTP range (`ios165/fetch-from-ipsw.py`). It is IMG4 with an LZFSE payload (`bvx2` at offset
  63), decoded with the system's liblzfse into a 50,888,732-byte arm64 Mach-O: an old-style prelinked kernelcache
  (`__PRELINK_INFO`). Local only (`$IOS_WORK/fairplay/kc/`).
- **The kext:** `com.apple.driver.FairPlayIOKit` 71.1.0, about 242 KB of `__TEXT`, 1.3 MB of `__text`; classes
  `com_apple_driver_FairPlayIOKit` (matched on IOResources) and `com_apple_driver_FairPlayIOKitUserClient`
  (entitlements `com.apple.private.FairPlayIOKitUserClient.access`, `...ProvInfoIOKitUserClient.access`).
  `OSBundleLibraries` names only the standard KPIs (mach, private, unsupported, iokit, libkern, bsd), so as a binary
  it would link against our XNU.
- **But at run time it looks up `IOAESAccelerator` and calls `performAES` with Apple's hardware keys.** Its own
  messages:
  - `generateSeed`: "IOAESAccelerator::performAES **GID** returned" / "performAES **UID** returned";
  - `getEncryptedSeedSegment`: performAES "in-field", "scrambler", "GID", "(scrambled)", "**(UID)**", plus
    `PIKDecryptWithFixedSharedKey`;
  - `generateInFieldSeed`, `encryptSUInfo`, `encryptWithWrappedKey`: performAES.
  
  Other user-client calls: `exchangeWithHoover` (an `AppleSTDP2700` display-port device, "probably means no Hoover
  on the device"), `SendFPMessage` (the LSKD key-delivery path), and `GetProductID` from `IODeviceTree:/product`
  `product-id`.
- **Answer to question 2: the device binding is the UID and GID keys.** These are AES keys fused into each Apple
  SoC's AES engine. Software cannot read them; they exist only inside Apple's silicon. The Snapdragon 855 has
  neither the keys nor an AES engine that holds them. FairPlay's seed (the device identity its keys are wrapped to)
  is made with them.
- **Verdict.**
  - Apple's own FairPlay code cannot do its job on this phone, because the keys it needs are not here.
  - The only ways around that would be faking Apple's hardware keys or writing our own FairPlay decryption. Both
    are circumvention and are out by this study's rules (and the real keys are secret anyway).
  - So **App Store (FairPlay-encrypted) apps cannot run on this port.**
  - What can run: apps whose code is not FairPlay-encrypted. That means Apple's own system apps (as now), apps the
    owner builds from source and signs, and unencrypted builds a developer gives out. Whether the port's code
    signing takes a self-signed app as is has not been tried yet.
  - **Study closed:** feasible only for unencrypted apps.
