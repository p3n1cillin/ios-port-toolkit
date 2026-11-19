# Bluetooth on the S10 Lite (r5q): plan

Started 2026-11-18. Read-only survey done; no Bluetooth code written yet. Each stage below is one test (or a few),
recorded here as it runs, like WIFI-PLAN.md.

## What the hardware is (from `live.dts` and the vendor image)

| What | Value | Where |
| --- | --- | --- |
| Controller | Qualcomm **WCN3990**-family BT (the same combo chip as Wi-Fi), `compatible = "qca,wcn3990"` | `bt_wcn3990` node |
| Supplies | core 1.8 V **PM8150 L7**, PA 1.3 V **PM8150L L2**, LDO 3.3 V **PM8150L L11** (RPMh VRMs: votes, not PMIC writes) | `qca,bt-vdd-*-supply` → `regulator-pm8150-l7`, `regulator-pm8150l-l2`, `regulator-pm8150l-l11` |
| HCI transport | high-speed UART, QUP SE at **0xc8c000** (wrapper 2), `status = "ok"`, `qcom,wakeup-byte = 0xfd`, clocks GCC 0x73 + wrapper-2 AHB 0x7d/0x7e, pins `<0x396 0x397 0x398>` (TX, RX, CTS/RTS) | `qcom,qup_uart@0xc8c000` |
| (another HS UART) | 0xa90000, always-on clock: not Bluetooth (to identify before touching) | `qcom,qup_hsuart@0xa90000` |
| BT audio (SCO/A2DP offload) | its own SLIMbus at 0x17240000 (`btfmslim_slave`, ifd 00 00 20 02 17 02): not needed for data | `slim@17240000/wcn3990` |
| Firmware | `/vendor/firmware/crbtfw21.tlv` (214 428 bytes, the ROM patch) and `crnv21.bin` (4 747 bytes, NVM) — what Linux's `btqca` loads for WCN3990 v2.1 (`apbtfw11.tlv` is for another chip) | vendor image (local copy in `work-sensors/vendor.img`; **never published**) |

## Stages

**Stage 1 — power and the UART (kernel, `bt=1`).**

1. Vote the three supplies through RPMh (`redwood_r5q_rpmh_enable`, as `ldoc8` for the sensors). Their cmd-db names
   are probably `ldoa7`, `ldoc2`, `ldoc11`; check them in cmd-db first.
2. Read the serial engine's clocks (GCC CBCR) before touching it. Then read `GENI_IF_DISABLE_RO`: if the FIFO
   interface is disabled (as on the touch engine), the UART needs GSI DMA or bit-banging, and this plan changes.
3. Set the pins and the UART at 115200 8N1 with RTS/CTS.

**Stage 2 — HCI and the firmware (kernel), following Linux `drivers/bluetooth/btqca.c` + `hci_qca.c` for WCN3990:**

1. Power-on sequence: `qca_power_on` → the controller's power-on byte/pulse on the UART for WCN399x.
2. `HCI_Reset` → Command Complete.
3. The QCA vendor command for the version (EDL_PATCH_GETVER, 0xFC00). It should report the SoC and ROM version
   matching `crbtfw21`.
4. Download the `.tlv` patch, then the NVM, in segments (EDL_PATCH_TLV_REQ_CMD).
5. Set the baud rate to 3 Mbit/s (vendor command, then the UART).
6. `HCI_Reset` again, then `Read_BD_ADDR` and `Read_Local_Version`.

Stop there: the controller is up. The BD address comes from the NVM, or Samsung's own (EFS, never touched;
read-only if at all).

**Stage 3 — what iOS's bluetoothd needs (research, then a design).** iPhone X's Bluetooth is Apple/Broadcom over
UART. `bluetoothd` (with `BlueTool`) loads its own chip's firmware and expects Apple's transport. To answer, read-only
in the shared cache and `bluetoothd`:

- How does `bluetoothd` reach the controller (an IOKit transport, `/dev/uart.*`, or Apple's HCI transport
  service)?
- What does it send before standard HCI (vendor commands, firmware load), and can that step be skipped when the
  controller is already up?
- Which vendor commands does it require afterwards (coex, logging), and which can be answered from the kernel?

The likely design: the kernel keeps the controller (stage 2) and offers it to iOS as the transport `bluetoothd`
expects, answering or skipping Apple's chip-specific commands. That needs to be found out before any code.

**Stage 4 — pairing and use:** discovery in Settings, pairing a keyboard/headphones (data only; audio over
`btfmslim` is a later stage).

## Safety for this work

- **Supplies:** RPMh votes only (standing authorization covers power votes). No PMIC register writes.
- **UART:** the serial engine is only written after its clocks are confirmed on.
- **BD address:** never write the phone's own address to its storage; EFS stays untouched.
- **Firmware:** read from the vendor image as Wi-Fi's is; it stays local and is not published.

## Progress

- **Test 244 (kernel t244, root test243; boot-arg `bt=2`): stage 1.** Code: `bt_stage1` in
  `kernel-build/r5q-overlay/bsd/dev/redwood_wifi.c`, started from `redwood_wifi_init` when `bt` is set, 15 s into
  the boot. It reads:
  - the UART's GCC clocks: vote register 0x152014 (bits 7 S3, 2 M_AHB, 1 S_AHB) and CBCRs 0x11e4d4/0x11e004/0x11e008;
  - the baud clock's RCG at 0x11e4d8;
  - GPIO43–46 (east tile);
  - the cmd-db addresses of `ldoa7`, `ldoc2` and `ldoc11`.

  With `bt=2` it then votes the three supplies, votes the three clocks, and reads the engine at 0xc8c000:
  firmware/protocol (+0x68), IF_DISABLE (+0x64), status (+0x40), DMA mode (+0x258), HW params (+0xe24/+0xe28) and
  clock config (+0x48). Questions it answers: is the FIFO interface there (FIFO UART possible) or not (GSI DMA or
  bit-bang); is the engine's firmware the UART protocol; are the supplies' names right.
- **Results, Tests 244-246 (2026-11-18):**
  - **Supplies:** the Wi-Fi driver's rails code already votes `ldoa7` ("vdd-1.8-xo", PM8150 L7), `ldoc2`
    ("vdd-1.3-rfa", PM8150L L2) and `ldoc11` ("vdd-3.3-ch0", PM8150L L11) at 0x41f00/0x41d00/0x41a00. The WCN3990's
    Bluetooth shares Wi-Fi's supplies, so they are on whenever Wi-Fi is.
  - **Test 244** printed nothing. **Test 245** stopped after "mapping": its large mappings covered GCC, the east tile
    from GPIO0 to 46, and cmd-db.
  - **Test 246** (single pages) read the clocks:
    - vote register 0x152014 = 0xf: S3 (bit 7) off, the wrapper-2 AHB clocks on;
    - CBCR S3 0x80004220 (off), M_AHB 0 (on), S_AHB 0x80000002;
    - the baud clock's RCG: cmd 0x80000000, cfg 0x100000 (root off).

    Then **the first access to GPIO43's page (TLMM east tile, 0x0352b000) ended the system** ("psci: cpu 1 ends the
    system: the return to the boot loader"). The phone stopped in the loader's maintenance mode, and iOS was
    restarted without `bt`. The touch driver reads GPIO87 in the same east tile without trouble, so it is these
    pins (or their pages) that are refused: protected by the hypervisor or TrustZone for HLOS? Not explained yet.
    **Rule until it is: never touch GPIO43-46 from our kernel.**
  - **Next for stage 1:**
    - find out whether the bootloader already sets those pins to qup13 (then nothing needs writing), by reading
      them from Android, or from UEFI (Mu-Silicium's `PinctrlDxe`, if any);
    - read the serial engine itself (0xc8c000) once its S3 clock is voted, then check whether its page is reachable,
      on one page only, said first.
- **iOS side, researched (no phone time; full notes with addresses in `wifi/BLUETOOTH-IOS-RESEARCH.md`):**
  - iPhone X's Bluetooth is a Broadcom BCM4355C0 on a UART speaking H4. bluetoothd and BlueTool talk to it through
    a **kernel control socket** (`PF_SYSTEM`/`SYSPROTO_CONTROL`):
    - BlueTool uses `com.apple.uart.bluetooth`;
    - bluetoothd uses `com.apple.uart.sk.bluetooth` with Skywalk by default, or a plain kctl named by env
      `BT_HCI_TRANSPORT=H4:POSIX:<name>,<baud>`.
  - bluetoothd only starts if `/dev/uart.bluetooth` (or `/dev/cu.bluetooth`, or an IOService named `bluetooth`)
    exists; otherwise it exits 0.
  - BlueTool loads Broadcom firmware compiled into itself (no skip path in retail builds). bluetoothd sends Broadcom
    vendor commands (OGF 0x3F). These must never reach the Qualcomm chip, which uses the same OGF for patch/NVM/BD_ADDR.
  - **Design:**
    - the kernel brings the WCN3990 up itself, then registers kctl `com.apple.uart.bluetooth` carrying H4, behind
      an HCI filter: standard commands and data pass, every 0x3F command is answered locally (a few with success,
      the rest "unknown command") and logged;
    - `/dev/uart.bluetooth` is created to pass the gate;
    - in userland, bluetoothd is allowed in `mkjobs.py` with `BT_HCI_TRANSPORT`, and a small `com.apple.BlueTool`
      XPC stand-in answers `boot`/`init` with result 0.
  - **Unknowns:**
    - the MobileGestalt product hash bluetoothd matches;
    - the exact XPC reply;
    - which refused 0x3F commands are fatal;
    - LE advertising features will likely be degraded. First goals: scanning, classic pairing, HID.
- **Test 247 (kernel t247, `bt=3`: pins skipped, straight to the serial engine):** the engine's clock vote works:
  vote register 0xf → 0x8f, CBCR S3 0x80004220 → 0x4220 (on); M_AHB on; S_AHB reads 0x80000002 (hardware-gated).
  Then **mapping/reading the engine's page 0xc8c000 ended the system** again ("psci: cpu 1 ends the system"). That
  is right after "mapping the engine's first page"; the mapping itself does not touch hardware, so either the first
  access or something set off by the S3 vote.
  - **Conclusion so far:** the Bluetooth UART's pins (GPIO43-46) and its serial engine (QUP wrapper 2 SE3) are
    refused to our kernel. Their neighbours are not: touch reads GPIO87 in the same tile, and other QUP engines are
    used.
  - **Most likely cause:** the hypervisor's or TrustZone's access control has them assigned away from HLOS on this
    boot path. Android's own path (ABL → Linux) may hand them over, or Android may reach them in a way we don't.
  - **Not to be touched again** until that is understood: every attempt costs a restart, and the system end is
    taken by the loader.
  - **Next, read-only:**
    1. Android's boot log: is the HS UART at 0xc8c000 probed, and is `hci_qca`/`btfm` started?
    2. The TrustZone/hypervisor device configuration (the devcfg image) for the access lists of QUP wrapper 2 and
       those TLMM pins.
    3. Whether Mu-Silicium's UEFI or ABL does an SCM call to assign them, which we skip.
    4. Whether a sibling engine on wrapper 2 (one HLOS clearly owns) can be read without ending the system, which
       would separate "wrapper 2 is fenced" from "this engine is fenced".
- **Android's boot log** (outputs/.../test67/.../firmware-log.txt, read-only) shows Android reaches this hardware
  normally:
  - `bt_power vendor:bt_wcn3990` powers the chip from the HAL (`bt_ioctl: BT_CMD_PWR_CTRL`);
  - the HS UART driver dumps the engine's registers (`geni_status`, `geni_ios`, `dma_rxlen 0x800`), so it runs in
    **DMA mode**;
  - `BT_CMD_CHIP_VERS soc_version: 0x40010224`.

  So HLOS can reach SE 0xc8c000 and GPIO43-46 on Android's boot path, and something on that path (ABL, Linux's
  QUP/GENI set-up, an SCM call) is missing from ours. That is the question to answer before any more Bluetooth
  tests.
- **Test 248 (kernel t248, `bt=4`, 2026-11-19): what is fenced?**
  - The probe was to read GPIO47 (same east tile, SE14's pin), then SE12 (0xc88000, an I2C on the same wrapper that
    Android uses) with its clock on.
  - It ended the system right after "GPIO47: mapping"; nothing after that line reached the log.
  - The loader's record of the run (read from the log slot in the listening window): `TEST248 XNU RETURN started=1
    reason=0`. No panic message: the kernel did not panic. The likely end is a CPU stalled on a bus access with our
    buffered log lines lost, and the return taken by the heartbeat/watchdog path ("psci: cpu 2 ends the system").
  - So the low pages of the east tile (GPIO43, GPIO47, probably GPIO43-49: SE13's and SE14's pins) and SE13 are
    fenced: an access-control (XPU) violation is the likely cause. GPIO87 in the same tile, which touch uses, is
    not. SE12 was never reached.
  - **Bluetooth parked here.** Android reaches all of it, so its boot path (Samsung ABL, Linux) opens it in some
    way, maybe an SCM call. To resume:
    1. find that call in Samsung's kernel source for SM-G770F (`drivers/soc/qcom`, `msm_geni_serial`, `btpower`,
       `qcom_scm` users) or in ABL;
    2. or test whether the loader (EL1 in UEFI, before XNU) can read GPIO47. If UEFI can and XNU cannot, the fence
       is set after UEFI.
- **USB recovery without a cable (2026-11-19):** when the loader's USB link sticks ("no link"),
  `wifi/r5q_reboot.c` (reboot(2) over r5qd, as root) restarts the phone from iOS. The loader brings USB up again.
  `phone.py reboot` then reaches the listening window, where `phone.py logs` can read the log slots.

## The "fence" was our kernel: XNU's 8 MB I/O submap (Tests 251-252, 2026-11-19) -- Bluetooth UNBLOCKED

- **Test 251 (`bt=5`):** map GPIO47's page, a line, 2 s, a line, only then read. Only "mapping" was ever printed:
  **`ml_io_map` itself never returned**. No read had happened, so there was no hardware fence.
- **Cause:** `osfmk/arm/io_map.c` gives every `ml_io_map()` its kernel addresses from an `io_submap` of
  `IO_MAP_SIZE` = **8 MB**, with `KMA_NOFAIL`. When it is full, the call waits for ever (and the system's watchdog
  then ends the run, reason 0, no panic). The port's drivers map far more device memory than an iPhone does. The
  same thing explains the early note "mapping GCC as 0x100000+0x1f0000 hung the thread silently".
  `ml_io_map_unmappable` (kernel_map) was never affected.
- **Fix (Test 252, kernel t252):** `kernel-build/r5q-overlay/osfmk/arm/io_map.c` with **IO_MAP_SIZE 64 MB**, plus a
  usage counter printed at each 1 MB mark ("io_map: N KB of the 64 MB I/O submap in use"). The same `bt=5` probe:
  "MAPPED ... still alive 2 s after the mapping ... GPIO47 cfg 0x1: READ FINE".
- **Stage 1 (`bt=2`) on t252:**
  - GPIO43-46: cfg 0x1/0x1/0x3/0x1. **Function 0 (plain GPIO), not qup13**: the bootloader leaves them unmuxed,
    TX (45) high.
  - Supplies `ldoa7`/`ldoc2`/`ldoc11` voted.
  - Clock vote 0xf → 0x8f; CBCR S3 0x4220 (on).
  - **SE 0xc8c000:** firmware 0x20b (**protocol 2 = UART**), **IF_DISABLE 0 (the FIFO interface IS there)**, status 0,
    DMA mode 0, HW params 0x20102864 / 0x20204900, clock cfg 0x11.
- **Tool fix:** `phone.py cycle`/`restart` merge every ring snapshot into `live.txt` (`merge_log`). Only the last
  snapshot used to be kept, so early kernel lines were lost once userland filled the 2 MB ring; Test 252's first
  `live.txt` had none of its kernel lines.
- (The Samsung-source research, `samsung-r5q-source-research.md` in the session notes, found no TZ/SCM grant and no
  protected pins in 43-49 on r5q. The sibling SM8150 phone S10+ 5G runs Bluetooth on this SE under mainline Linux.)

### Stage 2 plan (from Linux qcom_geni_serial.c + hci_qca.c, WCN3990)

1. Pins 43-46 to function 1 (qup13), with the DT's drive/pulls: CTS/RX 2 mA no pull, RTS pull-down, TX pull-up.
2. The baud clock (RCG 0x1e4d8, CMD/CFG/M/N/D): XO 19.2 MHz first (2400 baud for the power pulse: divider 500,
   exact), then 7.3728 MHz from GPLL0 even (M/N 384/15625) for 115200.
3. GENI UART in FIFO mode: packing, word length 8, stop bit, M_CMD start TX, S_CMD start RX, FIFO watermarks.
4. WCN3990 power-on as hci_qca: 2400 baud, the power-off byte 0xC0, then the power-on byte 0xFC; 100 ms; 115200.
   Then HCI_Reset, and the QCA version command (EDL_PATCH_GETVER).

## Tests 253-254 (2026-11-19): THE BLUETOOTH CONTROLLER ANSWERS HCI

- **Test 253 (`bt=6`, read only):**
  - QUP wrapper 2 version 0x10030000 (v1.3: **32x** UART oversampling, as Linux for < 2.5);
  - the S3 RCG's DFS is **off** (cmd+0x14 = 0x8020, bit 0 clear), its 8 levels empty; the RCG runs from XO
    (cfg 0x100000: source 0, no divider, bit 20 hardware control);
  - SE3 CLK_SEL 0, M_CLK_CFG 0x11.
- **Test 254 (`bt=7`, `bt_stage2` in redwood_wifi.c): GENI UART polled in FIFO mode + hci_qca's WCN3990 power-on:**
  - **RCG to 7.3728 MHz:** GPLL0 even (source 6), div field 1, M 0x180, N = D = ~(15625-384) = 0xc476, dual edge
    → cfg 0x102601. Dividers: 2400 baud = 96, 115200 = 2.
  - **Pins:** GPIO43/44/45/46 to 0x4/0x5/0x7/0x4 (qup13; CTS/RX no pull, RTS pull-down, TX pull-up, 2 mA).
  - **Engine:** CGC on, DMA CGC bits, output ctrl 0x7f, force default; IRQ_EN M+S, DMA mode off, GSI events off;
    RX watermark 2, RFR 30; packing 0x4380e/0xc3e0e (8 bits x 4, LSB first); 8N1; stale count 160;
    S_CMD UART_START_READ. Status 0x41000.
  - **Power-on:** 0xc0 at 2400 (CTS ignored, RFR held) → 115200 → 0xfc → 100 ms (the controller sent `00 00`)
    → flow control on.
  - **`HCI_Reset` → `04 0e 04 01 03 0c 00`** (Command Complete, status 0).
  - **QCA EDL_PATCH_GETVER (`01 00 fc 01 19`) → `04 ff 0e 00 02 0a 00 00 00 99 66 01 10 24 02 01 40` +
    `04 0e 04 01 00 00 00`**: SoC id **0x40010224** (= Android's `BT_CMD_CHIP_VERS soc_version`).
  - **`Read_Local_Version` → `04 0e 0c 01 01 10 00 08 00 00 08 1d 00 be 02`**: HCI 8 (Bluetooth 4.2), LMP 8,
    manufacturer **0x001d (Qualcomm)**, subversion 0x02be. It runs from ROM, no patch yet.
- **Next:**
  - stage 3: download `crbtfw21.tlv` (ROM patch) and `crnv21.bin` (NVM) as btqca.c (TLV segments by
    EDL_PATCH_TLV_REQ_CMD), then 3 Mbit/s;
  - stage 4: present it to iOS (kctl `com.apple.uart.bluetooth`, the HCI filter, `/dev/uart.bluetooth`, BlueTool
    stand-in) as in BLUETOOTH-IOS-RESEARCH.md.
- **Test 255 (kernel t255, root test255 with ../fw-bt in /fw; `bt=8` = stage 2 + 3): THE FIRMWARE LOADS.**
  - **`crbtfw21.tlv`** (214 428 bytes, TLV type 1, patch header download mode **3** = no events): 883 segments of
    243 bytes, each `01 00 fc <len+2> 1e <len> <bytes>`, none answered (as mode 3 says).
  - **`crnv21.bin`** (4747 bytes, TLV type **4**): 20 segments, each answered by a vendor event + command complete:
    `04 ff 03 00 04 00|10` + `04 0e 04 01 00 00 00`. Result byte 00 for the first, 0x10 for the rest: meaning not
    known (btqca would call non-zero an error?), but the controller works after it.
  - The NVM's HCI tag (17) was not found by the tag walk (type 4 layout?): its baud byte was not changed, and the
    link stayed at 115200 all the same.
  - **After `HCI_Reset` (OK):** the QCA version event's patch version **0x6699 → 0x0002** and its ROM field
    **0x1001 → 0x0201** (SoC 0x40010224): **the ROM patch is applied.** Read_Local_Version is unchanged (HCI 8,
    Qualcomm, subversion 0x02be). iOS ran on.
  - The root image now also has the Location Services fix (inject_app.c), and the root build copies ../fw-bt
    (pipeline/build_root_app.sh adds that step to build.sh).
- **Next, stage 4: give it to iOS** (BLUETOOTH-IOS-RESEARCH.md):
  - a kernel thread that owns the UART (RX into a ring);
  - kctl `com.apple.uart.bluetooth` (H4 stream, termios socket options) with the HCI filter (0x3F commands answered
    locally);
  - `/dev/uart.bluetooth`;
  - bluetoothd allowed in mkjobs with `BT_HCI_TRANSPORT=H4:POSIX:com.apple.uart.bluetooth,115200`;
  - a BlueTool XPC stand-in.
- **Test 256 (kernel t256, root test256; `bt=9` = stages 2 + 3 + 4): the controller to iOS.** What was built:
  - **Kernel (`bt_stage4` in redwood_wifi.c):**
    - kctl `com.apple.uart.bluetooth`: SOCK_STREAM, not privileged, 64 KB buffers;
    - TIOCSETA/TIOCGETA socket options store and return a termios; the line itself is not changed;
    - nodes `/dev/uart.bluetooth` (bluetoothd only `access()`es it) and `/dev/btwake` (writes dropped);
    - one thread (`bt_pump` loop) owns the UART: it sends the queued host packets and frames the controller's bytes
      into whole H4 packets for the client. QCA in-band-sleep bytes are handled: 0xfd is answered 0xfc, and
      0xfe/0xfc are dropped.
  - **HCI filter:** the host's stream is framed too. Every vendor command (OGF 0x3f) is answered in the kernel with a
    Command Complete and never reaches the WCN3990, where 0xfc00–0xfc1f are patch/NVM commands. `bt_vsc` sets the
    answer:

    | `bt_vsc` | Answer |
    | --- | --- |
    | 0 | every vendor command: status 0x01 (unknown command) |
    | 1 (default) | success for 0xfc01 BD_ADDR, 0xfc18 baud, 0xfc1c/0xfc1e PCM, 0xfc27 sleep mode, 0xfc45, 0xfc6e; 0x01 for the rest |
    | 2 | every vendor command: success, with 8 zero bytes of return parameters |

    The first 120 vendor commands, and the first 80 packets each way, are logged.
  - **Userland (`pipeline/build_root_app.sh`, `BT=1` default):**
    - `pipeline/patch_mkjobs.py` gives mkjobs.py a `JOBENV="label:VAR=value"` option;
    - bluetoothd gets `BT_HCI_TRANSPORT=H4:POSIX:com.apple.uart.bluetooth,115200` and `OS_ACTIVITY_DT_MODE`
      (its os_log goes to the console);
    - `ALLOW` takes in `bluetooth`;
    - `com.apple.BlueTool` is relocated to `/redwood/bin/r5q_bluetool` (`wifi/r5q_bluetool.c`). It answers every
      XPC message with `result 0`, manufacturer 29 and a firmware string, at the top level and in `kMsgArgs`, and
      logs each message.
  - **Result (run 20261119-014147):** stages 2 and 3 as in Test 255 (patch version 0x0002). Stage 4 came up: "THE
    CONTROLLER IS IOS'S". The jobs were right: bluetoothd had its JOBENV setting, and BlueTool was relocated.
    **But bluetoothd never connected.** launchd started it on demand every 10 s, and each time it ended within
    about 25 ms with `exit(0)`, after logging:
    ```
    could not find BT_DEVICE_ADDRESS env variable
    marconi-bt / bluetooth: Unable to match bluetooth service in device tree. ... Not a valid MAC address (all 00s).
    FindLocalMacAddress did not find bootargs for random address generation
    com.apple.server.bluetooth: Not a valid MAC address (all 00s).
    ```
  - **Why (bluetoothd main, 0x100026108–0x1000268c4):** the local address is a 6-byte global at 0x100847060. If it
    is all zeros at 0x100026364, bluetoothd logs and calls `exit(0)` at 0x1000268c4. Exit status 0 is a "successful
    exit", so launchd does not restart it. The sources it tries:
    1. **`BT_DEVICE_ADDRESS` (env):** it has to be 17 characters, and is parsed with `%02hhx:…`. **The retail build
       then zeroes the parsed bytes again** (0x100026168), so the variable is useless here.
    2. **The device tree:** IOServiceNameMatching `marconi-bt`, then `bluetooth`, `local-mac-address` (0x100027194).
       The port's loader tree has no such registered service.
    3. **`kern.bootargs`:** split on spaces. A token containing **`wlan.debug.generate-mac`** (0x100026ab4), or
       `wlan.factory=<hex>` with bit 2 set (0x100026b18), makes it get an address from 0x100025d30. That address is
       "stored to plist" and reused afterwards; it is parsed into the global and `setenv("BT_DEVICE_ADDRESS")`.
  - **Next (Test 257):** the same image with the boot-arg `wlan.debug.generate-mac=1`.
- **Test 257 (image 256 again, + boot-arg `wlan.debug.generate-mac=1`): bluetoothd starts — then a panic.**
  - The address problem is solved. The first bluetoothd logged "found wlan.debug.generate-mac" and "Storing local
    address to plist: 4b:d6:…"; the later ones logged "Local address from plist", the same address each time.
  - Before stage 4 was up (about 80 s after the kernel started), each start still ended with `exit(0)` at the
    `/dev/uart.bluetooth` gate.
  - The first start after stage 4 **ran on**:
    ```
    Searching for transport: "H4" / Searching for driver: "POSIX"
    ... Model: iPhone10,3 ... (the profiles registered: HFP, PBAP, HID, A2DP, AVRCP, MAP, LE Audio, GATT ...)
    Beginning asynchronous power init / Booting chipset / Opening XPC Connection com.apple.BlueTool
    ```
    No "Unknown platform" fault: the MG platform → chipset table was not a problem.
  - **launchd then started the stand-in, and the system panicked:** "userspace panic: unexpected exec of
    non-platform binary: /redwood/bin/r5q_bluetool: com.apple.BlueTool [194]". iOS's launchd refuses a com.apple.*
    job whose program is not an Apple platform binary (the port's own com.redwood.* jobs run unsigned programs).
    The run returned to the loader; the panic was found with `phone.py logs` (firmware ring), not in the cycle log.
  - **Fix (Test 258):** `RELABEL="com.apple.BlueTool=com.redwood.BlueTool"` (a new option in patch_mkjobs.py). The
    job is renamed and keeps its Mach service `com.apple.BlueTool`.
  - **Lesson for the toolkit:** a launchd job that runs a non-Apple program must not carry a `com.apple.` label.
- **Test 258 (kernel t256, root test258 with RELABEL; `bt=9 wlan.debug.generate-mac=1`): iOS's bluetoothd drives
  the WCN3990.**
  - The stand-in BlueTool ran as com.redwood.BlueTool, with no panic. bluetoothd's XPC message is
    **`kMsgId = RunBuiltinScript`** (not RunCommand), with two keys. Answered `result 0`, bluetoothd logged
    "BlueTool finished running boot script", then "Powering on chipset", then "... init script".
  - Then it connected to the kctl, set termios, and ran its HCI init over our UART. **Every command was answered by
    the QCA controller**, with no vendor command at all (`vendor commands answered here 0`):

    | Command | Answer |
    | --- | --- |
    | HCI_Reset 0c03 | 0 |
    | Read_Local_Version 1001 | HCI 8 (4.2), Qualcomm 0x1d, subversion 0x02be |
    | Read_Local_Supported_Commands 1002 | 64-byte mask |
    | Read_BD_ADDR 1009 | `ad 5a 00 00 00 00`, the chip's own default — not the generated one, since no Write_BD_ADDR was sent |
    | Read_Local_Features 1003, Read_Local_Extended_Features 1004 (page 2) | answered |
    | LE_Read_Local_Features 2003 | 0xff |
    | Set_Event_Mask 0c01, LE_Set_Event_Mask 2001 | 0 |
    | Write_Simple_Pairing_Mode 0c56, Write_LE_Host_Supported 0c6d | 0 |
    | LE_Read_Buffer_Size 2002 | 251 / 16 |
    | LE_Read_Max_Data_Length 202f | 251 / 17040 |
    | **LE_Set_Default_PHY 2031** | **0x01 (unknown)**: bluetoothd logged "Set default Phy failed STATUS 701", not fatal |
    | LE_Write_Suggested_Default_Data_Length 2024 | 0 |
    | LE_Read_Supported_States 201c | answered |
    | LE_Read_White_List_Size 200f | 0x80 |
    | Read_Buffer_Size 1005 | ACL 1024 x 7 |
    | Host_Buffer_Size 0c33, Set_Controller_To_Host_Flow_Control 0c31 | 0 |
  - **Then:** "OI_HCICMD_WriteDefaultErroneousDataReporting (status=628) / HCI initialization failed / Bluetooth
    error - restarting reason=108". The command was never sent. The stack read the supported-commands mask:
    octet 18 = 0x83, so bit 3 (Write_Default_Erroneous_Data_Reporting, Linux `commands[18] & 0x08`) was clear,
    and it treats that as fatal. bluetoothd then exited and was started again every 10 s, the same each time.
  - **Fix (kernel t259):** a table of standard commands answered in the kernel (`bt_fake`):
    Read/Write_Default_Erroneous_Data_Reporting 0c5a/0c5b, octet 18 bits 2/3. Their bits are set in the
    controller's Read_Local_Supported_Commands answer as it passes. The first four clients' exchanges are now
    logged in full.
- **Test 259 (kernel t259 with `bt_fake`): HCI init passes, and bluetoothd's stack starts.**
  - The stack now sends Write_Default_Erroneous_Data_Reporting (answered OK in the kernel) and goes on to its
    Broadcom vendor commands. bluetoothd's own names for them, in order:
    HDR_CONTROL, BTCLK_SENSORC_TIMESYNC, READ_PROP_LOCAL_FEATURES, OLYMPIC_SET_DIAG_DEBUG_EVENT_BEHAVIOR,
    OLYMPIC_SET_EXT_SCAN_LIMITS / _BUFFERS, OLYMPIC_SET_EXT_ADV_LIMITS, LE_META_ENA_FEATURES,
    OLYMPIC_CHANGE_LE_SETTINGS, OLYMPIC_SET_MIN_AFH_LE_CHANNELS, ENABLE_EXTENDED_DUPLICATE_FILTER.
    Seven of them reached the kernel (0xfce9, 0xfce2 sub 0x0b/0x1c/0x15, 0xfd48, 0xfd47); the rest bluetoothd
    settled itself. All seven got 0x01 back, and most were taken as "completed with status 701" (701 is the stack's
    code for an unknown command).
  - **"Stack did start"** (L2CAP, ATT MTU 527, "This is a phone").
  - **Then BD_VSC_OLYMPIC_ANTENNA_PREF_B** (0xfce2, sub-command 0x15): "Invalid data size 0 (status=65535)", then
    10 s later "timedout waiting for controller", "Bluetooth error - restarting reason=3602", and `exit(1)`, every
    16 s.
- **Test 260 (kernel t260, `bt_vsc=4`: the sub-command repeated after the status): "Invalid data size 1"**, the same
  timeout.
  - **The rule** (bluetoothd 0x100125f60 and 0x1000adc70, two copies of one reply handler): the reply's data, after
    the status, must be **more than 1 byte**. Otherwise it logs "Invalid data size %d" and never calls the
    command's callback, so the command times out and the stack restarts. With 2 bytes or more, the callback gets
    the status and the data.
- **Test 261 (image 260, `bt_vsc=3`):** every vendor command answered "success", with the sub-command repeated and
  8 zero bytes.
  - **Result: BLUETOOTH RUNS IN iOS.** Every vendor command now "completes with status 0", including ANTENNA_PREF_B,
    OP_DIVERSITY_AFH_MAPS, FACTORY_CAL_READ_TABLE, SET_SAR_LOCATION and the LE_META advertising filters. The stack
    starts and **bluetoothd stays up** (no restart in over 10 minutes).
  - **LE scanning works end to end.** The kernel log shows LE Advertising Report events (`04 3e … 02 01 …`) from
    devices around the phone. bluetoothd parses them, and its LE caches fill. (The names of nearby devices stay in
    the local cycle logs only.)
  - CoreBluetooth reports **state 5 (powered on)** to a client (`bt_test`, below). bluetoothd tells CoreBluetooth
    sessions "state updated: On". The Settings main list shows **"Bluetooth: On"**.
  - **But Settings > Bluetooth shows its switch greyed out and off.** That page uses BluetoothManager.framework, and
    `+[BluetoothManager sharedInstance]` is nil: `+lastInitError` is **9**. backboardd logs "BTSessionAttachWithQueue:9"
    as well.
  - **Why (MobileBluetooth `BTSessionAttachWithQueue`, cache 0x1b089d88c):** the call returns 9 before it sends
    anything, unless two checks pass (two MG-style answers; the second must be false) **and
    `IOServiceGetMatchingService(IOServiceNameMatching("marconi-bt"))` or `("bluetooth")` finds a registered
    service.** The port has no such IOService: bluetoothd got past its own copy of this check only through the
    `/dev/uart.bluetooth` node and `BT_HCI_TRANSPORT`. CoreBluetooth's XPC path does not check.
  - **New tool `wifi/bt_test.m`** (in `build_phone_tools.sh`), run as `/var/r5q/bin/bt_test [on|off|scan|le|methods]`.
    It shows BluetoothManager's available / enabled / powered state and CoreBluetooth's state, can power the radio
    and run discovery, and `methods` lists BluetoothManager's methods and prints `+lastInitError`.
- **Test 262 (kernel t262, root test258; `bt=9 bt_vsc=3`, no `wlan.debug.generate-mac`):**
  - the kernel publishes an IOService named `bluetooth` under IOResources, with `local-mac-address`
    02:52:44:57:00:02 (the loader's `mac-address-bluetooth0`, which MobileGestalt reports). The new helper
    `redwood_publish_named_service` lives in MiniIOMobileFramebuffer.cpp, the overlay's one C++ file;
  - the controller gets the same address through QCA's EDL_WRITE_BD_ADDR (0xfc14, most significant byte first, as
    Linux `qca_set_bdaddr`).
  - **Result: SETTINGS > BLUETOOTH WORKS.**
    - The chip took the address: EDL_WRITE_BD_ADDR answered `04 0e 04 01 14 fc 00`, and Read_BD_ADDR now gives
      `02 00 57 44 52 02` (02:52:44:57:00:02).
    - The service was published. bluetoothd took the address from it with no boot-arg ("Init Core Stack -
      {localname: iOS, local-addr: 02:52:44:57:00:02}"); "Deleting local address from plist" drops the generated
      one.
    - `bt_test` now gets a BluetoothManager object. (An unsigned tool's MBFramework messages are refused for
      "insufficient entitlements"; Preferences has them.)
    - **Settings > Bluetooth: the switch is on and live**, with "This iPhone is discoverable…", and DEVICES
      searching. bluetoothd ran Preferences' discovery agent: classic inquiry in 4 × 1.28 s periods, repeated, and
      "Local device is now discoverable". Nothing was found, because no device nearby was in pairing mode.
    - The phone's name shows **"(null)"**, although MobileGestalt says "iPhone". It is probably lockdownd's name
      path (lockdownd is not run). Cosmetic, left for later.
  - **Made standard:** `phone-args.txt` now includes `bt=9 bt_vsc=3`. The phone runs image 262 (kernel t262, root
    test258).
  - The method is written up for other phones in `toolkit/docs/BLUETOOTH.md` (README step 11).
  - **Next:**
    1. pair a real device: a hand test, the user puts a keyboard or headphones in pairing mode;
    2. ACL data;
    3. the local name;
    4. 3 Mbit/s;
    5. audio over the chip (later).
- **Test 263 (kernel t263, root test258; + `bt_baud=3000000`): the line at 3 Mbit/s.** Code: `bt_set_fast`.
  - At 32x oversampling, the 7.3728 MHz engine clock tops out at 230400 baud. So the S3 RCG goes to **96 MHz**
    (GPLL0-even 300 MHz × M/N 8/25, one of Linux's QUP DFS rates), with divider 1.
  - The chip follows `hci_qca.c` `qca_set_speed` for the WCN399x:
    1. RTS held "not ready" (`UART_MANUAL_RFR` = bit 31 | bit 1);
    2. EDL_SET_BAUDRATE `01 48 fc 01 0e` (index 14 = 3000000);
    3. wait 10 ms;
    4. change the host's speed;
    5. give RTS back to the hardware.
  - **Result: first try.**
    - At the new speed the chip sent `04 ff 02 92 01` (vendor event) and `04 0e 04 01 00 00 00`.
    - Read_Local_Version answered correctly at 3 Mbit/s: "THE LINE RUNS AT 3 Mbit/s (engine clock 96000000 Hz)".
    - Then the address and stage 4 as before, and bluetoothd's stack started ("Stack did start").
    - LE advertisements kept arriving (bluetoothd processed about 25 a minute).
  - If anything fails, both sides go back to 115200 (the host at once; the chip's speed is then unknown, and logged).
  - **Made standard:** `phone-args.txt` adds `bt_baud=3000000`. The phone runs image 263.
  - Side note: `bt_test le` (unsigned, no bundle) gets 0 advertisements. bluetoothd registers it as a
    "ThirdPartyApp" scan and delivers nothing, the same before and after the speed change. The system's own
    clients (Preferences, locationd) get theirs.
- **Test 264 (kernel t263, root test264 with `ALLOW="locationd lockdown"`): the "(null)" name. Left as is.**
  - bluetoothd takes the device name from lockdownd through liblockdown (`device_name`), and lockdownd is
    DENYed.
  - Even with `lockdown` allowed, mkjobs.py makes no job for it. lockdownd has no MachServices. It is started by
    launchd sockets (`/var/run/lockdown.sock`, **and TCP 62078 on IPv4/IPv6**) and by IOKit/notify LaunchEvents,
    which mkjobs strips. Its job also carries **`_PanicOnCrash`**.
  - Running it would mean a crash panics the system and the lockdown port opens on the Wi-Fi network: too much
    for a cosmetic name. The name needs another way (later).
  - Found on the way: **test.sh did not pass the root build's settings into WSL.** Git Bash's environment does not
    reach `wsl -e`, so the first Test 264 build silently ignored `ALLOW`. test.sh now forwards ALLOW, RELOCATE,
    JOBENV, RELABEL, BT, OSLOG, APPS, SBDIAG, GL, RC, RC_JOBS, EXTRA and EXTRA_SHARE, and prints them ("with …").
- **The "(null)" name: FIXED (2026-11-19, no new image).**
  - bluetoothd does not take the name from lockdownd. Its liblockdown imports are only check-in/send. It takes
    the name from **`SCDynamicStoreCopyComputerName`**, which configd fills from SystemConfiguration's
    `preferences.plist` (System > System > ComputerName). On an iPhone lockdownd writes that key, so here it was
    never set.
  - New tool **`wifi/sc_name.c`** (`/var/r5q/bin/sc_name [NAME]`) shows the name and sets it: SCPreferences
    ComputerName and LocalHostName, then commit, apply and `sync()`. It was set to **"iPhone"**, what
    MobileGestalt reports.
  - **Lesson:** /private/var is a RAM filesystem on this port. It outlives a boot only through
    `var_store save`, which runs on an orderly return, not on `phone.py restart`'s hard reset. Two restarts lost
    the setting; after `/redwood/bin/var_store save` it stayed.
  - Result: bluetoothd starts with "Init Core Stack - {localname: iPhone, …}", and Settings > Bluetooth says
    **This iPhone is discoverable as "iPhone"**.
  - The dynamic store's ComputerName still reads empty, yet bluetoothd has the name. It probably reads it through
    the same SCPreferences path at start.
  - For a fresh install, the key would have to be in the /var template; for now it lives in the saved store.
