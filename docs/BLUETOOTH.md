# Bluetooth: a Qualcomm controller under iOS's own bluetoothd

What the S10 Lite (SM7150 / WCN3990) needed, Tests 244–262 of the port (2026-11-18/19), as a method for other phones.
iOS 16.5 (20F66, iPhone10,3) is the donor userland: its bluetoothd expects Apple's Broadcom BCM4355C0 on a UART.
No Apple binary is patched.

The reference code is the port's kernel overlay `bsd/dev/redwood_wifi.c`: `bt_stage1`–`bt_stage4`, `bt_fake` and
`bt_host_packet`. The step-by-step test log is `wifi/BLUETOOTH-PLAN.md`, and the reverse-engineering notes on
bluetoothd and BlueTool are in `wifi/BLUETOOTH-IOS-RESEARCH.md`.

## 1. The kernel owns the chip (stages 1–3)

Follow Linux, from the phone's device tree (`qcom,wcn3990-bt` and its serial engine):

| What | S10 Lite value | Linux source |
| --- | --- | --- |
| UART | QUP serial engine 13 (SE 0xc8c000, wrapper 2 at 0xcc0000, **32x oversampling** on wrapper v1.3); pins GPIO43–46, function qup13 | `qcom_geni_serial.c` |
| Baud clock | the SE's RCG with DFS off, set by hand: 7.3728 MHz from GPLL0-even with M/N 384/15625. Divider = 7372800 / (baud × 32) | `clk-rcg2.c`, `gcc-sm7150.c` |
| Engine | FIFO mode, polled, packing 0x4380e / 0xc3e0e, 8N1, RFR by hardware after the first exchange | `qcom_geni_serial.c` |
| Power-on (WCN3990) | the supplies are already voted by Wi-Fi. Then 0xc0 at 2400 baud, 0xfc at 115200, wait 100 ms | `hci_qca.c` `qca_wcn3990_init` |
| Firmware | `crbtfw*.tlv` (ROM patch) and `crnv*.bin` (NVM) from the vendor partition's `/firmware`, as HCI 0xfc00 sub 0x1e in 243-byte segments. Patch download mode 3 sends no acknowledgements. Then HCI_Reset | `btqca.c` `qca_download_firmware` |
| Check | the QCA version event's patch version changes (0x6699 → 0x0002 here) | |
| BD_ADDR | QCA 0xfc14 with 6 bytes, most significant first | `btqca.c` `qca_set_bdaddr` |

Two lessons from the kernel side:

- **XNU's I/O submap is only 8 MB**, and `ml_io_map` waits for ever when it is full (`KMA_NOFAIL`). A port maps
  far more than an iPhone does, so the first Bluetooth mappings hung. The system watchdog then ended the run,
  which looked like a hardware "fence". Fix: `IO_MAP_SIZE` 64 MB (`osfmk/arm/io_map.c`). The new log line reports
  how much of it is in use.
- The vendor firmware is never put in a repository. The root build copies it from a local `fw-bt` folder.

## 2. The kernel hands it to iOS (stage 4)

| Piece | Why |
| --- | --- |
| kctl `com.apple.uart.bluetooth`: SOCK_STREAM, **not** privileged (bluetoothd runs as `mobile`). Socket options TIOCGETA 0x40487413 and TIOCSETA 0x80487414 store and return a termios; the line itself is not changed | bluetoothd's POSIX UART driver. `BT_HCI_TRANSPORT=H4:POSIX:com.apple.uart.bluetooth,115200` in its job selects it, instead of the Skywalk channel an iPhone uses |
| One thread owns the UART: it writes the queued host packets and frames the controller's bytes into whole H4 packets (QCA in-band-sleep bytes 0xfd/0xfe/0xfc handled) | `ctl_enqueuedata` must get whole packets |
| `/dev/uart.bluetooth` (and `/dev/btwake`) | bluetoothd's start-up gate only `access()`es the node |
| **An IOService named `bluetooth`** under IOResources, with `local-mac-address` (6 bytes) | MobileBluetooth's `BTSessionAttachWithQueue` returns **error 9** without a registered service matching `IOServiceNameMatching("bluetooth")` (or `marconi-bt`). Without it BluetoothManager is nil and Settings > Bluetooth is greyed out. bluetoothd takes its address from the same property; with no address it logs "Not a valid MAC address (all 00s)" and exits 0 |
| HCI filter, host → controller | below |

### The HCI filter

1. **Vendor commands (OGF 0x3f) never reach the QCA chip.** bluetoothd sends Apple's Broadcom ones, because it
   picks the chip from the model and believes it has a BCM4355C0. On a QCA chip 0xfc00–0xfc1f are the
   patch/NVM/EDL commands.
   - **Answer each one in the kernel with a Command Complete: status 0, the first parameter byte (the
     sub-command of the "Olympic"/LE_META meta commands) repeated, then 8 zero bytes** (`bt_vsc=3`).
   - bluetoothd's reply handler drops a reply whose data after the status is **≤ 1 byte** ("Invalid data size").
     The command then times out, and BD_VSC_OLYMPIC_ANTENNA_PREF_B's timeout restarts the whole stack.
   - Status 0x01 alone is accepted by most of them ("status 701"), but not by that one.
2. **Standard commands the chip lacks but the stack insists on** are answered in the kernel, and their bits are set
   in the controller's Read_Local_Supported_Commands reply as it passes (`bt_fake`).
   - On the WCN3990 these are Read/Write_Default_Erroneous_Data_Reporting (0x0c5a / 0x0c5b, octet 18 bits 2 and 3).
   - Without them: "OI_HCICMD_WriteDefaultErroneousDataReporting (status=628)", "HCI initialization failed".
   - The stack decides from the mask and never sends the command.
   - LE_Set_Default_PHY failing (0x01) is tolerated.

## 3. Userland: the port's own launchd jobs

- `mkjobs.py` **JOBENV** (`pipeline/patch_mkjobs.py`) gives bluetoothd `BT_HCI_TRANSPORT`. `ALLOW` admits
  `bluetooth` jobs, which are on demand.
- **BlueTool stand-in** `wifi/r5q_bluetool.c`. bluetoothd sends XPC `kMsgId = RunBuiltinScript` for "boot" and
  "init" to `com.apple.BlueTool`. The stand-in answers `result 0`, `firmware`, `manufacturer 29`; the real BlueTool
  only knows the Broadcom firmware compiled into it.
- **The stand-in's job must not be labelled `com.apple.*`.** iOS's launchd panics the whole system ("userspace
  panic: unexpected exec of non-platform binary") when a com.apple job runs a program that is not Apple's.
  mkjobs.py **RELABEL** names it `com.redwood.BlueTool`; its Mach service stays `com.apple.BlueTool`.
- Optional: `OSLOG` for bluetoothd sends its os_log to the console. That is the fastest way to read each failure.

## 4. Order of failures seen, and the fix for each

| Test | Symptom | Fix |
| --- | --- | --- |
| 245–252 | Bluetooth mapping "hangs", then the system ends | I/O submap 64 MB |
| 256 | bluetoothd `exit(0)` after "Not a valid MAC address" | an address: the IOService's `local-mac-address` (or the boot-arg `wlan.debug.generate-mac`). `BT_DEVICE_ADDRESS` is ignored by retail builds |
| 257 | panic: non-platform binary for com.apple.BlueTool | RELABEL |
| 258 | "HCI initialization failed" (erroneous data reporting) | `bt_fake` + supported-commands bits |
| 259–260 | ANTENNA_PREF_B "Invalid data size", timeout, restart | vendor replies with ≥ 2 data bytes |
| 261 | Settings > Bluetooth greyed (BluetoothManager nil, lastInitError 9) | IOService `bluetooth` |

## 5. Tools

- `wifi/bt_test.m` → `/var/r5q/bin/bt_test [on|off|scan|le|methods]`. It reports BluetoothManager's and
  CoreBluetooth's state, scans, and lists BluetoothManager's methods together with `+lastInitError`.
  - An unsigned tool gets CoreBluetooth, but not BluetoothManager's MBFramework messages ("insufficient
    entitlements").
- The kernel log (`bt: stage 4: host -> / controller -> ...`) records each of the first four clients' HCI
  exchanges in full, with counters every 500 packets.

## 6. Not done yet

- Pairing and connections: classic discovery runs, but nothing nearby was in pairing mode yet. ACL data, SCO and
  audio (A2DP over the chip's SLIMbus/PCM) are untested.
- (Fixed: the local name. bluetoothd reads it through SystemConfiguration (`SCDynamicStoreCopyComputerName`), from
  `preferences.plist` System > System > ComputerName, which lockdownd writes on an iPhone. Without it Settings
  says "(null)". `wifi/sc_name.c` sets it; on a port whose /var is not persistent, save it afterwards.)

(Done since: the line runs at **3 Mbit/s**, with `bt_baud=3000000`, Test 263. The engine clock goes to 96 MHz,
then the chip gets QCA EDL_SET_BAUDRATE 0xfc48 index 14 with RTS held, as Linux `qca_set_speed` does. Above
230400 the 7.3728 MHz clock cannot divide down at 32x oversampling.)
