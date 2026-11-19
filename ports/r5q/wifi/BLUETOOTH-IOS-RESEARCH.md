# How iOS 16.5 (20F66, iPhone10,3) reaches its Bluetooth controller — and how to give it a Qualcomm one

Read-only research, 2026-11-18. Sources: `rootfs/usr/sbin/bluetoothd`, `rootfs/usr/sbin/BlueTool` (arm64 Mach-O,
not in the shared cache), the launchd plists, and the port's own `donor-linux/userland/mkjobs.py` and
`port/xnuloader/loader.c`. All addresses are unslid VM addresses in the named binary. Scratch helpers (not part of
the toolkit) were an objdump annotator that resolves stubs and adrp+add strings.

## 0. Short answer

* iPhone X's Bluetooth is a **Broadcom BCM4355C0 over a UART speaking H4**. Neither daemon opens `/dev/uart.bluetooth`
  for data. Both reach the UART through **kernel-control sockets** (`PF_SYSTEM`/`SYSPROTO_CONTROL`):
  * BlueTool: `com.apple.uart.bluetooth`, a byte stream with termios passed as socket options;
  * bluetoothd: `com.apple.uart.sk.bluetooth` by default, a control socket plus a **Skywalk channel** for the data.
    The plain `com.apple.uart.bluetooth` byte-stream path is still built in, and an environment variable selects it.
* bluetoothd does not decide the chip from the controller. It decides it from **MobileGestalt's product type**
  (`MGIsDeviceOneOfType` table → chipset enum, BCM_4355C0 = 12). Firmware load and other chip setup ("boot", "init",
  "deepsleep") are **delegated to BlueTool over XPC** (`com.apple.BlueTool`, `RunCommand`). BlueTool looks the
  firmware up by (MG ProductHash, chip from HCI Read_Local_Version, MG WifiVendor). The firmware is **compiled into
  BlueTool**: its `__TEXT,__const` is 9 MB of HCD images, so the IPSW has no `/usr/share/firmware/bluetooth`.
* Recommended design: the kernel keeps the WCN3990 (power, QCA patch/NVM download, baud) and offers it as a **kctl
  `com.apple.uart.bluetooth` H4 byte stream**. An HCI filter in front of the chip answers Broadcom/Apple vendor
  commands (OGF 0x3F) itself and never forwards them to the QCA chip. In userland:
  1. **set `BT_HCI_TRANSPORT=H4:POSIX:com.apple.uart.bluetooth,3000000`** in the bluetoothd job (the port builds its
     own launchd jobs, so this needs no signature work);
  2. **replace BlueTool with a small `com.apple.BlueTool` XPC shim** that answers `boot`/`init`/`deepsleep` with
     result 0;
  3. remove `bluetooth` from mkjobs.py's DENY list.

  No Apple binary needs patching for a first light.

## 1. Transport

### 1.1 Gate in bluetoothd's main (0x100026368)
```
access("/dev/cu.bluetooth", F_OK)  || access("/dev/uart.bluetooth", F_OK)
  || IOServiceGetMatchingService(IOServiceNameMatching("marconi-bt"))
  || IOServiceGetMatchingService(IOServiceNameMatching("bluetooth"))
  else  "%s: No valid serial port found (tried cu.bluetooth and uart.bluetooth)"  -> errx(0) (0x1000268c4)
```
This is only an existence check. Because the exit status is 0 and the plist has `KeepAlive {SuccessfulExit: false}`,
launchd does not restart it. Today bluetoothd would end here: the loader's tree has no `bluetooth` node, and mkjobs.py
DENYs `bluetooth` anyway.

### 1.2 Transport selection (0x100155f8c)
* `getenv("BT_HCI_TRANSPORT")` first. Grammar: `TRANSPORT:DRIVER:options`, split on `:`; the options go to the
  driver (`strdup` at 0x100156090). Error strings: "Searching for transport", "Searching for driver",
  "FATAL ERROR - HCI Transport %s could not be matched".
* Otherwise it reads the device tree: `IOServiceNameMatching("marconi-bt")`, then `"bluetooth"`, then
  `IORegistryEntrySearchCFProperty(..., "transport-encoding", IODeviceTree)` as 4 bytes (0x1000e74b0; log
  "DeviceTree transport = 0x%08x"). If the property is missing, the default at 0x100844870 is **3**.
* The jump table at 0x1006771b0 maps the value to a transport. Its logs at 0x100156120 and on are "HCI Transport is
  set to H4 / H4DS / H4BC / H5 / APPLEBT":

| transport-encoding | transport |
| --- | --- |
| 0 | USB |
| 1 | H4 |
| 2 | H4DS |
| 3 | H4BC (the default) |
| 4 | H5 |
| 5 | BCSP→USB |
| 6 | APPLEBT |
| 7 | PCIe |

* Transports table at 0x1007fd3e8: `USB, H4DS, H4BC, H4, H5, APPLEBT, VIRT, PCIE, PCIESW`. Drivers table at
  0x1007fd438: `USB, POSIX, APPLEBT, VIRT, PCIE, PCIESW`.
* `transport-speed` (DT, 4 bytes) or `BT_BAUDRATE` (environment) sets the baud (0x1000e6fa4). The default is
  0x249f00 = 2 400 000.
* APPLEBT (`AppleBTHci`/`AppleBluetoothModule` user clients), PCIE/PCIESW (`AppleConvergedIPCInterface` +
  `ACIPCInterfaceTransport` + `IOSkywalkNexusUUID`) and the ACI/Marconi names are for Apple's own chips and PCIe
  Broadcoms (iPhone XS and later, M1/M2). They are not used on iPhone X.

### 1.3 The POSIX (UART) driver: open at 0x1000666a0
* Options string `name[,baud]` (split on `,`). With no options the name is **`com.apple.uart.sk.bluetooth`**.
* `socket(32 /*PF_SYSTEM*/, 1 /*SOCK_STREAM*/, 2 /*SYSPROTO_CONTROL*/)`, `ioctl(CTLIOCGINFO=0xc0644e03)`, then
  `connect`.
* If the name is `com.apple.uart.sk.bluetooth` (Skywalk):
  * `setsockopt(SYSPROTO_CONTROL, 0x800c5414, 12-byte cfg from 0x1006750a0)`;
  * `getsockopt(SYSPROTO_CONTROL, 0x40105412)` returns a 16-byte nexus UUID;
  * then `os_channel_create`. TX and RX go through `os_channel_*` slots, with kevent (0x100067100…0x1000675fc,
    "BM3UART os_channel_sync tx failed").
* Any other name (plain kctl):
  * `getsockopt(SYSPROTO_CONTROL, 0x40487413 = TIOCGETA, termios, 0x48)`;
  * `cfmakeraw` and `cfsetspeed`, with flow-control bits by mode;
  * `setsockopt(SYSPROTO_CONTROL, 0x80487414 = TIOCSETA, termios, 0x48)` (0x100066c88);
  * then plain **`read()` / `write()` on the socket**, H4-framed.

  The return values of the termios options are not checked.
* `/dev/btwake` is opened for BT_WAKE on the low-power H4 variants (0x100066604, "Unable to turn on BT)_WAKE
  signal").

### 1.4 The VIRT driver (0x1000cb6f0): a ready-made test path
`BT_HCI_TRANSPORT=VIRT:VIRT:/dev/foo` makes bluetoothd open:
* `/dev/foo-in` with `O_WRONLY|O_NOCTTY` (host → controller);
* `/dev/foo-out` with `O_RDONLY|O_NONBLOCK|O_NOCTTY` (controller → host).

The framing is H4 ("VirtualCTRL: unknown packet type received %d", "RxPacketHeader - bogus packet length"). This is
a fallback if a kctl is inconvenient: two XNU cdevs, or FIFOs fed by a helper.

### 1.5 BlueTool's own UART access
* Its `device` command (getopt `hd:Ds:Sc`, 0x10001d1e0) defaults to the kctl **`com.apple.uart.bluetooth`**; the
  environment variable `BT_UART` overrides it. It also has `-d <device>`, `-s <baud>`, `-S` (speed from DT) and
  `-D` (PCIe).
* Power, reset and wake go through **`/dev/btpoweroff`, `/dev/btreset`, `/dev/btwake`**.
* `bcm -A` takes the BD_ADDR from DT `local-mac-address`; otherwise from the env `BT_DEVICE_ADDRESS`, the XPC
  `btaddress`, or a random address from the boot-args.

## 2. Bring-up sequence

### 2.1 Who does what
* bluetoothd's PowerManager logs "Booting chipset", "Powering on chipset", "Powering off chipset" and "Resetting
  chipset" (0x100272b4c…). It sends XPC to the Mach service **`com.apple.BlueTool`**: `kMsgId = "RunCommand"`,
  `command = "boot" | "init" | "deepsleep"` (0x100272880–0x100272930).
* The reply handler (0x1002722c8) reads `result` (int64), `firmware` (string) and `manufacturer` (int64):
  * `result == 0`: success ("BlueTool finished running %s script"); "%lld/%s" of manufacturer and firmware is kept;
  * `result == 10 or 13`: "Couldn't find a platform / chipset / firmware combination for the HW…";
  * any other value: "BlueTool failed to run %s script with result %ld";
  * an XPC error: an internal error 0xd5.

  The PowerManager then logs "Unknown platform/chipset/manufacturer combination, restarting" or "timed out booting
  the chip, restarting" (0x1003a5fc4).
* BlueTool's launchd job is `/usr/sbin/BlueTool -R`, user `mobile`, Mach service `com.apple.BlueTool`.

### 2.2 How BlueTool picks the script (`runCommandForCurrentConfig`, 0x1000161a0)
* **Platform**: `MGCopyAnswer(kMGQProductHash)`, a 20-byte SHA-1 printed as 40 hex characters ("Platform is %s",
  0x100040c4c).
  * Boot-args `wifibt-external=0x442514e4 / 0x443314e4 / 0x443414e4` override it.
  * If there is no platform, `runCommand…` returns **10** (0x1000162a0).
* **Chip** (`getChipset`, 0x100041094): the transport comes from the DT, like bluetoothd's.
  * transport 7 (PCIe): from `AppleBCMWLANBusInterfacePCIe` `HWIdentifiers`;
  * transport 6 (AppleBT): MG checks for APPLE_M2 / M2_5;
  * any other transport: **HCI Read_Local_Version_Information over the UART** (0x100018ddc).

  Then a switch on the manufacturer:

  | Manufacturer | Chip |
  | --- | --- |
  | 0x0f Broadcom | by LMP subversion (below) |
  | 0x0a CSR | `CSR` or `BC6` |
  | 0x0d / 0x4c | ACI (`ACIBGA`/`ACIFPGA`) |
  | **any other, such as Qualcomm 0x1d** | **`ALL`** |

  Broadcom LMP subversions (the Linux btbcm numbers):

  | LMP subversion | Chip |
  | --- | --- |
  | 0x6103 | **BCM4355C0** |
  | 0x430a | BCM4355B3 |
  | 0x6214 | BCM4355C1 |
  | 0x610c | BCM4350C2 |
  | 0x6607 | BCM4350C5 |
  | 0x420e | BCM4357B1 |
  | 0x6206 | BCM43452A2 |
* **Vendor**: `MGCopyAnswer(kMGQWifiVendor)`, translated to `MUR` or `USI`. The SKU comes from MG `WSKU`.
* **Override scripts**: `/etc/bluetool/<HWModelStr>.<boot|init|deepsleep>.script`, then
  `/etc/bluetool/<platform>.<cmd>.script` (0x100016668/0x100016718). They are searched **only if MG `ReleaseType` is
  `Vendor` or `Internal`** (CFEqual at 0x100016270/0x100016288, gate at 0x100016474). Retail builds therefore always
  use the built-in tables.
* **Built-in table**: 221 registrations (`0x100014cf4(platformHash, chip, vendor, …, hcdName)`) cover the iPhone 8/X
  family. Each registration names a built-in boot/init/deepsleep function. Examples:
  * Joy: `e9fefef6e6f9…`, `61792bce3a5b…`;
  * Fear: `72b682e4b7d6…`, `c8c6f69f0067…`, `16fce4bf46be…`, `8be45d742522…`;
  * Anger: `e3522edc03a1…`, `aebc7f12a3b6…`;
  * all are BCM4355C0 MUR/USI, e.g. `BCM4355C0_19.1.206.5411_JoyA_OS_MUR_MCC_TRS_20210531.hcd`.

  There are **no `ALL`-chip entries for these platforms**, so a Qualcomm answer gets "Error - no firmware for this
  platform. Looking for %s %s %s". The hashes are not SHA-1 of "D22AP"/"iPhone10,3" (tested), so which pair is D22 is
  not known.
* **What a Broadcom boot sends** (the `bcm` command and its help strings):
  * the minidriver and the HCD patchram (Write RAM / Launch RAM: "Loading segment", "pktLength %zu WriteRAM");
  * the baud change (`-b`/`-B`, "HCI UART Baud Rate");
  * BD_ADDR (`-a`/`-A`);
  * the sleep mode (`-s`, 0x09 = H5);
  * the regulatory PTB (`-K`/`-x`) and calibration from DT `bluetooth-tx-calibration`, `bluetooth-rx-calibration`
    and `bluetooth-taurus-calibration` (missing ones are skipped: "No cal data found, move along");
  * then HCI_Reset ("ready to send HCI_Reset command").

### 2.3 How bluetoothd decides the chipset
It does not ask the controller. The platform factory at 0x10029aa28 runs a chain of `MGIsDeviceOneOfType(hash
list)` calls, each followed by `new Platform(0x1360 bytes)` and `0x1002597f0(obj, chipsetEnum)`.
* The enum names come from 0x100282c68: 0 THIRD_PARTY_DONGLE, 1 BCM_4350C2 … 11 BCM_4355B3, **12 BCM_4355C0**,
  13 BCM_4355C1, 15 BCM_4357, 19 BCM_4377, 20 BCM_4378, 22 BCM_4387, 23 BCM_4388, 2000/2001/2002 APPLE_M1/M2/M2_5.
* If nothing matches: fault "Unknown platform 0x%08x detected" (`MGGetProductType`, 0x10029c174), and the platform
  pointer is left unset.

**To verify on the phone**: print `MGGetProductType()` with `mg_test` and look it up in the factory (0x10029aa28 to
0x10029c150). The expectation is BCM_4355C0 (12), but it was **not proven**, because the product-type hash function is
unknown.

## 3. Skipping firmware load, unknown controllers, required vendor commands

* There is **no "already initialised, skip" path in BlueTool's retail flow**. The options for skipping the chip boot:
  * **(a) A `com.apple.BlueTool` shim** that replies `result = 0` (recommended).
  * **(b) The override scripts**. They need MG ReleaseType Internal or Vendor, which has wide side effects.
  * **(c) Make the real BlueTool succeed** by faking a Broadcom identity:
    * Read_Local_Version → manufacturer 15 / subversion 0x6103;
    * MG ProductHash and WifiVendor matching a registration;
    * accept and discard the ~hundreds of KB of HCD Write-RAM VSCs.

    This is fragile.
  * **(d) Patch bluetoothd** so the XPC reply reads as 0 (`bl xpc_dictionary_get_int64` at 0x1002723ac →
    `mov x0,#0`). BlueTool must still answer, so this saves nothing over (a).
* An unknown controller **manufacturer** is tolerated by BlueTool (chip `ALL`) but finds no firmware entry.
  bluetoothd never reads the manufacturer for its chipset choice: it keeps the MG platform's BCM_4355C0 and
  **will send Broadcom VSCs (OGF 0x3F)** from `Stack/vsc/bcm/bcm_vsc.c`. There are 219 `BD_VSC_*` names. Examples:
  * WRITE_SLEEPMODE_PARAM (bluetoothd logs "Setting sleep mode to %s");
  * LE_META_VSC, ADVBUF_*, LE_CREATE_EXT_ADV_INSTANCE…;
  * OLYMPIC_* coex/settings, ENABLE_HIGH_POWER, CORE_DUMP, SET_FW_TRACE_MODE…
* Tolerance:
  * debug VSCs log "… not implemented for current chipset!";
  * the parameter-cache layer has "**Assuming unsupported command %!, ignoring error**" for OI command ids
    0x2bd–0x301 and "Unsupported command %!, ignoring error" for 0x2cd (0x100123180–0x100123270).

  So *some* VSC failures are explicitly ignored. Which VSC failures are fatal was **not** enumerated. Expected
  outcome: core HCI (inquiry, LE scan, ACL, pairing) works, while Apple-specific features (hardware advertising
  buffers, Continuity/multi-advertising via VSC, coex, power telemetry) degrade or log errors.
* Opcode clash (important): QCA uses OGF 0x3F for EDL/patch/NVM/BD_ADDR (0xFC00, 0xFC0B, 0xFC14, …). Apple's
  Broadcom VSCs must **never reach the WCN3990**. A forwarded Broadcom 0xFC0B/0xFC14-style command could write QCA
  NVM or configuration.

## 4. Recommendation

### Kernel (our XNU)
1. **Keep the WCN3990.** Power, UART, `crbtfw21.tlv` and `crnv21.bin` download, baud, and BD_ADDR through QCA's own
   VSC: use the loader's `mac-address-bluetooth0` (02:52:44:57:00:02), so it matches MG's BluetoothAddress.
2. **Register a kernel control `com.apple.uart.bluetooth`** (`ctl_register`):
   * flags: `CTL_FLAG_REG_SOCK_STREAM`, **not** `CTL_FLAG_PRIVILEGED`, because both daemons run as `mobile`;
   * `getopt`/`setopt` for `TIOCGETA` (0x40487413) and `TIOCSETA` (0x80487414) on `SYSPROTO_CONTROL`: store and
     return a termios, and ignore the baud (the kernel owns the real line rate);
   * data: H4 packets, host→kernel through `ctl_send` handling, kernel→host through `ctl_enqueuedata`. One client at
     a time is enough (BlueTool and bluetoothd do not overlap).
3. **An HCI filter between the socket and the UART.** It forwards standard HCI (OGF 0x01–0x08) and ACL/SCO/ISO
   unchanged. It answers every host OGF 0x3F command locally with Command Complete:
   * status 0 for a small allow-list (sleep mode, BD_ADDR write, baud, and any VSC whose failure later shows up as
     fatal), with zeroed return parameters of the expected length;
   * status 0x01 (Unknown HCI Command) for all others.

   It logs every intercepted opcode for the first tests, so the real list can be learnt. It also absorbs HCI_Reset
   storms (forwarding is fine: patches survive a reset on WCN399x, as Linux's hci_qca relies on) and does the QCA
   wake byte or IBS sleep itself.
4. **Satisfy the gate cheaply**: a devfs node `/dev/uart.bluetooth`, which is only `access()`-checked. Optionally
   `/dev/btwake`, `/dev/btreset` and `/dev/btpoweroff` as harmless no-op nodes. A DT `bluetooth` node is only needed
   if the environment variable is not used: `transport-encoding` 1, `transport-speed`, and `local-mac-address`
   (6 bytes). It also has to be a *registered IOService*, since IOServiceNameMatching does not see plain DT entries.
5. Later, if wanted: `com.apple.uart.sk.bluetooth` with a Skywalk user-pipe nexus. That would run bluetoothd
   unmodified, with no environment variable. It means a lot more work: the 12-byte config option 0x800c5414 and the
   UUID option 0x40105412 are undocumented.

### Userland (port's own job files; no Apple binary patched)
* `mkjobs.py`: remove `bluetooth` from DENY, at least for `com.apple.bluetoothd` and `com.apple.bluetoothuserd`. Add
  to bluetoothd: `EnvironmentVariables: BT_HCI_TRANSPORT = "H4:POSIX:com.apple.uart.bluetooth,3000000"` (the baud is
  cosmetic). Optional: `OS_ACTIVITY_DT_MODE` for the first runs.
* **BlueTool shim**: a tiny daemon registered as Mach service `com.apple.BlueTool` that replies
  `{result: 0, firmware: "QCA WCN3990 <ver>", manufacturer: 29}` to RunCommand boot/init/deepsleep. Check the exact
  reply shape (top level vs inside `kMsgArgs`) against bluetoothd 0x1001d56d0/0x1002722c8 before writing it.
  * Alternative: keep the real BlueTool with `BT_UART=com.apple.uart.bluetooth` for diagnostics only.
  * A shim answer of manufacturer 15 might make later code take Broadcom paths. Unknown: start with 29.

### Risks and unknowns
* **The MG product-type → chipset match for this port is not verified** (§2.3). If it does not match, bluetoothd
  faults "Unknown platform" and probably crashes.
* bluetoothd's tolerance of failed Broadcom VSCs: some are ignored by design, the rest must be found by testing.
  Apple LE features (advertising instances via VSC) will probably not work. Plain LE scanning, classic pairing and
  HID are the realistic first goals. Audio (SCO/A2DP) is separate: btfmslim/SLIMbus vs Apple's PCM VSCs.
* Whether H4 (POSIX) adds H4BC-style BT_WAKE/sleep handling that expects `/dev/btwake` semantics: provide the node.
* The XPC reply format for the shim is inferred from strings (`kMsgId`, `kMsgArgs`, `result`, `firmware`,
  `manufacturer`), not traced end to end.
* The iPhone X device tree (DeviceTree.d22ap) was not available here: its real `bluetooth` node (transport-encoding,
  speed, calibration properties) could not be checked. iOS 16's default bluetoothd device is the Skywalk kctl,
  whatever the DT says.
