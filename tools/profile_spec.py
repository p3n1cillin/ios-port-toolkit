"""The device profile format: every field, which bring-up stage needs it, and where its value comes from.

A profile is JSON (toolkit/devices/<codename>.json). Numbers that are addresses or sizes are written as
hex strings ("0x9C400000"); tools read them with int(x, 0). A field may be null while unknown: the
validator then lists it under the stage that needs it. Anything private to one owner's phone (serial
number, absolute paths with a user name) goes in <codename>.local.json next to it, which the tools
merge on top and which is never shared.

Stages, in bring-up order (README.md, "The method"):
  safety    what the test runner needs to write recovery and put it back (needed before anything runs)
  boot      BootShim -> UEFI FD -> loader.efi: the hand-off chain up to XNU's first instruction
  kernel    XNU running: timer interrupt, GIC, cores, RAM ring log
  userland  the iOS system image over the USB RAM drive, resident in RAM; launchd, SpringBoard
  display   the panel: framebuffer, DSI window, scan-out, flip buffers
  touch / battery / buttons / cpufreq / wifi   peripherals, each on its own
"""

STAGES = ['safety', 'boot', 'kernel', 'userland', 'display', 'touch', 'battery', 'buttons', 'cpufreq', 'wifi']

# (path, type, stages, where it comes from)
# type: str | int | hex | bool | list | dict | sha256
FIELDS = [
    # ---- identity ---------------------------------------------------------------------------------------
    ('device.codename', 'str', ['safety'], 'ro.product.device / the kernel DT model (e.g. r5q)'),
    ('device.brand', 'str', [], 'samsung, xiaomi, google, oneplus, motorola, sony ... (BRANDS.md: how each enters a test)'),
    ('device.soc_data', 'str', [], 'the socs/<soc>.json used when the part number has none of its own (sm7325 -> sc7280)'),
    ('device.model', 'str', ['safety'], 'adb shell getprop ro.product.model (the runner refuses any other)'),
    ('device.name', 'str', [], 'marketing name'),
    ('device.soc', 'str', ['boot'], 'SoC part number (sm8150): picks toolkit/socs/<soc>.json'),
    ('device.soc_name', 'str', [], 'e.g. Snapdragon 855'),
    ('device.android_build', 'str', [], 'ro.build.display.id of the baseline'),
    ('device.ram_bytes', 'int', ['userland'], '/proc/meminfo MemTotal, or the DT memory node'),
    ('device.loader_platform', 'str', ['boot', 'kernel'], 'the name xnuloader gives /arm-io device_type; kernel drivers pick tables by it'),
    ('device.prefix', 'str', ['boot'], 'file prefix of build products, e.g. r5q (r5q-test68-kernel.bin)'),

    # ---- the donor's own profile variables (common/devices/<codename>.conf; tools/conf_bridge.py) -------
    ('donor.kind', 'str', [], 'donor KIND: phone | qemu (export writes phone-recovery for a recovery-tested phone)'),
    ('donor.boot_via', 'str', [], 'donor BOOT_VIA: fastboot | twrp (writes boot!) | recovery-test (this toolkit)'),
    ('donor.uefi_target', 'str', [], 'donor UEFI_TARGET: the Mu-Silicium package name'),
    ('donor.uefi_fd', 'str', [], 'donor UEFI_FD: a built FD'),
    ('donor.dtb', 'str', [], 'donor DTB: the phone\'s own device tree (private)'),
    ('donor.wifi_ath11k', 'int', [], 'donor WIFI_ATH11K'),
    ('donor.bootargs', 'str', [], 'donor BOOTARGS: what a run adds'),
    ('donor.default_secs', 'int', [], 'donor DEFAULT_SECS'),
    ('donor.userland_vars', 'str', [], 'donor USERLAND_VARS (common/build.sh always adds REST=1)'),
    ('donor.firmware', 'str', [], 'donor FIRMWARE: vendor firmware folders pulled from Android'),

    # ---- safety: what may be written and how to put it back --------------------------------------------
    ('flash.method', 'str', ['safety'], 'recovery (Samsung: write recovery, restore after) | fastboot-boot (RAM only, '
                                        'nothing written: Xiaomi, Pixel, OnePlus, Motorola ...); BRANDS.md'),
    ('flash.test_partition', 'str', ['safety'], 'method recovery: ALWAYS "recovery", the only partition a test writes'),
    ('flash.recovery_block_device', 'str', ['safety'], 'readlink -f /dev/block/by-name/recovery (root)'),
    ('flash.recovery_bytes', 'int', ['safety'], 'blockdev --getsize64 /dev/block/by-name/recovery; the test image is padded to it'),
    ('flash.never_write', 'list', ['safety'], 'partitions no tool may write: boot, vbmeta, dtbo, xbl, abl, ... (checked by toolkit/runner/safety.py)'),
    ('flash.enter_test', 'str', ['safety'], 'how Android reboots into the test: reboot(2) RESTART2 "recovery" (no BCB/misc write)'),
    ('flash.download_mode', 'dict', ['safety'], 'Samsung: USB id, tool (samloader/odin), partition name of RECOVERY; others: EDL/fastboot equivalent'),
    ('flash.requires_root', 'bool', ['safety'], 'Magisk root in Android: the runner dd-writes recovery and reads hashes as root'),
    ('flash.min_battery_percent', 'int', ['safety'], 'runner refuses below this'),
    ('baseline.recovery_sha256', 'sha256', ['safety'], 'sha256sum of the untouched recovery partition (backups/<serial>/recovery.img)'),
    ('baseline.boot_sha256', 'sha256', ['safety'], 'sha256sum /dev/block/by-name/boot: verified unchanged before and after every run'),
    ('baseline.vbmeta_sha256', 'sha256', ['safety'], 'sha256sum /dev/block/by-name/vbmeta: verified unchanged before and after every run'),
    ('baseline.dtbo_sha256', 'sha256', [], 'backup only'),
    ('baseline.xbl_sha256', 'sha256', [], 'backup only (XBL holds uefiplat.cfg and the stock DXEs)'),
    ('baseline.reboot_helper_sha256', 'sha256', ['safety'], 'the reboot-recovery helper binary (BootShim build)'),

    # ---- boot image (the container ABL loads from recovery) ---------------------------------------------
    ('bootimg.header_version', 'int', ['boot'], 'stock recovery.img header (offset 40)'),
    ('bootimg.page_size', 'int', ['boot'], 'stock recovery.img header (offset 36)'),
    ('bootimg.kernel_load_pa', 'hex', ['boot'], 'where ABL puts the Image: DRAM base + text_offset (BootShim test SOURCE)'),
    ('bootimg.avb_footer', 'bool', ['boot'], 'stock image ends in an AVBf footer: the test image gets one with algorithm NONE'),
    ('bootimg.seandroid_trailer', 'bool', ['boot'], 'Samsung: "SEANDROIDENFORCE" after the last component'),

    # ---- memory map -------------------------------------------------------------------------------------
    ('memory.dram_base', 'hex', ['boot'], 'DT /memory or uefiplat.cfg first DDR region'),
    ('memory.uefi_fd.base', 'hex', ['boot'], 'uefiplat.cfg "UEFI FD" (XBL dump); BootShim copies the FD here'),
    ('memory.uefi_fd.size', 'hex', ['boot'], 'uefiplat.cfg "UEFI FD"'),
    ('memory.payload.base', 'hex', ['boot'], 'free DDR for the XNUP container (kernel + images); not in any reservation'),
    ('memory.payload.max', 'hex', ['boot'], 'largest container BootShim accepts'),
    ('memory.display_reserved.base', 'hex', ['boot', 'display'], 'uefiplat.cfg "Display Reserved" = DT cont_splash_region'),
    ('memory.display_reserved.size', 'hex', ['display'], 'same'),
    ('memory.bootshim_info', 'hex', ['boot'], 'BootShim progress record: last MiB of Display Reserved, outside the frame'),
    ('memory.log_ring.base', 'hex', ['boot', 'kernel'], 'Samsung: sec_debug_region base + 0x200000 (last_kmsg ring, survives a warm reset); others: ramoops'),
    ('memory.log_ring.size', 'hex', ['boot'], 'ring bytes incl. its 16-byte header'),
    ('memory.log_ring.magic', 'hex', ['boot'], 'header word 1 ("LOGM" on Samsung)'),
    ('memory.smem.base', 'hex', ['boot'], 'DT reserved-memory smem / uefiplat.cfg SMEM'),
    ('memory.smem.size', 'hex', [], ''),
    ('memory.loader_pe_base', 'hex', ['boot'], 'loader.efi /base: inside a region UEFI reports as free (r5q: DXE Heap)'),
    ('memory.loader_window_check', 'dict', ['boot'], 'base/size that validate_memory_window.py proves free of reservations'),
    ('memory.xnu_window.base', 'hex', ['userland'], "the loader's kernel window: the largest free block under the full firmware (probe it: Test 44)"),
    ('memory.xnu_window.size', 'hex', ['userland'], ''),
    ('memory.resident_image.base', 'hex', ['userland'], 'window base + RESIDENT_RD_OFFSET: the iOS system image (md0) lives here'),
    ('memory.usb_ramdisk.base', 'hex', ['userland'], 'the FAT32 RAM disk the PC copies into: above the resident image'),
    ('memory.usb_ramdisk.size', 'hex', ['userland'], ''),
    ('memory.dxe_heap2', 'dict', [], 'extra DXE heap if the Qualcomm DXEs run out (r5q Test 39)'),
    ('memory.secure_no_map', 'list', ['boot'], 'every DT reserved-memory no-map region the UEFI memory map must also reserve (r5q Test 40/41)'),

    # ---- the test image's log slot ------------------------------------------------------------------------
    ('log_slot.offset', 'hex', ['boot'], 'inside recovery zero padding: after AVB-covered data + vbmeta, before the footer block'),
    ('log_slot.blocks', 'int', ['boot'], 'header block + ring blocks (4 KiB)'),
    ('log_slot.magic', 'str', ['boot'], 'the firmware writes only after reading this magic in the slot'),

    # ---- CPU, GIC, timer ---------------------------------------------------------------------------------
    ('cpu.cores', 'int', ['kernel'], 'DT /cpus'),
    ('cpu.mpidr_core_shift', 'int', ['kernel'], 'DT cpu@ reg: 0x100 steps -> 8 (Aff1)'),
    ('cpu.small_cores', 'hex', ['kernel'], 'bitmask of the efficiency cores (DT capacity-dmips-mhz lowest)'),
    ('cpu.midr', 'hex', [], 'loader log on the phone (MIDR_EL1)'),
    ('cpu.cntfrq', 'int', ['kernel'], 'DT arm,armv8-timer clock-frequency (19.2 MHz on Qualcomm)'),
    ('cpu.enable_method', 'str', ['kernel'], 'DT cpu enable-method (psci)'),
    ('gic.dist', 'hex', ['kernel'], 'DT interrupt-controller (arm,gic-v3) reg[0]'),
    ('gic.redist', 'hex', ['kernel'], 'DT interrupt-controller reg[1]'),
    ('gic.vtimer_intid', 'int', ['kernel'], 'DT arm,armv8-timer interrupts: 3rd PPI + 16 (SM8150: 19, not ARM 27). boot-arg gic_vtimer='),
    ('soc_io.base', 'hex', ['kernel'], 'loader struct platform io_base (one window over the peripherals)'),
    ('soc_io.size', 'hex', ['kernel'], ''),
    ('pshold', 'hex', [], 'uefiplat.cfg PSHOLD (MPM2_MPM_PS_HOLD); the reset the firmware does, never the kernel'),

    # ---- kernel/loader -----------------------------------------------------------------------------------
    ('kernel.overlay_dir', 'str', ['kernel'], 'this device\'s kernel overlay (applied over the donor files)'),
    ('kernel.base_boot_args', 'str', ['kernel'], 'what the loader always adds (debug=0x8 gic_vtimer=N ...)'),
    ('kernel.feature_boot_args', 'dict', [], 'boot-arg per feature: touch, cpufreq, buttons, fbscale ...'),
    ('loader.flag_prefix', 'str', ['boot'], 'loader.c compile-time flag prefix (R5Q_)'),
    ('loader.profile_flags', 'list', ['boot'], 'the flag set of the current userland test profile'),
    ('loader.platform_struct', 'dict', ['boot'], 'loader.c struct platform fields (fb_base identifies the device)'),

    # ---- firmware (Mu-Silicium) ----------------------------------------------------------------------------
    ('firmware.package', 'str', ['boot'], 'Mu-Silicium platform package (full: every Qualcomm DXE)'),
    ('firmware.reference_package', 'str', [], 'closest upstream Silicium device with the same SoC'),
    ('firmware.uefiplat_cfg', 'str', ['boot'], 'path of the uefiplat.cfg extracted from the phone\'s own XBL'),
    ('firmware.usb_msd_label', 'str', ['userland'], 'FAT volume label of the USB RAM drive (the PC side waits for it)'),
    ('firmware.usb_controller_init_event', 'str', ['userland'], 'event group GUID that makes UsbConfigDxe start the cores (Qualcomm)'),
    ('firmware.pmic_pshold_reset_type', 'dict', [], 'PON PS_HOLD_RESET_CTL value: warm keeps the RAM ring'),

    # ---- display ---------------------------------------------------------------------------------------------
    ('display.width', 'int', ['display', 'boot'], 'panel pixels (DT panel timing / dsi panel node / ABL log)'),
    ('display.height', 'int', ['display', 'boot'], ''),
    ('display.stride', 'int', ['display', 'boot'], 'bytes per row (width * 4 at 32 bpp)'),
    ('display.mdss_base', 'hex', ['display', 'boot'], 'DT qcom,mdss_mdp reg'),
    ('display.dsi0_offset', 'hex', ['display'], 'DSI0 from MDSS base (SM8150/8250: 0x94000); DSI 6G registers shifted by 4'),
    ('display.intf_offset', 'hex', ['display'], 'the interface driving the panel (INTF1 = +0x6B800)'),
    ('display.pipe_offsets', 'list', ['display'], 'SSPP blocks (VIG0-3, DMA0-3)'),
    ('display.te_gpio', 'int', ['display'], 'DT mdss TE pin (mdp_vsync function)'),
    ('display.te_function', 'int', ['display'], 'its mux index (socs/<soc>.json)'),
    ('display.panel_window_dcs', 'dict', ['display'], 'DCS 2A/2B column/page set (0..w-1, 0..h-1) resent after reset'),
    ('display.abl_command_buffer', 'hex', ['display'], 'DSI DMA_BASE the stock bootloader left (read from the DSI dump)'),
    ('display.autorefresh', 'hex', ['display'], 'INTF autorefresh the stock bootloader uses (0x80000002: every 2nd TE)'),
    ('display.flip_buffers', 'list', ['display'], 'two/three frames inside Display Reserved, 2 MiB aligned'),
    ('display.reserved_end', 'hex', ['display'], 'top of what the kernel may use (keeps the command page and BootShim record)'),
    ('display.gcc_disp_ahb_cbcr', 'hex', ['display'], 'GCC disp_ahb branch: HW_CTL means CLK_OFF reads 1 when idle'),
    ('display.dispcc_base', 'hex', ['display'], 'display clock controller'),
    ('display.ios_points', 'list', [], 'the iOS screen in points (iPhone X userland: 375 x 812)'),

    # ---- TLMM ------------------------------------------------------------------------------------------------
    ('tlmm.base', 'hex', ['touch', 'battery'], 'DT pinctrl@ reg[0]'),
    ('tlmm.tiles', 'dict', ['touch', 'battery'], 'tile -> base (socs/<soc>.json)'),

    # ---- touch -----------------------------------------------------------------------------------------------
    ('touch.compatible', 'str', ['touch'], 'DT touchscreen node compatible'),
    ('touch.driver', 'str', ['touch'], 'kernel overlay driver that speaks its protocol'),
    ('touch.i2c_address', 'hex', ['touch'], 'touchscreen@NN reg'),
    ('touch.se_base', 'hex', ['touch'], 'the I2C node (qcom,i2c-geni) reg'),
    ('touch.sda_gpio', 'int', ['touch'], 'pinctrl-0 of the I2C node: mux pins'),
    ('touch.scl_gpio', 'int', ['touch'], ''),
    ('touch.pin_function', 'int', ['touch'], 'mux index of the QUP function on those pins (socs/<soc>.json)'),
    ('touch.bus_tile', 'str', ['touch'], 'TLMM tile of SDA/SCL'),
    ('touch.irq_gpio', 'int', ['touch'], 'touchscreen interrupts / irq_gpio'),
    ('touch.irq_tile', 'str', ['touch'], ''),
    ('touch.bus', 'str', [], 'i2c | spi (the parent controller node)'),
    ('touch.reset_gpio', 'int', [], 'the chip reset line, if the DT has one (reset-gpios)'),
    ('touch.supplies', 'list', ['touch'], 'regulator-fixed GPIOs or PMIC LDOs the chip needs (read first, raise only if low)'),
    ('touch.gcc', 'dict', ['touch'], 'vote register + bits, SE/AHB branch CBCRs, SE RCG (from DT clocks + socs data)'),
    ('touch.transport', 'str', ['touch'], 'geni-fifo | bitbang (GSI-only engines: bit-bang the pins at 100 kHz)'),
    ('touch.max_coords', 'list', [], 'DT max_coords / chip panel info'),
    ('touch.firmware', 'str', [], 'DT firmware_name (not loaded by us: the chip keeps its flash)'),

    # ---- battery ---------------------------------------------------------------------------------------------
    ('battery.gauge', 'str', ['battery'], 'fuel gauge chip'),
    ('battery.compatible', 'str', ['battery'], ''),
    ('battery.i2c_address', 'hex', ['battery'], ''),
    ('battery.se_base', 'hex', ['battery'], ''),
    ('battery.sda_gpio', 'int', ['battery'], ''),
    ('battery.scl_gpio', 'int', ['battery'], ''),
    ('battery.pin_function', 'int', [], ''),
    ('battery.transport', 'str', ['battery'], ''),
    ('battery.registers', 'dict', ['battery'], 'READ-ONLY registers (Samsung driver source); never written'),
    ('battery.capacity_max', 'int', [], 'DT capacity_max (scaling to what Android shows)'),

    # ---- buttons ---------------------------------------------------------------------------------------------
    ('buttons.spmi', 'dict', ['buttons'], 'DT spmi@ reg-names core/chnls/obsrvr/intr/cnfg; arbiter version'),
    ('buttons.pon', 'dict', ['buttons'], 'qcom,qpnp-power-on of the main PMIC: SID, base, peripheral id, subtypes, KPDPWR/RESIN status bits'),
    ('buttons.volume_up', 'dict', ['buttons'], 'DT gpio_keys: PMIC GPIO -> peripheral id (sid << 8 | (0xC000 + (n-1)*0x100) >> 8)'),
    ('buttons.volume_down', 'str', ['buttons'], 'usually RESIN of the PON'),
    ('buttons.power', 'str', ['buttons'], 'usually KPDPWR of the PON'),

    # ---- cpufreq ---------------------------------------------------------------------------------------------
    ('cpufreq.kind', 'str', ['cpufreq'], 'osm (SM8150: qcom,clk-cpu-osm) | epss/cpufreq-hw (SM8250+)'),
    ('cpufreq.domains', 'list', ['cpufreq'], 'per cluster: cores + base (DT reg-names)'),
    ('cpufreq.table_offset', 'hex', ['cpufreq'], 'LUT start (OSM 0x110, EPSS 0x100)'),
    ('cpufreq.row_bytes', 'int', ['cpufreq'], 'LUT stride (OSM 32, EPSS 4)'),
    ('cpufreq.perf_state', 'hex', ['cpufreq'], 'the register taking the wanted row (OSM 0x920, EPSS 0x320)'),

    # ---- wifi ------------------------------------------------------------------------------------------------
    ('wifi.kind', 'str', ['wifi'], 'icnss (WCN399x on the SoC) | pcie (QCA6390/6490, BCM43xx) | sdio'),
    ('wifi.nodes', 'list', ['wifi'], 'DT nodes and their status'),

    # ---- userland ------------------------------------------------------------------------------------------
    ('userland.system_image', 'str', ['userland'], 'the iOS system volume (SYSTEM.APF)'),
    ('userland.system_image_bytes', 'int', ['userland'], 'its size: the rdres item and the markers use it'),
    ('userland.build_vars', 'str', [], 'userland/build.sh variables: REST=1 is the phone recipe'),
    ('userland.system_sources', 'list', ['userland'], 'usb-ramdisk (PC copy each run) and/or sdcard (FAT32 microSD)'),

    # ---- storage ---------------------------------------------------------------------------------------------
    ('storage.userdata_block_device', 'str', [], 'readlink -f /dev/block/by-name/userdata'),
    ('storage.ufs_luns', 'list', [], 'from the firmware probe (Test 44)'),
]

TYPES = {'str': str, 'int': int, 'bool': bool, 'list': list, 'dict': dict}


def get(profile: dict, path: str):
    node = profile
    for part in path.split('.'):
        if not isinstance(node, dict) or part not in node:
            return None
        node = node[part]
    return node


def num(value):
    """An address/size from a profile: int or "0x..." string."""
    if value is None:
        return None
    if isinstance(value, int):
        return value
    return int(str(value).replace('_', ''), 0)


def merge(base: dict, over: dict) -> dict:
    out = dict(base)
    for k, v in over.items():
        if isinstance(v, dict) and isinstance(out.get(k), dict):
            out[k] = merge(out[k], v)
        else:
            out[k] = v
    return out


def load(path, local=True) -> dict:
    """A profile plus its <codename>.local.json overlay (serial, local paths) when present."""
    import json
    import pathlib
    path = pathlib.Path(path)
    data = json.loads(path.read_text(encoding='utf-8'))
    over = path.with_name(path.stem + '.local.json')
    if local and over.exists():
        data = merge(data, json.loads(over.read_text(encoding='utf-8')))
    return data
