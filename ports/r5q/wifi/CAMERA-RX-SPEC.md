# Camera receive path on SM8150 (Titan 175): CSI-PHY 2 -> CSID-lite 0 -> IFE-lite 0 RDI0 -> memory

Desk research only. Nothing here has been run on the phone yet. This is the register-level porting spec for
`redwood_camera.c`'s cam=3/cam=4 path (donor: POCO X5 Pro, SM7325/yupik, Titan 165).

Goal: RAW10 frames from the S5K3L6 (DT `qcom,cam-sensor@2`, 4-lane D-PHY on CSI-PHY 2, DT 0x2b, mode 2 =
1028x772 @ 120, settle 14) written into a memory buffer, with the done status polled (no interrupts).

## Sources (all read as data)

| Tag | What | Where |
| --- | --- | --- |
| DT | the phone's live device tree | `work/ios-r5q/live.dts` (line numbers below) |
| DONOR | `redwood_camera.c` | `work/ios-r5q/kernel-build/r5q-overlay/bsd/dev/redwood_camera.c` |
| CC | downstream `drivers/clk/qcom/camcc-sm8150.c` (the phone's DT says `qcom,camcc-sm8150-v2`, so its `cam_cc_sm8150_fixup_sm8150v2()` applies) | LineageOS `android_kernel_qcom_sm8150`, branch lineage-20 |
| CCH | downstream `include/dt-bindings/clock/qcom,camcc-sm8150.h` (clock indexes) | same tree |
| GCC | downstream `drivers/clk/qcom/gcc-sm8150.c` + `qcom,gcc-sm8150.h` | same tree |
| APLL | downstream `drivers/clk/qcom/clk-alpha-pll.c` (`clk_trion_pll_configure`, `clk_trion_pll_enable`) | same tree |
| VDD | downstream `drivers/clk/qcom/vdd-level.h` | same tree |
| PHY | `cam_csiphy_1_1_hwreg.h`, `cam_csiphy_core.c` (`cam_csiphy_reset`, `cam_csiphy_config_dev`), `cam_csiphy_soc.c` | same tree, `drivers/media/platform/msm/camera/cam_sensor_module/cam_csiphy/` |
| CSID | `cam_ife_csid_lite17x.h` (used for `qcom,csid-lite175` by `cam_ife_csid_lite17x.c`), `cam_ife_csid_core.c/.h` | `.../cam_isp/isp_hw_mgr/isp_hw/ife_csid_hw/` |
| VFE | `cam_vfe_lite17x.h` (used for `qcom,vfe-lite175` by `cam_vfe17x.c`), `cam_vfe_bus_ver2.c`, `cam_vfe_top_ver2.c`, `cam_vfe_rdi.c`, `cam_vfe_core.c` | `.../isp_hw/vfe_hw/` |
| CPAS | `cam_cpastop_hw.c`, `cpastop_v175_101.h` | `.../cam_cpas/` |
| UP | upstream Linux `drivers/clk/qcom/camcc-sm8150.c` (cross-check: same PLL0 values and offsets) and `drivers/interconnect/qcom/sm8150.c` (BCMs) | torvalds/linux master |
| R5Q | this port's own proven SMMU and RPMh code | `redwood_wifi.c` (`r5q_smmu_look`, `rsc_write`, `redwood_r5q_rpmh_enable`), `redwood_adsp.c` (`frpc_smmu`, `r5q_slim_smmu`, `r5q_dma_area`) |

Note: the donor is closer to SM8150 than "Titan 570" suggests. It is **Titan 165**, using the same CSID-lite 17x
register map and the same VFE bus ver2 as SM8150's lite175 (DONOR comment above `RSC_IRQ_STATUS`, about line
1724). The CSID and IFE parts port almost unchanged. The camcc, CSI-PHY, GDSC and SMMU parts change.

## Addresses (from DT)

| Block | Physical base | DT node / line |
| --- | --- | --- |
| camcc | 0x0ad00000 (size 0x10000) | `qcom,camcc` 5498 |
| GCC | 0x00100000 | `qcom,gcc` 5474 |
| titan_top_gdsc | 0x0ad0c1bc (camcc + 0xc1bc) | `qcom,gdsc@0xad0c1bc` 658, phandle 0x32 |
| CPAS top / CAMNOC | 0x0ac40000 (0x1000) / 0x0ac42000 (0x5000) | `qcom,cam-cpas@ac40000` 4294 |
| CSI-PHY 2 | 0x0ac67000 (0x1000), cell 2, IRQ SPI 0x1df | `qcom,csiphy@ac67000` 3617 |
| CSID-lite 0 | 0x0acc8000 (0x1000), cell 2, SPI 0x1d4 | `qcom,csid-lite0@acc8000` 4518 |
| IFE-lite 0 | 0x0acc4000 (0x4000), cell 2, SPI 0x1d5 | `qcom,vfe-lite0@acc4000` 4538 |
| apps SMMU | 0x15000000 (0x100000), `qcom,qsmmu-v500`, `qcom,skip-init` | `apps-smmu@0x15000000` 24957, phandle 0x26 |
| apps RSC (DRV 2) | 0x18220000; TCS block 0x18220d00 | `mailbox@18220000` 7235 |

### Phandles resolved

- `0x33` = camcc. Clock indexes via CCH, all matching the DT's own `clock-names`:

  | Index | Clock | Index | Clock |
  | --- | --- | --- | --- |
  | 0x05 | CAM_CC_CAMNOC_AXI_CLK | 0x14 | CAM_CC_CSI2PHYTIMER_CLK_SRC |
  | 0x06 | CAM_CC_CAMNOC_AXI_CLK_SRC | 0x19 | CAM_CC_CSIPHY2_CLK |
  | 0x0d | CAM_CC_CPAS_AHB_CLK | 0x31 | CAM_CC_IFE_LITE_0_CLK |
  | 0x0e | CAM_CC_CPHY_RX_CLK_SRC | 0x32 | CAM_CC_IFE_LITE_0_CLK_SRC |
  | 0x13 | CAM_CC_CSI2PHYTIMER_CLK | 0x33 | CAM_CC_IFE_LITE_0_CPHY_RX_CLK |
  | 0x1f | CAM_CC_GDSC_CLK | 0x34 | CAM_CC_IFE_LITE_0_CSID_CLK |
  | 0x5f | CAM_CC_SLOW_AHB_CLK_SRC | 0x35 | CAM_CC_IFE_LITE_0_CSID_CLK_SRC |

- `0x1b` = GCC. Indexes 8/9/10/11 = GCC_CAMERA_AHB_CLK / GCC_CAMERA_HF_AXI_CLK / GCC_CAMERA_SF_AXI_CLK /
  GCC_CAMERA_XO_CLK (GCC header).
- `0x2a` (`mipi-csi-vdd-supply` of every CSI-PHY) = `pm8150l_l3`: RPMh resource **`ldoc3`**, fixed 1.2 V
  (0x124f80 µV), `qcom,proxy-consumer-enable` (DT 18099-18117). The DT's `csi-vdd-voltage` is the same 1.2 V.
- `0x32` (`gdscr-supply` / `camss-supply`) = titan_top_gdsc. **CSID-lite and IFE-lite have no GDSC of their own**:
  their only regulator is `camss` = titan_top (DT 4526, 4546). `ife_0_gdsc` (0xad0a004) is for the full IFE0 only.
- `0x1c` (camcc `vdd_mm`, titan_top `parent-supply`) = RPMh **`mmcx.lvl`** (`pm8150l_s5_level`, DT ~17960-17970).
- `0x58` (camcc `vdd_mx`) = RPMh **`mx.lvl`** (DT ~17915-17924).
- `0x26` = apps SMMU.

### DT clock rates used (the `svs` level)

- CSI-PHY 2: `cphy_rx_clk_src` 400 MHz (0x17d78400), `csi2phytimer_clk_src` 300 MHz (0x11e1a300), level "turbo"
  (DT 3633).
- csid-lite0 `clock-rates`, 6 clocks per level, levels lowsvs/svs/svs_l1/turbo (DT 4530):
  - `ife_csid_clk_src` 400/400/480/600 MHz;
  - `ife_clk_src` 320/400/480/600 MHz.
  - **Use svs: 400/400.** Both come from PLL0's odd output, so no PLL2 is needed. Lowsvs's 320 MHz would need PLL2.
- cpas `clock-rates` (DT 4311), 7 levels × 7 clocks:
  - `slow_ahb_clk_src` 80 MHz from lowsvs up;
  - `camnoc_axi_clk_src` 150 MHz at lowsvs (266.67/320/400/480 higher).
  - **Use 80 and 150.**

## (a) camcc and GCC

All RCGs: `CMD_RCGR` = base, `CFG_RCGR` = base + 4.

- `CFG_RCGR`: [4:0] divider field = 2·div − 1, [10:8] source, [13:12] M/N mode (0 here; no RCG below uses M/N).
- `CMD_RCGR`: bit 0 = UPDATE (set it; it self-clears), bit 31 = ROOT_OFF.
- Branch CBCR: bit 0 = enable, bit 31 = CLK_OFF. All branches below are `BRANCH_HALT`: wait for bit 31 to clear.
- Preserve the other `CFG_RCGR` bits (the donor's mask 0x371f, `clocks_up()`).

### a.0 Rails first (see (b)), then GCC

Sources: GCC `gcc_camera_*_clk`, DT 4309.

| Phys | Clock | Action |
| --- | --- | --- |
| 0x0010b008 | gcc_camera_ahb_clk (CRITICAL, hwcg bit 1) | already on: Test 271 read 0x80000003. Read only. |
| 0x0010b044 | gcc_camera_xo_clk (CRITICAL) | read only; expect bit 0 set |
| 0x0010b030 | gcc_camera_hf_axi_clk | set bit 0; wait for bit 31 = 0 |
| 0x0010b034 | gcc_camera_sf_axi_clk | set bit 0; wait for bit 31 = 0 |

The donor's GCC offsets are wrong here: its 0x26010 and 0x2601c are yupik's.

### a.1 Power domain (already proven: Test 273)

1. `0x0ad0c1e4` CAM_CC_GDSC_CLK: set bit 0. It is CRITICAL in CC `cam_cc_sm8150_critical_clocks`.
2. `0x0ad0c1bc` titan_top GDSCR: clear bit 0 (SW_COLLAPSE), then wait for bit 31 (PWR_ON). Test 273 read
   0x22f001 -> 0xf822f000.

> **Donor trap:** the donor's `TITAN_TOP_GDSCR` is **0xc194**. On SM8150, camcc 0xc194 is
> `cam_cc_camnoc_dcd_xo_clk`'s CBCR (CC). Never use the donor's constant.

### a.2 PLL0 (Trion), 1200 MHz; even output 600, odd output 400

Sources:
- values: CC `cam_cc_pll0_config_sm8150_v2` (UP's `cam_cc_pll0_config` is identical);
- order: APLL `clk_trion_pll_configure` (writes only non-zero config fields) + `clk_trion_pll_enable`;
- register offsets: APLL `TRION_PLL_*`.

PLL0 base = camcc + 0x0 = **0x0ad00000**.

Guard: read MODE (+0x00) and OPMODE (+0x38) first. If MODE bit 0 (OUTCTRL) and OPMODE bit 0 (RUN) are both set,
somebody else's PLL is running: stop, as the donor's `pll0_up()` does. Also check MODE bit 20 (VOTE_FSM_ENA): if
set, the PLL is in voting mode and the sequence below does not apply.

| Step | Phys | Value | Note |
| --- | --- | --- | --- |
| 1 | 0x0ad00004 L_VAL | 0x0000003E | 19.2 MHz × (62 + 0x8000/65536) = 1200 MHz |
| 2 | 0x0ad00008 CAL_L_VAL | 0x00000044 | TRION_PLL_CAL_VAL |
| 3 | 0x0ad00040 ALPHA_VAL | 0x00008000 | |
| 4 | 0x0ad00018 CONFIG_CTL | 0x20485699 | |
| 5 | 0x0ad0001c CONFIG_CTL_U | 0x00002267 | the donor's Lucid value: 0x2261 |
| 6 | 0x0ad00020 CONFIG_CTL_U1 | 0x00000024 | the donor's: 0x329a299c |
| 7 | 0x0ad0000c USER_CTL | 0x00003100 | [11:8] even postdiv = 1 (÷2), [15:12] odd postdiv = 3 (÷3); the donor's 0x3101 |
| 8 | 0x0ad00010 USER_CTL_U | 0x00000805 | |
| 9 | 0x0ad00014 USER_CTL_U1 | 0x000000D0 | the donor's: 0 |
| 10 | 0x0ad0002c TEST_CTL_U1 | 0x00000020 | TEST_CTL and TEST_CTL_U are 0 in v2 and so not written |
| 11 | MODE | MODE \| bit 23 | HW_UPDATE_LOGIC_BYPASS |
| 12 | MODE | MODE & ~bit 0 | outputs off |
| 13 | 0x0ad00038 OPMODE | 0 | STANDBY |
| 14 | MODE | MODE \| bit 2 | RESET_N: out of reset |
| 15 | OPMODE | 1 | RUN |
| 16 | poll MODE bit 31 | | LOCK_DET; the donor allows 1 ms |
| 17 | USER_CTL | \|= 0x7 | main, even and odd outputs |
| 18 | MODE | \|= bit 0 | OUTCTRL |

- Read back: MODE should have bits 31 (lock), 23, 2 and 0 set, i.e. about 0x80800005 (other status bits may also be
  set). OPMODE should be 1, USER_CTL 0x3107. STATUS is +0x30.
- Teardown (the reverse, as the donor's `pll0_down`): MODE &= ~1; USER_CTL &= ~7; OPMODE = 0; MODE &= ~bit 2.
- PLL0 1200 MHz needs **MX ≥ SVS_L1**: `cam_cc_pll0.rate_max` gives VDD_LOW (SVS) = 1066 MHz and VDD_LOW_L1 =
  1600 MHz; VDD maps VDD_LOW_L1 to RPMH_REGULATOR_LEVEL_SVS_L1 = 192.

### a.3 RCGs (source numbers: CC `cam_cc_parent_map_0`)

Source numbers: 0 = TCXO, 1 = PLL0 main (1200), 2 = PLL0 even (600), 3 = PLL0 odd (400), 5 = PLL2 main.

| RCG | CMD_RCGR (phys) | Freq-table entry (CC) | CFG_RCGR value ([10:8] src, [4:0] div) | MMCX need (CC rate_max) |
| --- | --- | --- | --- | --- |
| slow_ahb_clk_src | 0x0ad07058 | 80 MHz = PLL0_EVEN / 7.5 | **0x20e** | LOWER (64) |
| camnoc_axi_clk_src | 0x0ad0c170 | 150 MHz = PLL0_EVEN / 4 | **0x207** | LOWER (64) |
| cphy_rx_clk_src | 0x0ad0a064 | 400 MHz = PLL0_ODD / 1 | **0x301** | LOWER (64) |
| csi2phytimer_clk_src | 0x0ad0604c | 300 MHz = PLL0_EVEN / 2 (`ftbl_cam_cc_csi0phytimer_clk_src`) | **0x203** | LOWER (64) |
| ife_lite_0_csid_clk_src | 0x0ad0c020 | 400 MHz = PLL0_ODD / 1 (`ftbl_cam_cc_fd_core_clk_src`) | **0x301** | LOWER (64) |
| ife_lite_0_clk_src | 0x0ad0c004 | 400 MHz = PLL0_ODD / 1 (`ftbl_cam_cc_ife_lite_0_clk_src`) | **0x301** | **LOW = SVS (128)** |

For each RCG: write CFG (preserving the bits outside 0x371f), set CMD bit 0, then poll CMD bit 0 == 0 (the donor
allows 500 µs).

### a.4 Branches (CC `cam_cc_*_clk`, all `BRANCH_HALT`, enable mask bit 0)

| Branch | CBCR (phys) | Parent |
| --- | --- | --- |
| cam_cc_cpas_ahb_clk | 0x0ad0c168 | slow_ahb |
| cam_cc_camnoc_axi_clk | 0x0ad0c18c | camnoc_axi |
| cam_cc_csiphy2_clk | 0x0ad06068 | cphy_rx |
| cam_cc_csi2phytimer_clk | 0x0ad06064 | csi2phytimer |
| cam_cc_ife_lite_0_csid_clk | 0x0ad0c038 | ife_lite_0_csid |
| cam_cc_ife_lite_0_cphy_rx_clk | 0x0ad0c040 | cphy_rx |
| cam_cc_ife_lite_0_clk | 0x0ad0c01c | ife_lite_0 |

For each: set bit 0, read bit 0 back, then poll bit 31 == 0. If a branch was on already, leave it on, as the
donor's `clocks_up()` does. MCLK1 (RCG 0x5024 / CBCR 0x503c) is already running from TCXO and is independent of
PLL0.

Not needed: PLL1-4, ife_0/ife_1 clocks, ife_0_gdsc, camnoc_dcd_xo (0xc194: not in the DT's cpas clock list).

### a.5 First camera-block read (proves the AHB path)

Read `0x0ac40000` (CPAS top, camera version):
- expect [23:16] = 1, [15:8] = 7, [7:0] = 5, i.e. Titan 1.7.5 (CPAS `cam_cpastop_get_hw_info`);
- `0x0ac40004` = CPAS version: 1.0.0 / 1.0.1 / 1.2.0 selects `cpastop_v175_100/101/120.h`.

**Do this read only after a.1-a.4.** Touching a camera block whose AHB clock or power domain is off can stall the
bus (a hang, not a data abort).

## (b) Rails and bandwidth votes

**What Android does.**
- camcc's RCGs vote **MMCX** (`vdd_mm`) and its PLLs vote **MX** (`vdd_mx`): CC `DEFINE_VDD_REGULATORS`, probe.
- CPAS votes AHB through `qcom,msm-bus` "cam_ahb": master 1 (AMPSS_M0) -> slave 0x24d (CAMERA_CFG), DT 4316-4319.
- CPAS votes AXI through cam_hf_1/cam_hf_2/cam_sf_1: mnoc vectors 0x88/0x91/0x89 -> 0x200 (EBI), camnoc
  0x92/0x93/0x94 -> 0x30a (DT 4327-4390).
- The CSI-PHY driver asks for an AHB SVS vote and the default AXI bandwidth before configuring (PHY `CAM_START_DEV`
  in `cam_csiphy_core_cfg`).

**What the donor does.** Only rail votes, no bandwidth: CX at SVS (128) and MX at SVS_L1 (192), through DRV 2's TCS 1
as an AMC write (DONOR `rails()` / `rpmh_write()`). The level is converted to a cmd-db position (`rail_position()`),
and the vote is given back with value 0 at the end. On yupik, camcc hangs off CX; on SM8150 it hangs off **MMCX**.

**SM8150, minimum for a slow test:**

1. **`mmcx.lvl` ≥ SVS (128).** ife_lite_0_clk_src at 400 MHz needs VDD_LOW = SVS; everything else needs LOW_SVS.
   - Without a vote of our own, MMCX sits at whatever the display and boot chain left. The DT has a proxy vote of
     0x181 at boot and an init level of 0x41; neither is guaranteed under iOS.
   - The value written is the **position** (hlvl index) of 128 in cmd-db's level list for `mmcx.lvl`, as the
     donor's `rail_position()` computes it.
2. **`mx.lvl` ≥ SVS_L1 (192)**, for PLL0 at 1200 MHz (a.2).
3. **`ldoc3` (PM8150L L3, 1.2 V) enabled**: the CSI-PHY analog supply. `redwood_r5q_rpmh_enable("ldoc3")` already
   exists (R5Q). Display/DSI PHYs probably share it and keep it on, but this is not verified.
4. **Bandwidth: probably none needed.** UP `sm8150.c`:
   - BCM **MM0** (`qns_mem_noc_hf`) is `keepalive`. So are MC0 (DDR) and SH0 (LLCC).
   - BCM **MM1** holds camnoc_hf0/hf1 (uncomp and comp) **together with `qxm_mdp0/mdp1`**, so it is non-zero
     while the display scans out.
   - The CNoC path to CAMERA_CFG is already working: camcc at 0xad00000 answers.
   - If writes stall (WM0 status shows no progress, no SMMU fault, CAMNOC errlogger quiet), add a BCM vote for MM1
     (cmd-db "MM1"; value = commit bit 30 | valid bit 29 | x << 14 | y) as the next experiment.

**Implementation note.** Do **not** copy the donor's `rpmh_write()`: it drives TCS 1 with its own register code,
and R5Q's `rsc_write()` uses the same TCS 1 (`R5Q_RSC_TCS_ID 1`), serialized only by `rsc_busy`.
- Add a small exported helper next to `redwood_r5q_rpmh_enable()` in `redwood_wifi.c`, e.g.
  `redwood_r5q_rpmh_level(id, vlvl)`.
- It should look up the cmd-db address and level list (the donor's `rpmh_resource()` parses the aux data), then
  write +0x0 = the position through `rsc_write()`.
- Give the vote back (position 0) at teardown.

## (c) CSI-PHY 2, v1.1 (`qcom,csiphy-v1.1`), 4-lane D-PHY

Base **0x0ac67000**. Sources: PHY `csiphy_v1_1`, `csiphy_reset_reg_1_1`, `csiphy_common_reg_1_1`,
`csiphy_2ph_v1_1_reg`, `csiphy_irq_reg_1_1`; order from `cam_csiphy_enable_hw` -> `cam_csiphy_reset` and
`cam_csiphy_config_dev`.

Table entries are `{addr, data, delay_ms, type}`. **Delays are milliseconds** (`usleep_range(delay*1000, ...)`); the
donor spins 5 µs and 1 µs instead.

Preconditions:
- csiphy2_clk and csi2phytimer_clk on (a.4);
- cphy_rx at 400 MHz and the timer at 300 MHz;
- ldoc3 on.

### c.1 Lane mask, lane enable and settle count

- CamX lane_mask for 4 lanes = 0x1f: bit 0 DL0, bit 1 the clock lane, bits 2-4 DL1-DL3. This matches the module's
  lane assignment 0x3210.
- `cam_csiphy_config_dev` turns it into **lane_enable = 0xD5**: DL n at bit 2n, the clock lane at bit 7.
  0xD5 is also the table's default.
- Settle count: the module file's mode value **14 = 0x0E**, written as the low byte to each lane's `+0x08`.
  - Downstream computes `settle_time / 200000000` from the value CamX passes; how CamX scales the module's number
    is not visible here.
  - The donor writes the module value directly (it worked on yupik's v1.2.1), and 0x0E is also the v1.1 table's
    default.
  - Start with 14. If CSID shows SOT/ECC/CRC errors, sweep it (a `cam_settle=` boot-arg, as the donor has).

### c.2 Reset (`csiphy_reset_reg_1_1`)

| Reg | Value | Delay |
| --- | --- | --- |
| 0x0ac67814 | 0x00 | 5 ms |
| 0x0ac67818 | 0x00 | |
| 0x0ac6781c | 0x00 | |
| 0x0ac67800 | 0x01 | 1 ms |
| 0x0ac67800 | 0x00 | |

### c.3 Common (`csiphy_common_reg_1_1`)

| Reg | Value | Delay |
| --- | --- | --- |
| 0x0ac67814 | **0xD5** (lane_enable) | |
| 0x0ac67818 | 0x01 | |
| 0x0ac6781c | 0x02 | |
| 0x0ac67800 | 0x01 | 1 ms |
| 0x0ac67800 | 0x00 | |

Different from the donor's v1.2.1: there, 0x800 goes 0x03 then 0x02 (D-PHY).

### c.4 Per lane (`csiphy_2ph_v1_1_reg`, 14 writes each, in this order)

Lane blocks: DL0 = +0x000, CLK = +0x700, DL1 = +0x200, DL2 = +0x400, DL3 = +0x600. The table is processed in
lane_pos order 0..4 = DL0, CLK, DL1, DL2, DL3.

| # | Offset in block | Data lane | Clock lane (+0x700) |
| --- | --- | --- | --- |
| 1 | +0x30 | 0x00 | 0x00 |
| 2 | +0x04 | 0x0C | 0x0C |
| 3 | +0x20 | 0x00 | 0x00 |
| 4 | +0x2C | 0x01 | 0x01 |
| 5 | +0x34 | 0x0F | 0x0F |
| 6 | +0x1C | 0x0A | 0x0A |
| 7 | +0x14 | 0x60 | 0x60 |
| 8 | +0x28 | 0x00 | **0x04** |
| 9 | +0x3C | 0xB8 | 0xB8 |
| 10 | +0x00 | **0x90** | **0x80** |
| 11 | +0x08 | settle (0x0E) | settle (0x0E) |
| 12 | clock lane only: +0x0C | — | **0x16** |
| 12/13 | +0x10 | 0x52 | 0x52 |
| 13/14 | +0x38 | **0xFE** | **0x1F** |
| last | 0x0ac67800 (common) | 0x02 | 0x02 |

So DL0 writes 0xac67030, 0xac67004, ... 0xac67038, then 0xac67800 = 2. CLK writes 0xac67730 ... 0xac67738, then
0x800 = 2. DL1/DL2/DL3 are the same as DL0 at +0x200/+0x400/+0x600.

v1.1 has **no** 0x9xx/0xaxx/0xbxx/0xc80 second blocks. The donor's `csiphy_dphy()` writes `m+0x0/0x4/0x8` there,
and `b+0x10` = 0x02, `b+0x1c` = 0x08, `b+0x34` = 0x07, `0x884`: drop all of those.

### c.5 Interrupt masks and clear (`csiphy_irq_reg_1_1`; the same values as the donor's `irq_masks[]`)

- 0x0ac6782c..0x0ac67854, eleven words: FF FF FB FF 7F FF FF EF FF FF FF.
- Clear: write 0xFF to 0x0ac67858 + 4·i (i = 0..10), then 0x0ac67828 = 1, then 0.
- Status, polled: 0x0ac678b0 + 4·i (`mipi_csiphy_interrupt_status0_addr` = 0x8B0).

Teardown: the c.2 sequence again (PHY `cam_csiphy_shutdown` calls `cam_csiphy_reset`).

### c.6 When

Configure the PHY **after** the sensor's init + res2 tables and **before** its stream-on (0x0100 = 1), as the
donor's `capture_sensor()` does (`csiphy_up` just before `sensor_table("on")`). The PHY then sees LP-11 -> HS
correctly at the first frame.

## (d) CSID-lite 0 (`qcom,csid-lite175` = `cam_ife_csid_lite_17x_reg_offset`; same map as the donor's)

Base **0x0acc8000**. Common offsets: HW_VERSION 0x00, RST_STROBES 0x10, TOP_IRQ_STATUS/MASK/CLEAR 0x70/0x74/0x78,
IRQ_CMD 0x80.

| Group | Registers |
| --- | --- |
| RX | IRQ_STATUS/MASK/CLEAR 0x20/0x24/0x28; CFG0 0x100, CFG1 0x104, CAPTURE_CTRL 0x108; TOTAL_PKTS 0x160, STATS_ECC 0x164, TOTAL_CRC_ERR 0x168 |
| RDI0 | IRQ_STATUS/MASK/CLEAR 0x30/0x34/0x38; CFG0 0x200, CFG1 0x204, CTRL 0x208 |
| RDI0 drop/subsample | FRM_DROP_PATTERN/PERIOD 0x20c/0x210, IRQ_SUBSAMPLE_PATTERN/PERIOD 0x214/0x218 |
| RDI0 crop and pixel/line drop | HCROP/VCROP 0x21c/0x220, PIX_DROP_PATTERN/PERIOD 0x224/0x228, LINE_DROP_PATTERN/PERIOD 0x22c/0x230 |
| RDI0 status | STATUS 0x250, FORMAT_MEASURE0..2 0x278-0x280, SOF/EOF timestamps 0x290-0x2ac, BYTE_CNTR_PING/PONG 0x2e0/0x2e4 |

**The CSID's IRQ status registers latch with their mask at 0.** CSID `cam_ife_csid_reset_retain_sw_reg` writes
TOP_IRQ_MASK = 0 and then polls TOP_IRQ_STATUS bit 0. So polling works without enabling anything. Clearing:
- write the bits to the matching `*_IRQ_CLEAR`;
- then write **IRQ_CMD (0x080) = 1**.

### d.1 Global reset (CSID `cam_ife_csid_global_reset`)

1. Masks to 0: 0xacc8024 (RX), and 0xacc8034/44/54/64 (RDI0-3).
2. Clear everything:
   - 0xacc8078 = 1;
   - 0xacc8028 = 0x0FFFFFFF;
   - 0xacc8038/48/58/68 = 0x7FFF;
   - 0xacc8080 = 1.
3. 0xacc8104 (RX_CFG1) = 0x80. 0xacc8200/0x300/0x400/0x500 (RDIn_CFG0) = 0x2.
4. **HW reset:** 0xacc8010 = **0x1E** (csid_rst_stb). Poll 0xacc8070 bit 0 (100 ms max, 1 ms steps). Then clear:
   0xacc8078 = 1 and 0xacc8080 = 1.
5. **SW register reset:** 0xacc8010 = **0x1** (csid_reg_rst_stb). Poll 0xacc8070 bit 0. Then clear as above.

The donor resets in the other order (0x1 first, then 0x1e, from its newer driver) and sets TOP_IRQ_MASK = 1. Either
should work; SM8150's own driver does HW then SW. Read back 0xacc8000 (HW version): CSID says 1.7.0, so probably
0x1007_0000 ([31:28] major, [27:16] minor). The exact value is unverified.

### d.2 RX (CSID `cam_ife_csid_enable_csi2`, with `phy_sel = (res_type & 0xFF) − 1`)

`CAM_ISP_IFE_IN_RES_PHY_2` -> 2.

- **0x0acc8100 RX_CFG0 = 0x00232103**:
  - (lanes − 1 = 3);
  - lane_cfg 0x3210 << 4;
  - phy_sel 2 << 20;
  - lane_type 0 (D-PHY) << 24.
- **0x0acc8104 RX_CFG1 = 0x00000041**: MISR enable bit 6 | ECC correction bit 0. VC ≤ 3, so no bit 2.
- Optional mask for the record (not needed for polling): 0x0acc8024 = RST_DONE (bit 27) | lane FIFO overflows
  (bits 15-18) | CRC (19) | ECC (20) | MMAPPED_VC_DT (21) | STREAM_UNDERFLOW (23) | UNBOUNDED_FRAME (24) |
  TG FIFO overflow (26).

### d.3 RDI0 (CSID `cam_ife_csid_init_config_rdi_path`, `cam_ife_csid_get_format_rdi`)

For RAW10 written as packed MIPI (payload only), in = out = MIPI_RAW_10, which gives decode_fmt **0xF**, plain_fmt 0,
and no crop. The payload-only decode format disables crop.

| Step | Reg | Value |
| --- | --- | --- |
| 1 | 0x0acc8200 RDI0_CFG0 | **0x002BF007**: vc 0 << 22, dt 0x2B << 16, dt_id (CID) 0 << 27, fmt 0xF << 12, plain 0 << 10, timestamp enable bit 2, bits 1:0 = 3 (byte counter + format measure) |
| 2 | 0x0acc8204 RDI0_CFG1 | 0x2 (CSID_TIMESTAMP_STB_POST_IRQ) |
| 3 | 0x0acc8210 FRM_DROP_PERIOD | 1 |
| 3 | 0x0acc820c FRM_DROP_PATTERN | 0 |
| 4 | 0x0acc8218 IRQ_SUBSAMPLE_PERIOD | 1 |
| 4 | 0x0acc8214 IRQ_SUBSAMPLE_PATTERN | 0 |
| 5 | 0x0acc8224 PIX_DROP_PATTERN | 0 |
| 5 | 0x0acc8228 PIX_DROP_PERIOD | 1 |
| 6 | 0x0acc822c LINE_DROP_PATTERN | 0 |
| 6 | 0x0acc8230 LINE_DROP_PERIOD | 1 |
| 7 | 0x0acc8208 RDI0_CTRL | 0 (halt mode) |
| 8 | 0x0acc8200 RDI0_CFG0 | read \| bit 31 = **0x802BF007** (path enable) |
| 9 | 0x0acc8108 CAPTURE_CTRL | 0. Debug option: short-packet capture `1<<1`, or long-packet `1 | 0x2B<<4` |
| 10 | enable (`cam_ife_csid_enable_rdi_path`): 0x0acc8208 RDI0_CTRL | **1** (CAM_CSID_RESUME_AT_FRAME_BOUNDARY) |
| 11 | 0x0acc8034 RDI0_IRQ_MASK | 0x6. Polling does not need it; the donor uses 0x7E |

This is exactly what the donor's `csid_config()` writes; its cfg0 formula gives the same 0x002BF007.

**Alternative for a first, memory-safe test** (my suggestion, not Android's flow). Decode RAW10 to PLAIN16_10
(`get_format_rdi`: decode 0x2, plain 1) so that crop is allowed, and crop a small window. For example:
- CFG0 = 0x002B2467: fmt 2 << 12, plain 1 << 10, crop_h bit 5, crop_v bit 6, bit 2, 3;
- HCROP 0x0acc821c = (1023 << 16) | 0;
- VCROP 0x0acc8220 = (15 << 16) | 0.

That is 1024 px × 16 lines × 2 bytes = 32 KB, which fits inside the loader's existing 64 KB low-memory reservation
(see (f)).

### d.4 What to poll / read

- **RX 0x0acc8020 IRQ_STATUS (raw):**
  - bits 4-7 = DL0-3 SOT captured, bits 0-3 = EOT captured: the PHY lanes are alive;
  - bit 8 long packet, bit 9 short packet;
  - errors: 15-18 lane FIFO overflow, 19 CRC, 20 ECC, 21 MMAPPED_VC_DT, **22 UNMAPPED_VC_DT** (packets no path
    takes, e.g. embedded data, DT 0x12: benign), 23 stream underflow, 24 unbounded frame.
- **0x0acc8160 TOTAL_PKTS** (should climb), 0x0acc8164 ECC stats, 0x0acc8168 CRC errors.
- **RDI0 0x0acc8030 IRQ_STATUS:**
  - bit 12 INPUT_SOF, 9 INPUT_EOF, 11 SOL, 10 EOL;
  - 2 FIFO overflow, 13 pixel-count error, 14 line-count error, 15 CCIF violation;
  - 1 RST_DONE.
- 0x0acc8250 RDI0_STATUS; 0x0acc82e0/0x2e4 byte counters (bytes per frame should be about 1296 × 772); SOF/EOF
  timestamps 0x290/0x2a0 (they change frame to frame).

Teardown (CSID `cam_ife_csid_disable_rdi_path`, as the donor's `paths_down()`):
1. RDI0_IRQ_MASK = 0;
2. RDI0_CTRL = (RDI0_CTRL & ~3) | 2 (halt immediately);
3. RDI0_CFG0 &= ~bit 31;
4. RX_CFG0 = 0.

## (e) IFE-lite 0 (`qcom,vfe-lite175` = `cam_vfe_lite17x_hw_info`; same offsets as the donor's)

Base **0x0acc4000**.

| Group | Registers |
| --- | --- |
| Top | HW_VERSION 0x000, GLOBAL_RESET_CMD 0x018, IRQ_MASK0/1 0x05c/0x060, IRQ_CLEAR0/1 0x064/0x068, IRQ_STATUS0/1 0x06c/0x070, IRQ_GLOBAL_CLEAR 0x058, VIOLATION_STATUS 0x07c, **REG_UPDATE_CMD 0x4ac** |
| Bus | HW_VERSION 0x2000, CGC_OVD 0x200c, IRQ_MASK0-2 0x2044/48/4c, IRQ_CLEAR0-2 0x2050/54/58, IRQ_STATUS0-2 0x205c/60/64, IRQ_GLOBAL_CLEAR 0x2068, COMP_ERROR 0x206c, ADDR_SYNC_FRAME_HDR 0x2080, ADDR_SYNC_NO_SYNC 0x2084, TEST_CTRL 0x211c |
| WM0 (= RDI0) | STATUS0 0x2200, STATUS1 0x2204, CFG 0x2208, IMAGE_ADDR 0x2214, IMAGE_ADDR_OFFSET 0x2218, WIDTH 0x221c, HEIGHT 0x2220, PACKER 0x2224, STRIDE 0x2228, IRQ_SUBSAMPLE_PERIOD/PATTERN 0x2248/0x224c, FRAMEDROP_PERIOD/PATTERN 0x2250/0x2254, FRAME_INC 0x2258, BURST_LIMIT 0x225c |

RDI0's top bits (VFE `vfe17x_rdi_0_data`): reg-update data **0x2**, SOF = IRQ_STATUS0 **bit 27** (0x8000000),
reg-update done = STATUS0 bit 5 (0x20). Reset done = STATUS0 bit 31. The bus summary is STATUS0 bit 9.

### e.1 Reset (VFE `cam_vfe_top_reset`, `cam_vfe_reset`)

1. 0x0acc405c = 0x80000000 (mask all but reset).
2. 0x0acc4018 = **0x3F9F** (CAM_VFE_HW_RESET_HW_AND_REG_VAL).
3. Poll 0x0acc406c bit 31 (the donor allows 100 ms).
4. Clear: 0x0acc4064 = 0xFFFFFFFF, 0x0acc4068 = 0xFFFFFFFF, 0x0acc4058 = 1.

The IFE status registers latch without their mask: the donor polls STATUS0 bit 27 with only bit 31 masked in
(DONOR `ios_session`).

### e.2 Bus init (VFE `cam_vfe_bus_init_hw`)

| Reg | Value | Meaning |
| --- | --- | --- |
| 0x0acc6080 ADDR_SYNC_FRAME_HDR | 0 | |
| 0x0acc600c BUS_CGC_OVD | 0xFFFFF | no clock gating at bus input |
| 0x0acc611c TEST_CTRL | 0 | |
| 0x0acc6084 ADDR_SYNC_NO_SYNC | 0xFFFFFF | (1 << 24) − 1 |

The top's own `bus_cgc_ovd` (0x03c) is not written by Linux.

### e.3 WM0 in RDI frame mode (VFE `cam_vfe_bus_acquire_wm` for MIPI_RAW_10, `cam_vfe_bus_start_wm`, `cam_vfe_bus_update_wm`, `cam_vfe_bus_update_hfr`)

| Reg | Value | Meaning |
| --- | --- | --- |
| 0x0acc625c BURST_LIMIT | 0xF | |
| 0x0acc621c WIDTH | **0xFF01** | CAM_VFE_RDI_BUS_DEFAULT_WIDTH |
| 0x0acc6220 HEIGHT | 0 | |
| 0x0acc6224 PACKER | 0 | |
| 0x0acc6228 STRIDE | **0xFF01** | CAM_VFE_RDI_BUS_DEFAULT_STRIDE |
| 0x0acc6250 FRAMEDROP_PERIOD | 0 | CamX default (HFR off) |
| 0x0acc6254 FRAMEDROP_PATTERN | 1 | CamX default (HFR off) |
| 0x0acc6248 IRQ_SUBSAMPLE_PERIOD | 0 | |
| 0x0acc624c IRQ_SUBSAMPLE_PATTERN | 1 | |
| 0x0acc6048 BUS_IRQ_MASK1 | 0x1 | optional for polling |
| 0x0acc6214 IMAGE_ADDR | buffer device address | **32 bits only**, see (f) |
| 0x0acc6258 FRAME_INC | stride × height | 1296 × 772 = 1,000,512 = 0xF4440 |
| 0x0acc6208 WM0_CFG | **0x3** | bit 0 enable, bit 1 frame-based mode |
| 0x0acc64ac REG_UPDATE_CMD | **0x2** | RDI0 |

In frame mode, the WM writes the RDI payload as it arrives, one line after the other. The CSID pads each line to
the 16-byte bus width, so the stride is `RAW10_STRIDE(w) = (w*10/8 + 15) & ~15` (DONOR):
- for 1028: 1285 -> **1296 bytes**;
- for 4000: 5008 (15 MB per frame: too big, see (f)).

**There is no height bound in frame mode** (HEIGHT 0). A frame longer than expected writes past the buffer: keep a
guard band (the donor's `PLACE_SPARE` is 256 KB).

### e.4 Poll and next frame

- **Frame done:** 0x0acc6060 (BUS_IRQ_STATUS1) bit 0 = WM0 done. Clear: 0x0acc6054 = 1, then 0x0acc6068 = 1.
- **SOF:** 0x0acc406c bit 27. Clear: 0x0acc4064 = 0x08000000, then 0x0acc4058 = 1.
- Next buffer: write IMAGE_ADDR, then REG_UPDATE 0x2. The update latches at the next SOF (DONOR
  `capture_sensor`, `ios_session`).
- **Errors:** BUS_IRQ_STATUS0 bits 11-14 (0x7800) and STATUS2 bit 6 (VFE `bus_error_irq_mask`);
  0x0acc407c VIOLATION_STATUS; 0x0acc606c COMP_ERROR; 0x0acc6200/0x6204 WM0 status.

Teardown: WM0_CFG = 0, REG_UPDATE = 0x2, then wait at least one frame time (the donor waits 1 ms; use a full frame,
8.3 ms at 120 fps) **before** taking the SMMU match away.

### e.5 CAMNOC (optional; CPAS `cam_cpastop_poweron`, `cpastop_v175_101.h`)

Linux writes QoS for the IFE02 port (IFE0 plus IFE2 = IFE-lite 0, cell 2) at CAMNOC 0x0ac42000:

| Reg | Value |
| --- | --- |
| +0x430 PRIORITYLUT_LOW | 0x66666543 |
| +0x434 PRIORITYLUT_HIGH | 0x66666666 |
| +0x438 URGENCY_LOW | 0x3 (masked field) |
| +0x440 DANGERLUT | 0xFFFFFF00 |
| +0x448 SAFELUT | 0x1 |

It also enables the error logger: +0x2708 ERRLOGGER_MAINCTL = 1; IFE02 encoder error enable +0x5a0 = 1.

None of this is needed for function. Useful readbacks:
- 0x0ac44710 (+0x2710) ERRLOGGER_ERRVLD, cleared by +0x2718 = 1;
- 0x0ac42590 IFE02 ENCERRSTATUS;
- 0x0ac44048 (+0x2048) SBM_FAULTINSTATUS0.

## (f) SMMU: stream IDs, context, bypass

**DT `msm_cam_smmu_ife`** (line 4132): `iommus` = SID/mask pairs 0x840/0x620, 0x860/0x620, 0xa40/0x620, 0xa60/0x620,
0xc40/0x620, 0xc60/0x620, 0xe40/0x620, 0xe60/0x620. All four IFEs share this one context; the IFE-lites are inside
it. There is **no `qcom,iommu-s1-bypass`** on it, and IOVA region "io" = 0x07400000 + 0xd8c00000.

- **One SMR covers all eight:** id 0x840, mask 0x620. 0x840 with bits 5, 9 and 10 free gives exactly
  0x840/0x860/0xa40/0xa60/0xc40/0xc60/0xe40/0xe60.
  - SMR value = **0x8000_0000 | 0x620 << 16 | 0x840 = 0x86200840**.
  - Which of the eight IFE-lite 0 actually emits could not be determined from the sources. The SIDs span both
    `mnoc_hf_0_tbu` (0x800-0xbff) and `mnoc_hf_1_tbu` (0xc00-0xfff), DT ~25040/25010.
  - The donor's 0x800/0x4e0 is yupik's.
- **No clash with the display:** the MDP's SMR is 0x800/0x420 (DT 885), which covers 0x800/0x820/0xc00/0xc20, not
  0x840+. Display DMA already crosses both hf TBUs, so their GDSCs (`hlos1_vote_mmnoc_mmu_tbu_hf0/hf1`, GCC
  0x17d050/0x17d058) are on while the display runs.

**What the donor does** (DONOR `smmu_find`, `smmu_up`):
- takes a free SMR and a context bank no stream uses;
- programs it as stage 1 with its own 3-level page tables: CBAR type 1, CBA2R 64-bit, TCR 39-bit, MAIR0 0xf404ff44,
  TTBR0 = its tables with ASID = bank;
- maps IOVA 0x40000000 onto the frames;
- invalidates the TLB, sets SCTLR = 0x1127;
- then S2CR = bank and SMR = valid | 0x4e0 << 16 | 0x800.

**What is proven on this phone** (R5Q):
- sCR0 has USFCFG set: **unmatched streams fault**. Test 112: the WLAN's first DMA without a match **reset the
  phone**.
- The hypervisor **refuses S2CR bypass**.
- A context bank of our own **faulted** (Test 114: CB55 FSR 0x2; "the hypervisor set nothing up behind it").
- What works, three times (Wi-Fi 0x640, SLIMbus BAM, FastRPC 0x5a1-0x5a3):
  - give the new SMR a **copy of the S2CR of an existing valid match whose context bank has translation off**:
    S2CR type 0, CB SCTLR bit 0 = 0, CBAR == 0x1f000;
  - the device then emits **physical addresses**.
- Free-SMR convention: Wi-Fi takes the lowest free SMR and the SLPI the highest. Pick another (e.g. the second
  highest) and hold the shared SMMU lock.

**Recommendation for SM8150:**

1. **Path A (proven mechanism):** SMR 0x86200840 in a free slot, with S2CR = a copy of a translation-off boot bank's
   S2CR (exactly `r5q_smmu_look()`'s search).
   - IMAGE_ADDR = the buffer's **physical** address, which must be **< 4 GB** (IMAGE_ADDR is 32 bits).
   - iOS has no RAM below 4 GB except the loader's 64 KB `lowdma_pa` (`r5q_dma_area`). So either:
     - (i) the loader reserves a larger low block (about 3 MB: two 1028x772 frames of 0xF4440 plus guard), or
     - (ii) the first test uses the CSID crop (d.3) for a 32 KB frame inside the existing 64 KB.
   - There is **no protection** against frame-mode overrun in this path.
2. **Path B (donor's method):** own bank with own stage-1 tables. This allows buffers above 4 GB and bounds DMA to
   mapped pages, but Test 114 suggests the hypervisor gives such a bank no stage 2. Try it only after A works, and
   watch the bank's FSR.

Install the SMR **before** WM0_CFG = 3. Remove it only after the WM is off and a frame time has passed.

Readbacks:
- SMR/S2CR read back as written;
- CB FSR = bank page + 0x58 (bank page = 0x15000000 + numpage·page + k·page; numpage and page from IDR1, as in
  `r5q_smmu_look`), FAR + 0x60, FSYNR0 + 0x68;
- global SGFSR 0x15000048 and SGFSYNR1 0x15000054 (DONOR `smmu_faults`).

## The complete order on SM8150

0. **Sensor:** powered, MCLK1 on, `init` + `res2` written, **not streaming** (as Test 275 up to before "on").
1. **RPMh:** `mmcx.lvl` ≥ 128, `mx.lvl` ≥ 192, `ldoc3` enable (b).
2. **GCC** hf_axi/sf_axi on; check ahb/xo (a.0).
3. **camcc** GDSC clk 0xc1e4, titan_top 0xc1bc (a.1).
4. **PLL0** (a.2).
5. **RCGs**: slow_ahb, camnoc_axi, cphy_rx, csi2phytimer, ife_lite_0_csid, ife_lite_0 (a.3).
6. **Branches**: cpas_ahb, camnoc_axi, csiphy2, csi2phytimer, ife_lite_0_csid, ife_lite_0_cphy_rx, ife_lite_0
   (a.4).
7. **Read** CPAS version 0x0ac40000 (a.5). Optional CAMNOC QoS (e.5).
8. **Buffer:** cache-clean (`dc civac`, as the donor), then SMMU match (f).
9. **CSID:** reset, RX, RDI0 (d.1-d.3).
10. **IFE-lite:** reset, bus init, WM0, REG_UPDATE (e.1-e.3).
11. **CSI-PHY 2:** reset, common, lanes, IRQ (c.2-c.5).
12. **Sensor stream on** (0x0100 = 0x0100, its "on" table).
13. **Poll** (e.4 / d.4), about 8.3 ms per frame at 120 fps; invalidate the cache (`dc civac`) before reading the
    frame.
14. **Teardown:**
    1. sensor "off";
    2. IFE WM0 off + REG_UPDATE, wait one frame;
    3. CSID RDI halt/off, RX_CFG0 = 0;
    4. CSI-PHY reset;
    5. **then** the SMR/S2CR restored;
    6. branches off, RCGs back to their found CFG, PLL0 off, GDSC back as found, RPMh votes back to 0.

## (g) Donor step -> SM8150

| Donor (function: what) | SM8150 | Kind |
| --- | --- | --- |
| `CAMCC_PHYS` 0xad00000, `CAMCC_SIZE` 0x20000 | 0xad00000, size 0x10000 (DT) | size differs |
| `GCC_CAMERA_AHB_CBCR` 0x26004, `_XO_` 0x26028 | 0xb008, 0xb044 (gcc base 0x100000) | **offsets differ** |
| `gdsc_on`: `TITAN_TOP_GDSCR` 0xc194 | **0xc1bc**, plus `CAM_CC_GDSC_CLK` 0xc1e4 first. 0xc194 is camnoc_dcd_xo's CBCR here | **offset differs (dangerous)** |
| `rails()`: CX 128 + MX 192 via own TCS-1 code | **MMCX** (`mmcx.lvl`) 128 + MX 192 (+ `ldoc3`), through R5Q `rsc_write` (the same TCS 1, shared lock) | resource and code path differ |
| `pll0_up`: Lucid, L 0x3e, CAL_L 0x44, alpha 0x8000, CONFIG 0x20485699/0x2261/0x329a299c, USER 0x3101/0x805/0 | Trion: L 0x3E, CAL_L 0x44, alpha 0x8000, CONFIG 0x20485699/**0x2267/0x24**, USER **0x3100**/0x805/**0xD0**, **TEST_CTL_U1 0x20**. Same register offsets, same enable sequence | values differ |
| `rcgs[]` slow_ahb 0x7058 src 2 div 14 | 0x7058, CFG 0x20e | same |
| camnoc_axi 0xc124 src 2 div 7 | **0xc170**, 0x207 | offset differs |
| cphy_rx 0xa064 src 2 div 2 | 0xa064, **0x301** (odd ÷ 1, Android's table) | value differs (both are 400 MHz) |
| csiNphytimer (csiphy2: 0xe0f4) src 2 div 3 | **0x604c**, 0x203 | offset differs |
| ife_lite_0_csid 0xc020 src 2 div 2 | 0xc020, **0x301** | value differs (both are 400 MHz) |
| ife_lite_0 0xc004 src 3 div 1 | 0xc004, 0x301 | same |
| `branches[]` gcc hf_axi 0x26010 / sf_axi 0x2601c | **0xb030 / 0xb034** | offsets differ |
| cpas_ahb 0xc11c | **0xc168** | offset differs |
| camnoc_axi 0xc140 | **0xc18c** | offset differs |
| csiphy2 0xe110 | **0x6068** | offset differs |
| csi2phytimer 0xe10c | **0x6064** | offset differs |
| ife_lite_0_csid 0xc038, _cphy_rx 0xc040, ife_lite_0 0xc01c | 0xc038, 0xc040, 0xc01c | same |
| `frames_alloc`: `kmem_alloc_contig` anywhere | physical **< 4 GB** for path A (loader block or crop test) | differs |
| `smmu_find/smmu_up`: own CB + own S1 tables, SMR 0x800/0x4e0, IOVA 0x40000000 | SMR **0x840/0x620**; S2CR = copy of a translation-off boot bank (R5Q method); IOVA = PA | **method and values differ** |
| `CSID_PHYS` 0xacc8000, `IFE_PHYS` 0xacc4000 | same | same |
| `csid_reset` | same registers. SM8150's order is HW (0x1e) then SW (0x1) | same map, order differs (either OK) |
| `csid_config`: RX_CFG0 = (lanes−1) \| lane_cfg << 4 \| cphy << 24 \| phy << 20; CFG1 0x41; RDI0 0x2BF007 ... CTRL 1 | identical (0x232103, 0x41, 0x802BF007) | same |
| `ife_config`: reset 0x3f9f, bus init, WM0 0xff01/0/0/0xff01, cfg 3, REG_UPDATE 0x2 at 0x4ac | identical (`cam_vfe_lite17x.h`) | same |
| `CSIPHY_PHYS(n)` = 0xace0000 + 0x2000·n | **0xac65000 + 0x1000·n** (CSI-PHY 2 = 0xac67000) | **base differs** |
| `csiphy_up` reset: µs delays | same registers, **ms** delays (5 ms, 1 ms) | timing differs |
| `csiphy_common`: 0x800 = 3 then 2 (D-PHY) | 0x800 = **1 (1 ms) then 0**; 0x814 = 0xD5, 0x818 = 1, 0x81c = 2 | values differ |
| `csiphy_dphy` (v1.2.1, two blocks per lane) | v1.1 table (c.4): one block per lane, other values (0x90/0x80, 0x52, 0x0F, 0x0A, 0x16) | **table differs** |
| `csiphy_cphy` | not used (D-PHY sensor) | — |
| `csiphy_up` IRQ masks + clear (0x82c.., 0x858.., 0x828) | identical values and offsets | same |
| `show_csiphy` 0x8b0 + 4i | same | same |
| polling `BUS_IRQ_STATUS1` bit 0 / `IFE_IRQ_STATUS0` bit 27 | same | same |
| `titan_down` order | same, but keep the SMR until the WM is off + one frame | — |

## (h) Biggest risks, and what to read at each step

1. **Unmatched SID -> phone reset** (Test 112). Have the SMR in place and read back before WM0_CFG = 3. Keep it
   until after the WM is off.
2. **Frame-mode overrun** writes past the buffer. With path A there is no SMMU bound: it lands in physical memory.
   - Give it a generous guard.
   - Verify the CSID byte counter (0xacc82e0) and line count before trusting the size.
   - First test: the crop variant (32 KB) or a single frame.
3. **Bus stall** from touching 0xac4xxxx/0xac6xxxx/0xaccxxxx with the power domain or clocks off. Order a.1-a.4
   strictly before any read. Read 0xac40000 first: a sane 1.7.5 means the AHB path is good.
4. **Donor constants:** GDSC 0xc194, the CSI-PHY base, the GCC offsets and the camnoc/cpas_ahb/csiphy branches all
   differ (g). Port by table, not by copy.
5. **RPMh TCS collision:** the donor's own TCS-1 code against R5Q's `rsc_write` (also TCS 1). Use one helper.
6. **Rail too low:** ife_lite at 400 MHz needs MMCX SVS; PLL0 at 1.2 GHz needs MX SVS_L1. A symptom is random
   hangs or garbage. Vote both; read back the TCS command status (bit 16 completed).
7. **Settle count / lane errors.** Read the CSID RX status: SOT bits 4-7 for all four lanes, ECC bit 20, CRC bit 19,
   TOTAL_PKTS climbing, and the CSI-PHY IRQ status 0xac678b0..8d8. Sweep the settle count if needed.
8. **CSI-PHY supply** `ldoc3` off: no SOTs at all. Read L3C over the SPMI observer, or just vote it on.
9. **PLL0 already owned** (boot chain or another driver). Check MODE/OPMODE first.
10. **Low-memory budget:** the 64 KB `lowdma_pa` is shared with SLIMbus/FastRPC. Full frames need a loader change.
11. **Cache:** the IFE is not IO-coherent here (the donor cleans before and invalidates after). Read the frames
    through `dc civac` or an uncached mapping.
12. **Bandwidth:** if writes stall with no SMMU fault and no CAMNOC error, vote BCM MM1 (b).

**Readback checklist per step:**

| Step | Read | Expect |
| --- | --- | --- |
| RPMh | TCS command status | bit 16 set |
| GCC | 0x10b030 / 0x10b034 | bit 31 = 0 |
| GDSC | 0xad0c1bc | bit 31 = 1 |
| PLL0 | 0xad00000 | bit 31 = 1; OPMODE 0xad00038 = 1; USER_CTL 0xad0000c = 0x3107 |
| RCGs | each CMD | bit 0 = 0 and (once a branch is on) bit 31 = 0; CFG reads back as written |
| Branches | each CBCR | bit 31 = 0 |
| CPAS | 0xac40000 | 0x00010705; 0xac40004 = CPAS version |
| SMMU | SMR / S2CR | as written; CB FSR 0; SGFSR 0 |
| CSID reset | 0xacc8070 | bit 0 seen twice; 0xacc8000 a non-zero version |
| CSID config | RX_CFG0 0xacc8100 / RDI0_CFG0 0xacc8200 | 0x232103 / 0x802BF007 |
| IFE reset | 0xacc406c | bit 31; 0xacc4000 / 0xacc6000 non-zero versions |
| WM0 | 0xacc6208 | 3 |
| CSI-PHY | 0xac67814 | 0xD5; lane registers read back (the donor's `blk_put` checks them) |
| After stream-on: CSID | 0xacc8020 SOT bits; 0xacc8160 TOTAL_PKTS; 0xacc8030 SOF/EOF bits | lanes alive, count increasing, SOF/EOF seen |
| After stream-on: IFE | 0xacc406c bit 27 (SOF); 0xacc6060 bit 0 (WM0 done) | both seen |
| After stream-on: errors | 0xacc6200/0x6204, 0xacc407c, CAMNOC 0xac44710, SMMU CB FSR | quiet |

## Not determined from the sources

- Which of the eight IFE SIDs IFE-lite 0 emits, and on which hf TBU. The single 0x840/0x620 SMR makes this moot.
- Whether this phone's hypervisor gives a self-programmed context bank a usable stage 2 (Test 114 says probably
  not). Hence path A.
- The exact HW_VERSION values of CSID-lite 175 and IFE-lite 175 (expected major 1 minor 7; not verified).
- How CamX scales the module's settle value into `settle_time` (downstream divides by 200,000,000). 14 is used as
  the count; it is also the v1.1 default.
- Whether MMCX, MX, `ldoc3` and BCM MM1 are already high enough under iOS. Read the TCS/cmd-db state, or vote
  anyway.
- Whether the CAMNOC QoS writes matter for a slow RDI-only test (assumed not).
- Whether the S10 Lite's Samsung kernel (not available here) deviates from the LineageOS/CAF SM8150 camera driver.
  The DT compatibles (`csiphy-v1.1`, `csid-lite175`, `vfe-lite175`, `camcc-sm8150-v2`) match it.
