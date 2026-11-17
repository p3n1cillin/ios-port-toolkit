# S10 Lite (r5q): hypervisor, reset and watchdog facts for the hand-off tests

Host-only research, 2026-11-16. Sources: the phone's live device tree (`backups/<SERIAL>/live.dtb`), the
hardware-verified Test 12 firmware source, and Samsung's SM-G770F GPL kernel at commit
`153e588c8576dcf60cdc4d3a31765ee8721bfec3` (sparse subset in `/var/tmp/codex-ios-r5q/work/research-src/sm-g770f-kernel`,
fetched by `fetch_samsung_kernel_subset.sh`; greps in `research/grep_samsung_*.sh`).

## What the phone runs

| Fact | Evidence |
|---|---|
| Samsung uH hypervisor with RKP/KDP/CFP | `r5q_eur_open_defconfig`: `CONFIG_UH=y`, `CONFIG_UH_RKP=y`, `CONFIG_RKP_KDP=y`, `CONFIG_RKP_CFP_ROPP_SYSREGKEY=y`; DT `rkp_region@B0200000` (= `UH_LOG_START`) |
| RKP is started by Android's kernel, not before | `include/linux/rkp.h` `RKP_START`/`RKP_DEFERRED_START`, issued via `uh_call` (`smc #0`, `arch/arm64/kernel/uh_entry.S`). Our UEFI-stage loader runs before any of that |
| No kexec on this kernel | `# CONFIG_KEXEC is not set` |
| Debug level LOW | DT bootargs `androidboot.debug_level=0x4f4c` |
| Download (upload) mode is cleared on every normal reboot | `sec_debug.c` reboot notifier: `set_dload_mode(0)`, restart reason NORMAL; `msm-poweroff.c` then uses a PS_HOLD hard reset |
| APSS watchdog is HLOS-owned, plain MMIO at EL1 | DT `qcom,wdt@17c10000` (`qcom,msm-watchdog`, bark 0x2af8 ms); `watchdog_v2.c`: `WDT0_RST 0x04`, `EN 0x08`, `STS 0x0C`, `BARK_TIME 0x10`, `BITE_TIME 0x14`, 32765 Hz |
| Samsung forces a reset by watchdog bite | `sec_debug.c`: `EN=1`, `BITE_TIME=1`, `RST=1` ("Causing a watchdog bite!") |
| The firmware's proven self-reset | Test 12 `FirstBootManager.c`: `MmioWrite32(0x0C264000 /*PS_HOLD*/, 0)`; brought Tests 5/9/13/17 back with the RAM log intact |
| Our loader's 60 s "watchdog" did nothing on r5q | `loader.c` `wdt_arm()` only arms the hypervisor virtual watchdog (SMC 0x86000005-8) on `platform_redwood`; the R5Q test path never calls it |

## What it means for the hand-off hang (Tests 15/16/18)

- RKP's MMU/page-table policing is not active yet when our loader runs, so it is not the likely culprit.
- The bootloader (ABL, also a UEFI app) turns the MMU off at EL1 under this same hypervisor on every Android
  boot, so "MMU off" by itself is allowed. Coming back (MMU and caches on again with UEFI's tables), or the
  TLB/I-cache maintenance the stub does in between, is the less-trodden path. Linux never turns the MMU off and
  on again at EL1 here (no kexec). Prediction to check: Tests 19 to 21 return, 22 (after the MMU is on again) is
  the first to fail. Only the phone can confirm it.
- A watchdog bite during a test should reboot straight to Android (download mode is off). That makes an armed
  watchdog a candidate safety net for every risky test. It is unproven, so Test 24 checks it first.

## Built from this (all host-checked, unflashed, start-guarded)

- **Test 24**, watchdog probe: Test 19's path, then arm the APSS watchdog (bark 5 s, bite 8 s) and wait without
  petting. If no reset within 20 s by the virtual counter, fall back to PS_HOLD. 11 shim + 5 roundtrip Unicorn
  cases pass. Recovery image `d125d9ef…`.
- Suggested order once the phone is back: **19 → 24 → 20 → 21 → 22 → 23**. If 24 bites cleanly, Tests 20 to 23
  can be rebuilt with the watchdog armed before the MMU-off jump, so a hang reboots by itself.
