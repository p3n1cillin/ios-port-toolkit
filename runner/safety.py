"""The port's safety rules, as checks a tool must pass before it touches a phone.

These are the rules every hardware test on the S10 Lite ran under (Tests 1-68). They are not
optional and no flag turns them off:

  R1  A test writes ONLY the recovery partition. Never boot, vbmeta, dtbo, xbl, abl or any other.
  R2  Every run restores the original recovery automatically when Android comes back, and verifies
      recovery, boot and vbmeta against the baseline hashes before and after.
  R3  Entering the test is reboot(2) RESTART2 "recovery": no BCB/misc write, no fastboot, and the
      operator refuses if a bootloader command is already pending in misc.
  R4  No PSCI SYSTEM_RESET/OFF from our code paths and no PMIC writes: resets are the firmware's
      PS_HOLD; drivers read PMICs through the SPMI observer channel only; nothing writes a PMIC register.
  R5  Download Mode restore flashes ONLY the original recovery image (hash-checked), with no
      repartition / PIT / other partitions.
  R6  One run per authorization (or a standing authorization the owner gave in words, recorded with
      the run). Never chain or retry tests automatically.
  R7  Wiping Android (installing iOS onto userdata / repartitioning) needs a downloaded stock firmware
      for the exact model first AND the owner's explicit confirmation for that step. No tool here does it.

    from safety import check_bundle, assert_shell_safe, assert_download_restore
"""
import hashlib
import json
import pathlib
import re

RULES = __doc__

FORBIDDEN_PARTITIONS = {'boot', 'vbmeta', 'vbmeta_samsung', 'vbmeta_system', 'dtbo', 'xbl', 'xbl_config', 'abl',
                        'aop', 'tz', 'hyp', 'keymaster', 'devcfg', 'param', 'misc', 'efs', 'sec_efs', 'persist',
                        'userdata', 'super', 'modem', 'dsp', 'bluetooth', 'vendor_boot', 'init_boot', 'up_param',
                        'steady', 'keyrefuge', 'frp', 'pit'}


class SafetyError(RuntimeError):
    pass


def sha256(path) -> str:
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


FASTBOOT_ALLOWED = {'boot', 'getvar', 'devices', 'reboot'}
FASTBOOT_GETVARS = {'product', 'unlocked', 'serialno', 'current-slot', 'slot-count', 'version-bootloader',
                    'max-download-size', 'is-userspace', 'secure', 'variant', 'all'}


def assert_fastboot_safe(argv: list, profile: dict):
    """R1 for fastboot-boot phones: a test only ever RAM-boots. Allowed: `fastboot [-s S] boot <image>`,
    `getvar <read-only var>`, `devices`, plain `reboot` (back to Android). Refused: flash, erase, format,
    set_active, oem, flashing, update, boot with --slot or extra arguments, reboot bootloader/recovery/fastboot/edl."""
    args = list(argv)
    if args and pathlib.Path(args[0]).name.lower().startswith('fastboot'):
        args = args[1:]
    while args and args[0] in ('-s',):
        args = args[2:]
    if not args:
        raise SafetyError('fastboot: no command')
    verb = args[0]
    if verb not in FASTBOOT_ALLOWED:
        raise SafetyError(f'R1: fastboot {verb} is not allowed: tests only RAM-boot (fastboot boot <image>)')
    if verb == 'boot' and (len(args) != 2 or args[1].startswith('-')):
        raise SafetyError('R1: fastboot boot takes exactly one image and no options')
    if verb == 'getvar' and (len(args) != 2 or args[1] not in FASTBOOT_GETVARS):
        raise SafetyError(f'fastboot getvar {args[1:]} is not on the read-only list')
    if verb == 'reboot' and len(args) > 1:
        raise SafetyError('R3: only a plain `fastboot reboot` (back to Android)')
    if (profile.get('flash', {}).get('method') or 'recovery') != 'fastboot-boot':
        raise SafetyError('this profile does not use fastboot-boot (flash.method)')


def assert_profile_safe(profile: dict):
    flash = profile.get('flash', {})
    if (flash.get('method') or 'recovery') == 'fastboot-boot':
        if flash.get('test_partition'):
            raise SafetyError('R1: a fastboot-boot profile writes no partition (flash.test_partition must be null)')
        never = set(flash.get('never_write') or [])
        for p in ('boot', 'vbmeta'):
            if p not in never:
                raise SafetyError(f'R1: flash.never_write must list {p}')
        return
    if flash.get('test_partition') != 'recovery':
        raise SafetyError('R1: flash.test_partition must be "recovery"')
    never = set(flash.get('never_write') or [])
    for p in ('boot', 'vbmeta'):
        if p not in never:
            raise SafetyError(f'R1: flash.never_write must list {p}')
    base = profile.get('baseline', {})
    for k in ('recovery_sha256', 'boot_sha256', 'vbmeta_sha256'):
        if not re.fullmatch(r'[0-9a-f]{64}', str(base.get(k) or '')):
            raise SafetyError(f'R2: baseline.{k} must be known before any hardware test')


def assert_shell_safe(command: str, profile: dict):
    """A root shell command a tool is about to run on the phone: refuse anything that writes outside recovery.

    Allowed writes: dd of=/dev/block/by-name/recovery (or the profile's resolved recovery node), files under
    /data/local/tmp/<prefix>-test*. Refused: any other of=/dev/block/..., fastboot, setprop of boot control,
    reboot into anything but recovery through the helper, echo/cat redirection into /dev/block, PMIC debugfs writes.
    """
    rec = profile.get('flash', {}).get('recovery_block_device')
    targets = re.findall(r'\bof=(\S+)', command)
    for t in targets:
        t = t.strip("'\";")
        if t.startswith('/data/local/tmp/'):
            continue
        if t in ('/dev/block/by-name/recovery', rec):
            continue
        raise SafetyError(f'R1: refusing a write to {t}: tests write only recovery')
    if re.search(r'>\s*/dev/block/', command):
        raise SafetyError('R1: refusing a redirection into a block device')
    if re.search(r'\b(fastboot|flash_image|blockdev\s+--setrw|sgdisk|parted|mkfs|wipe|erase)\b', command):
        raise SafetyError('R1/R7: refusing a partitioning/flash/wipe tool')
    if re.search(r'/sys/kernel/debug/regmap/.*/data|spmi.*\bwrite\b|\bpmic\b.*\bwrite', command):
        raise SafetyError('R4: refusing a PMIC/regmap write')
    if re.search(r'\breboot\s+(bootloader|download|edl|fastboot)\b', command):
        raise SafetyError('R3: only the reboot-recovery helper enters a test')
    if re.search(r'\bsetprop\s+(sys\.powerctl|ro\.boot|persist\.sys\.boot)', command):
        raise SafetyError('R3: refusing a boot-control property')


def check_bundle(bundle: pathlib.Path, profile: dict) -> list:
    """Static checks of a test bundle before a run. Returns the list of checks passed; raises on the first failure."""
    assert_profile_safe(profile)
    passed = []
    bundle = pathlib.Path(bundle)
    m = json.loads((bundle / 'manifest.json').read_text())
    img = bundle / m['image']['name']
    if sha256(img) != m['image']['sha256']:
        raise SafetyError('the test image does not match its manifest hash')
    passed.append('test image matches its manifest')
    rb = profile['flash'].get('recovery_bytes')
    if rb and img.stat().st_size != rb:
        raise SafetyError(f'the test image is {img.stat().st_size} bytes; the recovery partition is {rb}')
    passed.append('test image is exactly the recovery partition size')
    base = profile['baseline']
    if m['original_recovery']['sha256'] != base['recovery_sha256']:
        raise SafetyError('R2: the bundle restores a recovery that is not the baseline')
    orig = bundle / 'restore' / 'recovery-original.img'
    if not orig.exists() or sha256(orig) != base['recovery_sha256']:
        raise SafetyError('R2/R5: restore/recovery-original.img is missing or not the baseline recovery')
    passed.append('original recovery for restore is present and matches the baseline')
    if m.get('original_boot_sha256') != base['boot_sha256'] or m.get('original_vbmeta_sha256') != base['vbmeta_sha256']:
        raise SafetyError('R2: the bundle baseline for boot/vbmeta differs from the profile')
    passed.append('boot/vbmeta baseline matches the profile')
    for ops in bundle.glob('Manage-Test*.ps1'):
        text = ops.read_text(errors='replace')
        for t in re.findall(r'\bof=(\S+?)[;\s\'"]', text):
            if t not in ('/dev/block/by-name/recovery',):
                raise SafetyError(f'R1: operator {ops.name} writes {t}')
        if 'by-name/misc' in text and 'od -An' not in text:
            raise SafetyError('R3: operator touches misc other than to read it')
        if "start_blocked_reason" not in text:
            raise SafetyError('R6: operator lacks the start guard (manifest start_blocked_reason)')
        passed.append(f'{ops.name}: writes only recovery, reads misc only, has the start guard')
    return passed


def assert_download_restore(image: pathlib.Path, profile: dict, partition: str):
    """R5: the only thing a Download Mode restore may flash."""
    if partition.upper() != 'RECOVERY':
        raise SafetyError('R5: Download Mode restore flashes RECOVERY only')
    if sha256(image) != profile['baseline']['recovery_sha256']:
        raise SafetyError('R5: that image is not the original recovery')


def assert_wipe_allowed(stock_firmware: pathlib.Path | None, owner_confirmation: str | None):
    """R7: what a future installer must have before it may touch userdata or the partition table."""
    if not stock_firmware or not pathlib.Path(stock_firmware).exists():
        raise SafetyError('R7: download the stock firmware for this exact model first (the way back)')
    if not owner_confirmation or 'wipe' not in owner_confirmation.lower():
        raise SafetyError('R7: the owner must confirm the wipe explicitly, in words, for this step')
