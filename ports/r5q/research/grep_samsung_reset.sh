#!/bin/bash
# Read-only greps over the pinned Samsung SM-G770F kernel subset (reset, download mode, debug level).
cd /var/tmp/codex-ios-r5q/work/research-src/sm-g770f-kernel || exit 1
F=drivers/power/reset/msm-poweroff.c
echo "== $F"
grep -n "download_mode\s*=\|static int download_mode\|scm_disable_sdi\|set_dload_mode(\|sec_debug\|SEC_DEBUG\|DLOAD\|in_panic\|deassert_ps_hold\|PS_HOLD\|warm\|RESTART_REASON\|qpnp_pon_system_pwr_off\|PON_POWER_OFF" "$F" | head -60
echo "== sec_debug download/upload decisions"
grep -rn "sec_debug_is_enabled\|SEC_DEBUG_LEVEL_LOW\|debug_level\s*==\|set_dload_mode\|force_upload\|upload_cause\|UPLOAD_CAUSE_WATCHDOG\|wdog\|watchdog" drivers/samsung/debug/*.c include/linux/samsung/debug/*.h 2>/dev/null | head -40
