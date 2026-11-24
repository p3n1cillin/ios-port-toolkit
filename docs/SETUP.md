# Setting up a machine for the S10 Lite port

The scripts were written on Windows 11 with WSL 2 (Ubuntu), and they assume that split: the phone is driven from
Windows (fastboot over USB), and everything is built inside WSL. Nothing in them depends on a user name or a
checkout location any more; what they do expect is below.

> Read [SAFETY.md](../SAFETY.md) first. The port replaces Android on the phone; keep the stock firmware for your
> exact model at hand so Download Mode can always restore it.

## 1. Clone

```bash
git clone https://github.com/p3n1cillin/ios-port-toolkit.git
```

`ports/r5q` is the port's working tree. Shell scripts find it on their own (or take `R5Q_PORT`); Python scripts
take it as an argument or from their own location.

## 2. Windows side

- Python 3.11+ and `pip install pyserial pillow`
- Android platform tools (`fastboot`, `adb`) on `PATH`, or `PLATFORM_TOOLS` set to their folder
- Samsung USB drivers, and samloader or Odin for Download Mode

`ports/r5q/phone.py` is the everyday tool (flash BOOT, restart, follow the kernel's log, set boot-args, read the
shared memory, take screenshots): run it from `ports/r5q`.

## 3. WSL side: the workspace

The Linux-side scripts use one workspace directory, by default `/var/tmp/codex-ios-r5q/work`. Shell scripts take
another with `R5Q_WORK=...`; the Python build scripts (`build_xnu_linux.py`, `build_loader_linux.py`,
`build_uefi_linux.py`) use the default, so either create it there or make it a symbolic link to yours.

Packages: `clang lld llvm python3 git bubblewrap util-linux qemu-system-arm` (and `gcc-aarch64-linux-gnu`
binutils for the research scripts).

What goes in the workspace:

| Directory | What | Where from |
| --- | --- | --- |
| `linux-source/` | the donor port (kernel, loader, iOS user space build) | [G986N-iOS](https://github.com/leh0726/G986N-iOS), commit `946de0fed8e5` |
| `apple-xnu-8796.121.2/` | Apple's XNU | [apple-oss-distributions/xnu](https://github.com/apple-oss-distributions/xnu), commit `c54f35ca7679` |
| `xnu-source-cache/` | XNU's build dependencies and `MacOSX13.3.sdk.tar.xz` | listed with their commits in `ports/r5q/research/xnu-source-staging.json` and `macos-sdk-input.json` (the SDK's SHA-256 is checked) |
| `theos/` | the iOS toolchain (`toolchain/linux/iphone`) and `sdks/iPhoneOS16.5.sdk` | [Theos](https://theos.dev) and an iOS 16.5 SDK you obtain yourself |
| `Mu-Silicium/`, `mu-venv/` | the phone's UEFI and its Python environment | [Mu-Silicium](https://github.com/Project-Silicium/Mu-Silicium), commit `e51cd877e31c`; its own setup instructions for the venv |
| `sdks-source/iPhoneOS16.5.sdk` | the iOS SDK `pipeline/build_init.sh` and `build_root_app.sh` compile the iOS-side libraries with | the same SDK as Theos's (a copy or a symbolic link) |
| `openh264-ios/` | Cisco's OpenH264 v2.4.1 encoder as a static arm64 iOS library, for the camera's H.264 video (`libredwood-media.dylib`, Tests 431-434) | `bash ports/r5q/pipeline/build_openh264.sh` (clones the release, builds `libopenh264_enc.a`); BSD-2-Clause -- Cisco's patent licence covers only its own binaries, see openh264.org before distributing a build |

`pipeline/build_init.sh` builds `libredwood-media.dylib` only when OpenH264 and the donor's `userland/src` files
(with `ports/r5q/userland-patches/` applied) are there; otherwise it says what is missing and builds init and
var_store alone.

The iOS 16.5 user space (the system image) is built by the donor's tooling from Apple's update files; it is not
in either repository.

## 4. Build and run one test

From `ports/r5q`, in WSL (paths are examples):

```bash
python3 -I build_xnu_linux.py "$PWD" "$PWD/port/build/xnu-r5q-tNNN.macho"
python3 -I build_loader_linux.py port/xnuloader port/build/r5q-testNNN-loader.efi --testNNN
python3 build_testNNN_shim.py "$PWD/port"
python3 package_install_boot.py . NNN
```

then from Windows, with the phone on the cable:

```powershell
python phone.py cycle port\build\r5q-install-boot-NNN.img 120 [boot-args...]
```

`wifi/WIFI-PLAN.md` and `SESSION.md` show the arguments each stage used.

## 5. Phone-side data the port reads

Some inputs come from the phone's own stock firmware and are never in this repository: for Wi-Fi, the vendor
partition's `/vendor/firmware/wlanmdsp.mbn` and `wlan/qca_cld/*`, packed into a small store and written to
`userdata` at 4 GiB + 64 MiB (see the Test 108 entry in `wifi/WIFI-PLAN.md`).

## Environment variables

| Variable | Used by | Default |
| --- | --- | --- |
| `R5Q_PORT` | shell scripts | the script's own `ports/r5q` |
| `R5Q_WORK` | shell scripts | `/var/tmp/codex-ios-r5q/work` |
| `R5Q_STOCK` | firmware extraction scripts | none: the folder with your stock `AP_/BL_/CP_` files |
| `R5Q_SCRATCH` | a few check scripts | `$TMPDIR` or `/tmp` |
| `PLATFORM_TOOLS` | PowerShell test runners | `fastboot` on `PATH` |
| `R5Q_OUTPUTS` | PowerShell test runners | `ports/r5q/outputs` |
| `R5Q_RECOVERY_IMG` | `odin_restore_recovery.py` | `recovery-original.img` |
| `GHIDRA_HOME` | the research decompile scripts | none |
