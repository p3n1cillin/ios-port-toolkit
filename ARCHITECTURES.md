# Architectures: what carries over to which SoC

The port has three layers. Each depends on the SoC to a different degree.

| Layer | What it needs | Qualcomm | MediaTek | Exynos / Tensor |
|---|---|---|---|---|
| **Boot chain**: BootShim → UEFI FD → loader.efi | A Mu-Silicium UEFI for the SoC (the loader runs as a UEFI app, and USB/storage come from the UEFI's drivers) | 26 silicon packages, 80+ devices (`socs/mu-silicium.json`) | MT6768, MT6789 only | Exynos 7870/7885/9810/9820/990/2400; **no Tensor (gs101+)** |
| **Generic kernel**: GICv3, ARM generic timer, PSCI, MMU/caches, RAM log, framebuffer | Standard ARMv8 pieces | yes (vtimer INTID may differ: SM8150 uses 19) | GICv3 on recent SoCs; check the timer PPI | GICv3; check the timer PPI |
| **Peripherals**: touch, battery, buttons, CPU clocks, display, Wi-Fi | Vendor blocks | Full SoC data: TLMM pins/tiles/functions, GCC clock registers by QUP engine (`socs/<soc>.json`, 24 SoCs + aliases) | Pin modes and pio blocks (`socs/mt*.json`, 10 SoCs); **clocks, PMIC (PWRAP) not covered** | Pin banks per controller (`socs/exynos*.json`, `gs101.json`, 8 SoCs); **clocks (CMU), PMIC (ACPM/SPEEDY) not covered** |

`tools/assess_device.py` turns this into a tier for a given phone:

| Tier | Meaning | Examples |
|---|---|---|
| A | The method is proven on this SoC | SM8150 (S10 Lite), SM8250 (S20+), SM7325 (POCO X5 Pro) |
| B | Qualcomm with a UEFI and full SoC data: the S10 Lite path applies step by step | SM8350/8450/8550/8650/8750, SDM845, SM6115/6125/6375, SM7125/7150 phones with Mu-Silicium ports |
| C | A UEFI exists but SoC data is partial: driver work for each peripheral | Exynos 990/9810/9820/2400 Galaxies, MT6768/MT6789 Xiaomis |
| D | No UEFI for the SoC: a Mu-Silicium silicon port comes first, as a project of its own | Pixel 6+ (Tensor), most MediaTek Dimensity |
| X | Blocked | Huawei/Honor (no unlock); GICv2-only SoCs need a new interrupt driver first |

## Qualcomm (the reference path)

`tools/qcom_socdata.py --linux <tree> --all-phone` builds `socs/<soc>.json` from Linux. For each SoC it records:

- every GPIO's TLMM tile and mux functions, with the tile bases taken from the dtsi's tlmm `reg-names` or the
  driver's tile offsets
- every GCC clock's CBCR, vote register and bit, and RCG
- the clock of each QUP serial engine, keyed by its address

The address key matters: Samsung's vendor trees number clocks differently from upstream (S20+), so the
extractor resolves a bus by its engine address.

Things learned on Qualcomm that likely repeat on any SoC:

- Hand off with ABL's own shutdown sequence (set/way flush, SCTLR off), not a per-region DC clean over the
  firmware map. The clean hung the S10 Lite.
- The vendor's virtual-timer PPI. When the kernel boots and then never wakes, check `gic_vtimer`.
- Reserve every live-DT `no-map` region in the UEFI memory map.
- The vendor's display controller starts in a state of its own (panel window, autorefresh): measure it first.

## MediaTek

`tools/soc_other.py --mediatek <soc>` gives:

- each pin's modes, from Linux `pinctrl-mtk-<soc>.h`
- the pio register blocks and the I2C controllers, from the dtsi

DT pin states are `pinmux = <(pin << 8) | mode>`; the extractor reads them and names the mode (`SDA0_0`, ...).

Not covered yet:

- **Clocks:** infracfg/pericfg gates. Each is a set/clear register pair per gate, from Linux `clk-mt<soc>-*.c`.
- **PMIC:** MediaTek PWRAP (the PMIC wrapper; read-only use only, R4).
- **I2C:** the controller (`i2c-mt65xx.c`), a different engine from Qualcomm GENI.
- **Display:** MediaTek DISP (OVL/RDMA/DSI).

Boot chain: MediaTek uses LK/`lk2nd`-style bootloaders. Only MT6768/MT6789 have Mu-Silicium packages. Other
SoCs need a UEFI port, or a different first stage that provides the services `loader.efi` uses: memory map,
GOP, block I/O and USB.

## Exynos and Google Tensor

`tools/soc_other.py --exynos <soc>` gives the pin banks (name, pin count, offset) of every pin controller, from
Linux `pinctrl-exynos-arm64.c`. Each controller's base comes from the dtsi aliases `pinctrlN`.

DT pin states are `samsung,pins = "gpp16-0"` with `samsung,pin-function = <3>`. The extractor resolves these
to the bank's register address (gs101 HSI2C8: `pinctrl_peric0` + 0x200 = 0x10840200).

Not covered yet:

- **Clocks:** the CMU blocks, from Linux `clk-exynos*.c` / `clk-gs101.c`.
- **PMIC:** Samsung S2MPS over ACPM/SPEEDY. Read-only only (R4).
- **I2C:** the HSI2C controller, which differs from GENI.
- **Display:** DECON/DSIM.

Boot chain: Exynos Galaxies with Mu-Silicium packages (S5E7870/7885/9810/9820/9830/9945) are tier C. Their
bootloader is Samsung's S-Boot, not Qualcomm ABL. Check how its recovery boot loads an image before reusing
BootShim. Tensor (Pixel 6+) has no Mu-Silicium package: tier D.

## Adding a vendor

1. Write `socs/<soc>.json` with `vendor`, pin data and register blocks. Follow `tools/soc_other.py`.
2. Teach `extract_profile.mux_pins` the vendor's DT pin-state syntax and `soc_pin` its register layout.
3. Add a synthetic fixture under `tests/fixtures/` built from upstream data, and a check in `tools/selftest.py`.
4. Add the clock/PMIC/I2C notes for the vendor to this file. The kernel drivers are the per-vendor code
   (skill `ios-port-peripherals`).
