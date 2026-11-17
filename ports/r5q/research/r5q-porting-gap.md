# S10 Lite (r5q, SM8150) porting gap against the donor kernel

Host-only survey, 2026-11-16. Donor: `/var/tmp/codex-ios-r5q/work/linux-source` (G986N-iOS, a HackiOS port that boots
unmodified iOS 16.5 on the Galaxy S20+ y2q / SM8250). Phone facts come from `backups/<SERIAL>/live.dtb`.

## Loader: already correct

`platform_r5q` in `port/xnuloader/loader.c` matches the live device tree field for field:

| Field | loader | live.dtb |
|---|---|---|
| GIC dist / redist | 0x17A00000 / 0x17A60000 | `arm,gic-v3` reg 0x17a00000 0x10000 0x17a60000 0x100000 |
| cores, MPIDR shift | 8, 8 | cpu@0..cpu@700 |
| small cores | 0x0f | cpu 0-3 capacity 0x400, 4-7 0x6cc |
| framebuffer | 0x9C400000 | cont_splash / splash 0x9c400000 size 0x2000000 |

Model "Samsung R5Q PROJECT Rev09", compatible `qcom,sm8150-mtp`. The `darwin` partition is 0/0 (none): correct
for now, since no partition exists for iOS on this phone.

## Kernel: what is generic, what is gated

Every phone-specific driver asks `redwood_platform_is(name)` (bsd/dev/redwood_input.c:216), which compares
`/arm-io` `device_type` in the loader-built Apple device tree. On r5q that is "r5q", so all `y2q`/`redwood`
branches are off: framebuffer controller (MiniIOMobileFramebuffer.cpp:970), touch (redwood_sec_ts.c:1157,
redwood_goodix.c:1512), PCIe/Wi-Fi, MAX77705, Adreno board table, UFS darwin placement (redwood_ufs.c:88-94).

Paths that would touch SM8250/SM7325-only hardware are boot-arg gated and off with the lab cmdline (`-v`, plus
the loader's `debug=0x8 cpus=1`):

- `redwood_cpufreq.c`: EPSS at 0x18591000 (SM8150 has OSM instead), only with `cpufreq=`/`ddr=` boot-args.
- `loader_return.c`: hypervisor watchdog pet (SMC 0x86000007), only with `hv_wdt_pet=1`.

Keep these boot-args OFF for r5q until an r5q-specific implementation exists.

## Work an r5q bring-up needs (in donor PORTING.md order)

1. **Early kernel on hardware**: reach the checkpoints past 1 on the phone. Unicorn cannot go past checkpoint 4
   (an unexplained prefetch abort at MMU enable; the kernel uses 4 KB pages, so it is not the page granule).
   QEMU (`run_qemu_ladder.sh`, platform qemu-virt) returns cleanly from every checkpoint except 10, which hangs
   for the prebuilt too (its return path runs GIC/boot-arg code before PE_init_platform). QEMU cannot model
   r5q's hypervisor/cache behaviour (the Test 15/16 problem). Usable hardware targets: 1-5 (assembly), 11 onward (C).
2. **Display**: generic UEFI framebuffer first (the generic console works on any `fb_base`); an r5q MDSS
   (`qcom,mdss_mdp` present) driver like y2q's MiniIOMobileFramebuffer path only for flipping/scaling later.
3. **Touch**: r5q has `sec_ts` in its DT, the same Samsung LSI family as y2q's redwood_sec_ts.c. Needs r5q's
   I2C/QUP bus address, GPIOs and IRQ from live.dtb; candidate for a gated `r5q` branch.
4. **Buttons**: redwood_buttons.c table line 173 already describes y2q's PM8150 PON block. r5q also has a PM8150
   (`qcom,pm8150` present), so an `r5q` row may be the same or close. Verify SPMI addresses from live.dtb.
5. **Storage**: a `darwin` partition is needed before any iOS root on flash; until then use ramdisk boots
   (`xl ramdisk` style). Repartitioning the phone is high risk and needs the user's explicit decision.
6. **Later**: GPU (Adreno 640 vs y2q's 650: new adreno_boards row and firmware), CPU frequency (OSM, not EPSS),
   charger/battery (no MAX77705 on r5q), Wi-Fi.

Nothing here has been flashed or approved for hardware.
