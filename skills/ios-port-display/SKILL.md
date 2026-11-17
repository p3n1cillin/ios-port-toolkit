---
name: ios-port-display
description: Diagnose and fix the panel picture of a native iOS port on a Qualcomm phone (MDSS/DPU + DSI command-mode panels) - shifted or wrapped picture, wrong position, tearing, flips failing, framebuffer placement. Use when the screen shows UEFI/iOS misaligned, garbled, small, or tearing.
---

# Display bring-up (MDSS/DSI, command-mode panels)

Everything here was learned on the S10 Lite in Tests 52-68. The register layout is SM8150/SM8250 DPU.
Profile fields: `display.*` and `memory.display_reserved`.

## Read before you write

- A read of a block whose clock is off can stop the SoC. Check GCC `disp_ahb` (`display.gcc_disp_ahb_cbcr`) and
  the dispcc MDSS GDSC first.
- On SM8150 that branch is HW_CTL: CLK_OFF reads 1 whenever the bus is idle (0x80000003). That does **not** mean
  the clock is lost (Test 67's flip gave up on exactly this).
- Dump first, change later. Read CTL0's layer mix, every SSPP's source address, size, stride and format, INTF1's
  tear check and autorefresh (+0x2B4), and DSI0's control and command DMA registers. The S10 Lite had
  `[MDSS]`/`[SCAN]` dumps in the firmware boot manager (port/r5qfull/R5qScanout.c).

## Symptom → cause seen on the S10 Lite

| Symptom | Cause | Fix |
|---|---|---|
| UEFI cursor or iOS shifted per boot, lines wrapped. BootShim bands clean | The panel's own write window (DCS 2A/2B) was garbled early in boot, so each frame starts some rows in | After a DSI reset, resend `13`, `2A 00 00 (w-1)`, `2B 00 00 (h-1)` by command DMA (R5qPanelSync.c, Test 61) |
| DSI reset "did nothing" | DSI 6G registers are shifted by 4 (Linux `DSI_6G_REG_SHIFT`): RESET is 0x118, not 0x114 | Use the shifted offsets (`display.dsi_reset_reg`) |
| Command DMA "no DMA done" | MASK_CMD_DMA_DONE off; own buffer not visible | Enable the done mask and use the bootloader's DMA buffer (`display.abl_command_buffer`), released afterwards |
| iOS fails: "no memory at the resident address" | The panel command page was claimed inside the resident image range | Release the page after the commands (Test 63) |
| Tearing | One buffer, CPU copy slower than the 60-120 Hz scan | Flip between 2-3 buffers inside Display Reserved, swap the pipe source at vblank (`display.flip_buffers`) |
| Picture small, top-left, old screen around it | The flip/scaler set up, then the first frame aborted | Check the HW_CTL clock reading above |

## Rules of thumb

- Do not kick CTL_START while autorefresh is on. Turn autorefresh off, drain, send one clean frame, then turn it
  back to the stock value (`display.autorefresh`: 0x80000002 = every second TE).
- The TE pin needs the `mdp_vsync` function (`display.te_gpio`/`te_function`; the extractor finds both).
- Keep the BootShim record and the DSI command page in Display Reserved but outside every frame. The validator
  checks this.
- iOS draws 375 x 812 points. `fbscale=2` renders at 2 pixels per point and the pipe scaler fills the panel.
- Each display test should leave a calibration picture the owner can photograph (border, coloured corners,
  centre cross, a row ruler). The photo is the measurement.
