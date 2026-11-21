# Modifications (Apple Public Source License 2.0, section 2.2(c))

This directory is derived from [G986N-iOS](https://github.com/leh0726/G986N-iOS) (commit 946de0fed8e5), itself a
port of [HackiOS](https://github.com/oosexgang/hackios-xnu-kernel) by OOSEX GANG, whose kernel side is under the
Apple Public Source License 2.0 ([LICENSE](LICENSE)). It was changed for the Samsung Galaxy S10 Lite (r5q)
native iOS port between November 1 and November 20, 2026; the git history of this repository has the details.

These files replace the donor's files of the same path in its `xnu/files/` tree when the kernel is built
(`kernel-build/build-xnu-sandbox.sh` copies them over it). What changed, broadly:

- `bsd/dev/redwood_ufs.c`: the r5q's UFS partitions, the `darwin` area in `userdata`, writes for the data volume,
  named partition reads for the modem's file servers.
- `bsd/dev/redwood_sec_ts.c`: the r5q's touch controller and buttons bus, the S2MU106 fuel gauge, the S2MU106
  charger's status and input limit over QUP0 SE4, battery reporting to iOS.
- `bsd/dev/redwood_buttons.c`, `redwood_cpufreq.c`, `iokit/Kernel/MiniIOMobileFramebuffer.cpp`,
  `osfmk/arm64/gic_v3_irq.c`: the r5q's buttons, CPU frequency domains, panel and interrupt controller.
- `bsd/dev/redwood_wifi.c`: the link with the r5q's modem (SMP2P, GLINK, QRTR), its file servers (rmtfs, TFTP with
  Android's path rules), pd-mapper, servreg, DIAG and memshare probes, the WCN3990 supplies, and the Wi-Fi
  firmware's WLFW handshake (MSA assignment through TrustZone, board data, WLAN configuration and mission mode).

- `bsd/dev/redwood_wifi.c` (Tests 111-243): the WLAN host side on top of WLFW (copy engines, HTC/WMI, scans,
  joining with WPA2 in the kernel, the network interface `en0`), and an RPMh resource vote helper used by sound and
  sensors.
- `bsd/dev/redwood_adsp.c`: the r5q's audio DSP (ADSP) start and its services, the sensors DSP (SLPI) started from the
  kernel with FastRPC (its file server) and the SEE sensors protocol (accelerometer, gyroscope, magnetometer, light,
  proximity; `kern.redwood.motion`), and the microphone's SLIMbus work (not working yet).
- `bsd/dev/redwood_sound.c`, `redwood_haptic.c`, `redwood_torch.c`, `redwood_adreno.c`: the r5q's speaker path,
  vibration motor, flashlight and GPU bring-up steps.
- `bsd/dev/redwood_buttons.c`: a read-only PMIC peek for diagnostics (`redwood_spmi_peek`).
- `bsd/miscfs/apfs/apfs_vnops.c`: fixed changes to what a file reads, made as it is read, each only where the file's
  name, size and old bytes match (iOS's CoreMotion motion capabilities and compass factory in the dyld shared cache,
  with the matching code-signature page hashes); see docs/SYSTEM-PATCHING.md. Since Test 373 also, only with
  boot-arg `bbfake=1`, the baseband manager's boot decision (libBasebandManager, subcache .17): its fake-boot path, so
  it does not try to flash a modem that is already running.
- `bsd/dev/redwood_wifi.c` (Tests 244-262, November 18-19): Bluetooth. The WCN3990 over a polled GENI UART, its
  firmware download, and its hand-over to iOS's bluetoothd: the kernel control `com.apple.uart.bluetooth`, an HCI
  filter, `/dev/uart.bluetooth`. See docs/BLUETOOTH.md.
- `iokit/Kernel/MiniIOMobileFramebuffer.cpp` (Test 262): `redwood_publish_named_service`, a plain named IOService (the
  `bluetooth` service that MobileBluetooth looks for).
- `bsd/dev/redwood_wifi.c` (Test 263): the Bluetooth line at 3 Mbit/s. `bsd/dev/redwood_adsp.c` (Tests 265-266):
  a polled GENI SPI probe of the WCD9340 (`mic=8`).
- `bsd/dev/redwood_camera.c` (Tests 267-269): the donor's camera driver (the POCO X5 Pro's hardware path), here
  refusing everything but its synthetic source (`cam=5 cam_fake=1`) on the S10 Lite, and saying why an open is refused.
- `bsd/dev/redwood_sec_ts.c` (Tests 270-274): the camera regulators (two S2MPB03) read, and one camera sensor powered
  (S2MPB03 LDOs, L14, the camera power domain, MCLK, reset), its ID read over bit-banged I2C, all put back.
- `bsd/dev/redwood_sec_ts.c` (Tests 275-329): the S5K3L6's receive path (CSI-PHY, CSID, IFE-lite into the loader's
  camera block through the SMMU), `redwood_r5q_cam_run()` handing frames to `/dev/redwood-camera`, a persistent
  pipeline (the camera stays up between iOS's sessions), timestamped per-step logging, and soak/hold test modes.
  Teardown parks the PLL0-fed RCGs on the crystal before PLL0 goes off (Test 327), and each thread drives the
  bit-banged buses through its own bus pointer (Test 324).
  iOS's auto-exposure applied to the sensor between frames (Test 328).
  The pipeline kept only across iOS's camera switches, powered off on a close (Test 329).
- `bsd/dev/redwood_camera.c` (Tests 290-299): `cam_r5q=1` serving the S10 Lite's sensor for every camera iOS asks for
  (mode 1028x772 kept on switches), and reads in flight counted so a session's end never frees the ring under them.
- `bsd/dev/redwood_wifi.c` (Tests 276-305): `redwood_r5q_rpmh_level()` (an RPMh rail to a level) and
  `redwood_r5q_rpmh_bcm()` (a bus clock manager's bandwidth vote, as Linux's bcm-voter) for others in the kernel.
- `bsd/dev/redwood_wifi.c` (Tests 352-358): `cell=1/2`, a QMI look at the modem over QRTR (SIM, operating mode,
  registration; states only); `bbstub=1` publishing the baseband stubs, and `kern.redwood.bb_answer`, their answers set
  while iOS runs.
- `bsd/dev/redwood_adsp.c` (Tests 359-362): `adsp_diag=1`, DIAG with the audio DSP over its QRTR link (its own F3
  messages, readable ones rendered in the kernel), the microphone's sequence waiting for its masks.
- `bsd/dev/redwood_adsp.c` (Tests 347-349): the motion sensors' rates as boot-args (`see_rate`, `see_mag_rate`) and the
  last 16 samples of each kept with their times (`struct redwood_motion` version 2).
- `bsd/dev/redwood_sec_ts.c` (Tests 334-351): the front (IMX616) and main (IMX586) cameras through a common Sony
  streaming path (`sony_stream`), their whole binned frames for iOS.
- `bsd/vfs/vfs_lookup.c` (Test 367): Apple's open-source XNU file (xnu-8796.121.2) with `namei()` wrapped: with
  `vfslog=1`, a lookup of a path naming the baseband's firmware is logged with its process and result.
- `iokit/conf/files` (Test 355): Apple's open-source XNU file (xnu-8796.121.2) as the donor ships it, with
  `iokit/Kernel/RedwoodBaseband.cpp` added to the build.
- `osfmk/arm/io_map.c` (Test 252): Apple's open-source XNU file (xnu-8796.121.2) with the I/O submap at 64 MB instead of
  8 MB, and its use logged; a full submap made `ml_io_map` wait for ever.
- `bsd/dev/memdev.c` (Test 394): Apple's open-source XNU file (xnu-8796.121.2) as the donor's patch changes it, plus:
  md0 can be switched to the phone's flash. Pieces of the flash are compared with md0's memory first; then reads go
  to `redwood_ufs_read_named("darwin")` and the memory goes back to the VM (`ml_static_mfree`). It runs on boot-arg
  `md_flash=S` or sysctl `kern.redwood.md_flash`. A rw-lock keeps reads of the memory and the switch apart.
- `bsd/dev/redwood_ufs.c` (Test 393, the donor's file): root-only sysctl `kern.redwood.wired`, the kernel's wired
  memory as `mach_memory_info_t` records from `vm_page_diagnose()`.

`bsd/kern/kern_exec.c` is Apple's open-source XNU file (xnu-8796.121.2) as the donor's patch changes it (library
insertion by program name, `kern.exec_inject`); here its setting holds 2048 bytes instead of 512.

New in this directory (not the donor's): `bsd/dev/redwood_wlfw_ath10k.h` and `redwood_wlfw_ath10k_ce.h`, generated
from Linux's ath10k (ISC, see their headers); `bsd/dev/redwood_wpa.h` (WPA2's key handling for the kernel's
supplicant); `iokit/Kernel/RedwoodBaseband.cpp` (Tests 355-358: listening stubs of the IOKit services iOS's
telephony stack opens -- AppleBaseband, AppleIPAppender, AppleBasebandPCIControl, AppleBasebandPCIInterface; and
since Test 374 AppleBasebandPCIPDPManagerBase, which libPCITransport's PCI event listener watches; since Tests
378-388 a QMUX transport on AppleBasebandPCIInterface -- async reads and writes, QMI CTL answered in the kernel, every
other service's QMI bridged to the modem over QRTR by `redwood_bb_qmi_send` in bsd/dev/redwood_wifi.c, whose QRTR port
table grew from 16 to 96).

/private/var on the flash (Tests 406-412, wifi/STORAGE-PLAN.md):

- `bsd/hfs/` is Apple's open-source HFS, hfs-627.100.6 (APSL 2.0, [APPLE_LICENSE](bsd/hfs/APPLE_LICENSE)), built into
  this kernel instead of loaded as hfs.kext. Each `core/*.c` file carries a "Modified for the Galaxy S10 Lite"
  note and includes `kext-config.h` first:
  - `kext-config.h`: CONFIG_PROTECT, QUOTA and secluded rename off; HFS_COMPRESSION on; the checked string
    builtins off (`_FORTIFY_SOURCE 0`, Test 408); kext names mapped to their in-kernel spellings.
  - `hfs_iokit.cpp` is replaced by the new `hfs_builtin.c`: VFS registration from `redwood_ufs_init()`, no sysctl
    nodes, IOKit lookups stubbed.
  - `hfs_encodings/` is copied as Apple ships it and not built.
- `bsd/conf/files`: Apple's open-source XNU file (xnu-8796.121.2) with the donor's patch applied and the HFS sources
  added.
- `bsd/dev/redwood_ufs.c`: `/dev/ufs_var` and `/dev/rufs_var` (boot-arg `ufs_var=1`), darwin from 16 GiB to its end
  as a disk. The logical block size can be set, partial writes are read-modify-write, and cluster I/O buffers are
  mapped with `buf_map()`.
- `bsd/miscfs/apfs/apfs_vfsops.c` (the donor's file): the root volume reports its own size plus the size and free
  space of an HFS volume mounted at `/private/var`, as an APFS container shared by system and data would.
- `bsd/dev/redwood_sec_ts.c` (Test 413): device mappings are kept and reused (`r5q_io_map_kept` in front of
  `ml_io_map` / `ml_io_map_unmappable`). Each camera session had mapped the same ranges again and leaked the kernel's
  I/O map.
- `bsd/dev/redwood_sec_ts.c` (Test 414): the IMX586's two PLLs get pre-divider 2 with the multipliers scaled to the
  same VCOs (pixel 225, output 260), inside the limits the sensor reports (CCS 0x110c-0x1116). Boot-arg `cam586_pre`.
- `bsd/dev/redwood_wifi.c` (Test 416): a deauthentication or disassociation from the access point counts as a
  refusal only when it is addressed to us or broadcast.
