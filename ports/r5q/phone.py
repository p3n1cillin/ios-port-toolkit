"""Talk to the installed Galaxy S10 Lite in maintenance mode (Test 74+ loader). Run a command here first, then start the
phone (or plug it in) with the cable in this PC: in its first seconds the loader listens, and the first fastboot command
keeps it in maintenance mode instead of starting iOS (a volume key held at the start does the same).

  python phone.py logs [OUT]        everything: the live firmware ring, the three saved logs in USERDATA (the boot
                                    manager's two slots and the early driver-stage slot), the loader's log; as text
  python phone.py read PART OFF LEN [FILE]   any partition range (hex offsets; whole 4 KiB blocks; at most 80 MiB)
  python phone.py flash-boot IMAGE  a new BOOT image (written, read back and compared on the phone)
  python phone.py continue          start iOS
  python phone.py poweroff | reboot
  python phone.py cycle IMAGE [SECS [ARGS...]]  the whole test cycle (ARGS: new extra boot-args for it), no hands: end the run, catch the restart, flash BOOT,
                                    verify, restart, follow the new run's log for SECS (default 120). ARGS that all
                                    start with + or - (or the word std) are changes to the standard set in
                                    phone-args.txt: `cycle IMG 120 +bt=4 -torch` = standard, bt=4 added, torch taken
                                    out; `cycle IMG 120 std` = the standard set again. Other ARGS replace the set as
                                    given. A run that ends the system is said, with the lines before it.
  python phone.py rescue            (2026-11-19) back to a running iOS from anything: a stuck USB link ("no link")
                                    is cleared by restarting the phone from iOS over Wi-Fi (r5q_reboot); a phone left
                                    in maintenance mode by a test that ended the system is restarted into its
                                    listening window, its log slots saved first (the crash's record, before a new run
                                    overwrites it), and iOS started with the standard boot-args
  python phone.py unlock | home     the screen woken and the lock screen swiped away | the home gesture
  python phone.py open BUNDLE_ID    an app opened (unlocked first; e.g. com.apple.Preferences, or a URL)
  python phone.py tools             every built on-phone tool (port/build/phone-tools, made by
                                    pipeline/build_phone_tools.sh) put into /var/r5q/bin over Wi-Fi
  python phone.py args [ARGS... | --clear | --show]  extra kernel boot-args kept on the phone for every start
                                    (key=value replaces the image's; -key removes it); iOS restarts with them
  python phone.py smem [FILE]        the shared memory while iOS runs, decoded (the modem's state and crash reason)
  python phone.py charger           the charger's state (PM8150B registers, reads only), also while iOS runs
  python phone.py settime           the phone's clock from this PC (kept for later starts without a PC)
  python phone.py restart [SECS]    the same without the flash: end the run, restart, follow the new run
  python phone.py live [SECONDS]    while iOS runs (live mode): its kernel log as it is written
  python phone.py stop              end the iOS run now (it returns to the loader as at its time limit)
  python phone.py alive             (2026-11-19) iOS running, maintenance mode, or nothing (exit 0/1/2). Not `raw
                                    devices`: the loader's USB answers "xnuloader fastboot" while iOS runs too
  python phone.py unstick           (2026-11-20) the loader's USB present but silent while iOS runs: restart the phone
                                    from iOS over Wi-Fi (r5q_reboot) so the loader's USB comes back; no replug
  python phone.py wlog [FILE [SECS]] (2026-11-19) the kernel log over Wi-Fi (wifi/klog.c via r5qd) every second into
                                    FILE, independent of USB; stops 60 s after the phone goes silent
  python phone.py screenshot [PNG]  the phone's screen
  python phone.py raw ARGS...       any fastboot command (e.g. raw oem part userdata)
  python phone.py wifi-join [--forget]  the Wi-Fi network iOS joins: asks for its name and password here, keeps only
                                    the WPA2 key made from them (PBKDF2, the PMK) in USERDATA 4 GiB + 520 KiB

Writes on the phone are only what flash-boot (BOOT) and the loader's own allowlist (boot, userdata) permit.
"""
import os
import pathlib
import shutil
import struct
import subprocess
import sys
import tempfile
import time

FASTBOOT = shutil.which('fastboot') or str(pathlib.Path.home() / 'AppData/Local/Microsoft/WinGet/Packages/'
                                           'Google.PlatformTools_Microsoft.Winget.Source_8wekyb3d8bbwe/platform-tools/fastboot.exe')
RING = 0x200000
SLOT_BYTES = 4096 + RING                       # a header block + the ring
LOG_AREA = (4 << 30) + (256 * 4096)            # in USERDATA: 1 MiB past the iOS install record at 4 GiB
SLOTS = {'boot-manager-0': LOG_AREA, 'boot-manager-1': LOG_AREA + 1024 * 4096,
         'early-driver-stage-a': LOG_AREA + 2048 * 4096, 'early-driver-stage-b': LOG_AREA + 3072 * 4096}


def fb(*args, check=True, timeout=600):
    try:
        r = subprocess.run([FASTBOOT, *args], capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        if check:
            raise SystemExit(f'fastboot {" ".join(args)}: no answer in {timeout} s')
        return f'FAIL (no answer in {timeout} s)'
    out = (r.stdout + r.stderr).strip()
    if check and r.returncode:
        raise SystemExit(f'fastboot {" ".join(args)}: {out}')
    return out


SMEM_BASE, SMEM_SIZE = 0x86000000, 0x200000
SMP2P_BITS = {0: 'err-fatal', 1: 'err-ready', 2: 'proxy-unvote', 3: 'stop-ack', 7: 'shutdown-ack'}


MPSS_BASE, MPSS_SIZE = 0x8d800000, 0xa000000


def modemdump(out=None):
    """(Test 97 loader) stop the crashed modem through TrustZone (oem modemstop: PAS shutdown, as Linux before its crash
    dump), then read its 160 MB of memory in 2 MiB peeks into one file"""
    out = pathlib.Path(out or (pathlib.Path(tempfile.gettempdir()) / 'r5q-modem.bin'))
    piece = pathlib.Path(tempfile.gettempdir()) / 'r5q-modem-piece.bin'
    print(fb('oem', 'modemstop'))
    with open(out, 'wb') as f:
        for off in range(0, MPSS_SIZE, 0x200000):
            fb('oem', 'peek', f'{MPSS_BASE + off:x}', '200000')
            fb('get_staged', str(piece))
            f.write(piece.read_bytes()[:0x200000])
            if off % 0x1000000 == 0:
                print(f'  {off >> 20} MB', flush=True)
    print(f'modem memory: {out}')


def smem(save=None):
    """the shared memory between the processors, read while iOS runs (Test 84+ loader: oem peek, reads only), decoded:
    partitions, items, the modem's SMP2P entries (item 435: modem -> us) and its crash reason (item 421)"""
    raw = pathlib.Path(save or (pathlib.Path(tempfile.gettempdir()) / 'r5q-smem.bin'))
    piece = pathlib.Path(tempfile.gettempdir()) / 'r5q-smem-piece.bin'
    m = bytearray(SMEM_SIZE)

    def peek(off, size):
        # (only the TOC and the partitions the AP is one end of: the others are protected, and a read of one hangs the
        # loader -- Test 84; the Test 85 loader refuses them itself)
        fb('oem', 'peek', f'{SMEM_BASE + off:x}', f'{size:x}')
        fb('get_staged', str(piece))
        m[off:off + size] = piece.read_bytes()[:size]
    toc = SMEM_SIZE - 0x1000
    peek(toc, 0x1000)
    if m[toc:toc + 4] != b'$TOC':
        raise SystemExit('no SMEM table of contents')
    count = struct.unpack_from('<I', m, toc + 8)[0]
    items = {}
    for i in range(count):
        e = toc + 32 + i * 48
        off, size, flags, h0, h1 = struct.unpack_from('<IIIHH', m, e)
        if not (h0 == 0 or h1 == 0 or h0 == 0xfffe) or off == 0 or off + size > SMEM_SIZE:
            continue
        peek(off, size)
        if m[off:off + 4] != b'$PRT':
            continue
        name = 'global' if h0 == 0xfffe else f'{h0}-{h1}'
        free = struct.unpack_from('<I', m, off + 12)[0]
        at, found = off + 32, []
        while at < off + free:
            canary, item, isize, pdata, phdr = struct.unpack_from('<HHIHH', m, at)
            if canary != 0xa5a5:
                found.append('(broken canary)')
                break
            data = at + 16 + phdr
            found.append(item)
            items.setdefault(item, (name, data, isize - pdata))
            at = data + isize
        if found:
            print(f'partition {name}: items {found}')
    if 435 in items:
        _, d, n = items[435]
        magic, ver, lpid, rpid, total, valid, flags = struct.unpack_from('<4sIHHHHI', m, d)
        print(f'modem SMP2P (item 435): {magic!r} version {ver & 0xff}, pid {lpid}->{rpid}, {valid}/{total} entries, flags {flags:#x}')
        for k in range(valid):
            nm, val = struct.unpack_from('<16sI', m, d + 20 + 20 * k)
            bits = [b for i, b in SMP2P_BITS.items() if val >> i & 1] if nm.startswith(b'slave-kernel') else []
            print(f'    {nm.split(bytes(1))[0].decode(errors="replace"):16s} {val:#010x} {" ".join(bits)}')
    for item in (421, 428):
        if item in items:
            _, d, n = items[item]
            body = m[d:d + n]
            print(f'item {item} ({n} bytes): ' + (body.split(bytes(1))[0].decode(errors='replace') if item == 421 else body[:32].hex()))
        else:
            print(f'item {item}: not there')
    for item in sorted(set(items) - {421, 428, 435, 478, 479, 480}):
        _, d, n = items[item]
        body = bytes(m[d:d + min(n, 96)])
        text = ''.join(chr(c) if 32 <= c < 127 else '.' for c in body)
        print(f'item {item} ({n} bytes): {body[:48].hex()}  |{text}|')
        if item == 611:     # the modem's error report (err_smem): text, all of it
            full = bytes(m[d:d + n]).rstrip(bytes(1))
            print('    ' + ''.join(chr(c) if 32 <= c < 127 or c == 10 else ('\n' if c == 0 else '.') for c in full).replace('\n\n', '\n'))
    raw.write_bytes(bytes(m))
    print(f'(raw copy: {raw})')


CHG_STATE = ['trickle', 'pre-charge', 'FAST', 'full-on', 'taper', 'TERMINATED', 'INHIBITED', 'DISABLED']
APSD = {0: 'SDP (a PC port)', 1: 'OCP', 2: 'CDP (a charging PC port)', 3: 'DCP (a wall charger)', 4: 'float', 5: 'QC2', 6: 'QC3'}


def charger():
    """the PM8150B charger's registers (Test 86+ loader: oem charger, reads only; works while iOS runs), decoded"""
    tmp = pathlib.Path(tempfile.gettempdir()) / 'r5q-charger.txt'
    fb('oem', 'charger')
    fb('get_staged', str(tmp))
    text = tmp.read_text(errors='replace')
    print(text.rstrip())
    regs = {}
    for line in text.splitlines():
        if '=' not in line:
            continue
        left, _, right = line.partition('=')
        parts = left.split()
        if len(parts) < 2:
            continue
        try:
            regs[parts[0] + ' ' + parts[1]] = [int(v, 16) for v in right.split()]
        except ValueError:
            continue
    if not regs:
        print('=> PM8150B charger registers did not answer (S10 Lite uses S2MU106 I2C); checking live kernel battery log...')
        out = r5qd_quiet('run', '/var/r5q/bin/klog', '58000', timeout=20)
        hits = [l for l in out.splitlines() if 'battery:' in l or 'charger:' in l]
        for l in hits[-8:]:
            print('   ' + l)
        return
    st = regs.get('chgr status_1..2')
    if st:
        print(f'=> charger state: {CHG_STATE[st[0] & 7]}')
    ap = regs.get('usbin apsd_status,result')
    if ap:
        kinds = [v for b, v in APSD.items() if ap[1] >> b & 1]
        print(f'=> port detected (APSD done={ap[0] & 1}): {", ".join(kinds) or "none"}')
    icl = regs.get('misc icl_status')
    cfg = regs.get('usbin icl_cfg')
    if icl and cfg:
        print(f'=> input current limit: in force {icl[0] * 50} mA, configured {cfg[0] * 50} mA')
    fcc = regs.get('chgr fcc_cfg')
    if fcc:
        print(f'=> fast charge current setting: {fcc[0] * 50} mA')
    en = regs.get('chgr enable_cmd')
    if en:
        print(f'=> charging enable command bit: {en[0] & 1}')
    il = regs.get('usbin cmd_il')
    if il:
        print(f'=> USB input suspended: {il[0] & 1}')


def settime():
    """the phone's clock from this PC's (Test 81+ loader): kept in USERDATA with the PM8150 RTC's count, so later
    starts without a PC know the time too"""
    out = fb('oem', 'settime', str(int(time.time())), check=False)
    print('    clock: ' + (out.splitlines()[0] if out else '?'), flush=True)


IOS_W, IOS_H = 750, 1624                       # iOS's screen as the screenshot shows it (top left of the image)


def touch(kind, rest):
    """(Test 126 loader+kernel) a gesture for the running iOS: tap X Y | press X Y [MS] | swipe X0 Y0 X1 Y1 [MS],
    in pixels of iOS's 750x1624 screen (the top-left part of `screenshot`)."""
    v = [int(float(a)) for a in rest]
    need = 4 if kind == 'swipe' else 2
    if len(v) < need:
        raise SystemExit(f'usage: {kind} ' + ('X0 Y0 X1 Y1 [MS]' if kind == 'swipe' else 'X Y' + (' [MS]' if kind == 'press' else '')))

    def sx(x):
        return max(0, min(65535, x * 65535 // IOS_W))

    def sy(y):
        return max(0, min(65535, y * 65535 // IOS_H))
    if kind == 'swipe':
        k, x0, y0, x1, y1, ms = 2, sx(v[0]), sy(v[1]), sx(v[2]), sy(v[3]), v[4] if len(v) > 4 else 300
    else:
        k, x0, y0, ms = (3 if kind == 'press' else 1), sx(v[0]), sy(v[1]), v[2] if len(v) > 2 else 800
        x1, y1 = x0, y0
    for _ in range(50):
        out = fb('oem', 'touch', *('%x' % n for n in (k, x0, y0, x1, y1, ms)), check=False)
        if 'still being played' not in out:
            break
        time.sleep(0.1)
    print(f'{kind} {rest}: ' + (out.strip().splitlines()[0] if out.strip() else 'OKAY'))


WIFI_KEY_OFF = (4 << 30) + 520 * 1024        # USERDATA block 4 GiB + 520 KiB (after the clock 512 and args 516)


def wifi_join(rest):
    """the network to join: name and password asked for here (never on the command line), the WPA2-PSK key derived
    from them (PBKDF2-HMAC-SHA1, 4096 rounds, 32 bytes) kept on the phone; the password itself is not stored anywhere"""
    import getpass
    import hashlib
    block = bytearray(4096)
    if rest != ['--forget']:
        ssid = input('Wi-Fi network name: ').encode()
        pw = getpass.getpass('Wi-Fi password (not shown): ').encode()
        if not 1 <= len(ssid) <= 32 or not 8 <= len(pw) <= 63:
            raise SystemExit('a name of 1-32 bytes and a WPA2 password of 8-63 characters')
        pmk = hashlib.pbkdf2_hmac('sha1', pw, ssid, 4096, 32)
        del pw
        block[0:16] = b'R5Q-WIFI-KEY/1\0\0'
        struct.pack_into('<I', block, 16, len(ssid))
        block[20:20 + len(ssid)] = ssid
        block[52:84] = pmk
        h = 2166136261
        for b in bytes(block[0:84]):
            h = ((h ^ b) * 16777619) & 0xffffffff
        struct.pack_into('<I', block, 84, h)
    if 'FAIL' in fb('oem', 'args', 'show', check=False):
        print('ending the iOS run to reach the listening window ...', flush=True)
        fb('oem', 'stop', check=False)
        if not gone(45):
            fb('reboot', check=False)
            gone(30)
        wait_for_phone()
    tmp = pathlib.Path(tempfile.gettempdir()) / 'r5q-wifi-key.bin'
    try:
        tmp.write_bytes(bytes(block))
        fb('stage', str(tmp))
    finally:
        tmp.write_bytes(bytes(4096))
        tmp.unlink()
    out = fb('oem', 'writepart', 'userdata', '%x' % WIFI_KEY_OFF)
    print(('Wi-Fi key removed: ' if rest == ['--forget'] else 'Wi-Fi network stored (the key only): ') + out.splitlines()[0])
    print(fb('oem', 'continue'))


def args(rest):
    """extra kernel boot-args kept on the phone (Test 82+ loader), put in at every start after the BOOT image's own:
    a key given here replaces the image's, '-key' takes it away. No new BOOT image needed."""
    if 'FAIL' in fb('oem', 'args', 'show', check=False):
        # iOS is running (live mode): only the listening window before iOS takes these; end the run and catch it
        print('ending the iOS run to reach the listening window ...', flush=True)
        fb('oem', 'stop', check=False)
        if not gone(45):
            fb('reboot', check=False)
            gone(30)
        wait_for_phone()
    if not rest or rest == ['--show']:
        pass
    elif rest == ['--clear']:
        print(fb('oem', 'args', 'clear'))
    else:
        text = ' '.join(rest).encode()
        if len(text) > 400:
            raise SystemExit('at most 400 bytes of arguments')
        tmp = pathlib.Path(tempfile.gettempdir()) / 'r5q-args.txt'
        tmp.write_bytes(text)
        fb('stage', str(tmp))
        print(fb('oem', 'args', 'set'))
    shown = pathlib.Path(tempfile.gettempdir()) / 'r5q-args-now.txt'
    fb('oem', 'args', 'show')
    fb('get_staged', str(shown))
    print('extra boot-args now: ' + (shown.read_text().strip() or '(none)'))
    settime()
    print('starting iOS with them: ' + fb('oem', 'continue', check=False).splitlines()[0])


def usb_present():
    """(2026-11-20) True when Windows lists the loader's USB device (VID 18D1 / PID D00D) as present, whether or not it
    answers fastboot. Present but silent = the loader's USB link is stuck, NOT an unplugged cable: a hung `phone.py
    screenshot`, stopped midway, left the loader's USB wedged for hours while it was misreported as 'unplugged'."""
    try:
        r = subprocess.run(['powershell', '-NoProfile', '-Command',
                            "@(Get-PnpDevice -PresentOnly | Where-Object InstanceId -match 'VID_18D1&PID_D00D').Count"],
                           capture_output=True, text=True, timeout=20)
        return r.stdout.strip() not in ('', '0')
    except (OSError, subprocess.TimeoutExpired):
        return False


def r5qd_quiet(*args, timeout=30):
    """a wifi/r5qd_client.py command; '' when the phone does not answer over Wi-Fi"""
    try:
        r = subprocess.run([sys.executable, str(pathlib.Path(__file__).resolve().parent / 'wifi/r5qd_client.py'), *args],
                           capture_output=True, text=True, timeout=timeout, env={**os.environ, 'MSYS_NO_PATHCONV': '1'})
        return r.stdout if r.returncode == 0 else ''
    except subprocess.TimeoutExpired:
        return ''


def unstick_usb():
    """(2026-11-20) The loader's USB is present but silent while iOS answers over Wi-Fi: restart the phone from iOS
    (/var/r5q/bin/r5q_reboot, reboot(2) over r5qd; the firmware restarts the phone and the loader brings its USB up
    again). No replugging. True when a restart was sent."""
    if not usb_present() or 'pong' not in r5qd_quiet('ping', timeout=15):
        return False
    print('    USB cable connected but the loader does not answer (stuck link): restarting the phone from iOS over Wi-Fi',
          flush=True)
    r5qd_quiet('run', '/var/r5q/bin/r5q_reboot', timeout=20)     # (the connection drops as the phone restarts)
    return True


def wait_for_phone(seconds=3600, unstick=False):
    """fastboot keeps asking until the phone shows up; its first command, the moment the loader listens (the first
    15 s of every start with the cable in a PC), puts the phone into maintenance mode.
    unstick (only `cycle`'s catch after ending a run): when the loader's USB stays silent for three tries over ~45 s
    while the device is present and iOS answers over Wi-Fi, restart the phone from iOS (unstick_usb). (2026-11-20: a
    first version did this after one 10 s miss in every caller, and `phone.py open` restarted a working phone while
    mediaserverd was restarting.)"""
    if unstick:
        for attempt in range(3):
            try:
                quick = subprocess.run([FASTBOOT, 'getvar', 'product'], capture_output=True, text=True, timeout=15)
                if 'xnuloader' in quick.stdout + quick.stderr:
                    print('the loader answers on USB (maintenance mode, or iOS running in live mode: `phone.py alive` tells)')
                    return
            except subprocess.TimeoutExpired:
                continue
            if 'Write to device failed' in (quick.stdout + quick.stderr):
                continue
            break                                           # (an answer, just not the loader: no restart)
        else:
            unstick_usb()                                   # (a stuck link, not a missing cable)
    print('waiting for the phone: start it (or plug it in) now ...', flush=True)
    r = subprocess.run([FASTBOOT, 'getvar', 'product'], capture_output=True, text=True, timeout=seconds)
    if 'xnuloader' not in r.stdout + r.stderr:
        raise SystemExit('no phone in maintenance mode: ' + (r.stdout + r.stderr).strip())
    print('the loader answers on USB (maintenance mode, or iOS running in live mode: `phone.py alive` tells)')


def staged(command, dest):
    fb('oem', *command.split())
    fb('get_staged', str(dest))
    return pathlib.Path(dest).read_bytes()


def unroll(ring):
    if len(ring) < 16 or ring[4:8] != b'LOGM':
        return b''
    widx = struct.unpack_from('<I', ring, 8)[0]
    data, cap = ring[16:RING], RING - 16
    return data[:widx] if widx <= cap else data[widx % cap:] + data[:widx % cap]


def logs(out):
    out = pathlib.Path(out) / time.strftime('maint-logs-%Y%m%d-%H%M%S')
    out.mkdir(parents=True)
    ring = staged('fwlog', out / 'firmware-ring-now.bin')
    (out / 'firmware-now.txt').write_bytes(unroll(ring))
    print(f'live firmware log: {len(unroll(ring))} bytes')
    for name, off in SLOTS.items():
        raw = staged(f'readpart userdata {off:x} {SLOT_BYTES:x}', out / f'{name}.bin')
        header = raw[:104]
        kind = header[:16].split(b'\0')[0].decode(errors='replace')
        why = header[32:96].split(b'\0')[0].decode(errors='replace')
        if kind.startswith('R5QEARLYLOG'):
            start, state = struct.unpack_from('<II', header, 96)
            why += f' | start {start}, ' + {1: 'DIED IN THE DRIVER STAGE', 2: 'got through the driver stage'}.get(state, f'state {state}')
        text = unroll(raw[4096:])
        (out / f'{name}.txt').write_bytes(text)
        print(f'{name}: {kind or "(empty)"}; {why!r}; {len(text)} bytes of log')
    # (the loader's own "oem log" upload wedged the USB link on 2026-11-17: left out until fixed)
    print(f'all in {out}')


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    cmd, rest = sys.argv[1], sys.argv[2:]
    if cmd == 'alive':
        alive()
    if cmd == 'unstick':
        sys.exit(0 if unstick_usb() else 1)
    if cmd == 'wlog':
        wlog(rest[0] if rest else time.strftime('wlog-%Y%m%d-%H%M%S.txt'), float(rest[1]) if len(rest) > 1 else 600)
        return
    if cmd not in ('rd', 'respring', 'sh', 'ps', 'tools', 'rescue', 'unlock', 'home', 'open'):  # (Wi-Fi, or their own way)
        wait_for_phone()
    if cmd == 'tools':
        tools()
        return
    if cmd in ('unlock', 'home', 'open'):
        wait_for_phone()
        {'unlock': unlock, 'home': home}[cmd]() if cmd != 'open' else open_app(rest[0])
        return
    if cmd == 'rescue':
        rescue()
        return
    if cmd == 'logs':
        logs(rest[0] if rest else pathlib.Path(__file__).parent / 'port/build/phone-logs')
    elif cmd == 'read':
        part, off, length = rest[0], int(rest[1], 16), int(rest[2], 16)
        dest = rest[3] if len(rest) > 3 else f'{part}-{off:x}-{length:x}.bin'
        staged(f'readpart {part} {off:x} {length:x}', dest)
        print(f'{dest}: {length} bytes')
    elif cmd == 'flash-boot':
        image = pathlib.Path(rest[0])
        if image.stat().st_size != 64 << 20 or image.read_bytes()[:8] != b'ANDROID!':
            raise SystemExit('not a 64 MiB Android boot image')
        print(fb('flash', 'boot', str(image)))
    elif cmd == 'continue':
        print(fb('oem', 'continue'))
    elif cmd in ('poweroff', 'reboot'):
        print(fb('oem', 'poweroff') if cmd == 'poweroff' else fb('reboot'))
    elif cmd == 'live':
        live(float(rest[0]) if rest else 2.0)
    elif cmd == 'cycle':
        cycle(pathlib.Path(rest[0]), int(rest[1]) if len(rest) > 1 else 120, rest[2:] or None)
    elif cmd == 'args':
        args(rest)
    elif cmd == 'wifi-join':
        wifi_join(rest)
    elif cmd in ('tap', 'swipe', 'press'):
        touch(cmd, rest)
    elif cmd in ('rd', 'respring', 'sh', 'ps'):
        # (Test 157) the running iOS without a reboot, over Wi-Fi through r5qd (wifi/r5qd.c, wifi/r5qd_client.py):
        #   rd ping|put LOCAL /var/..|run PROG ARGS|kill NAME [SIG]|inject [CONF]|reload LIB PROGRAM
        #   respring            SpringBoard restarted (launchd starts it again)
        #   sh PROG ARGS...     a program run on the phone, its output here (no shell: PROG is a full path)
        #   ps                  the processes
        sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent / 'wifi'))
        import r5qd_client
        if cmd == 'respring':
            r5qd_client.main(['kill', 'SpringBoard'])
        elif cmd == 'ps':
            r5qd_client.main(['run', '/bin/ps', '-Ao', 'pid,%cpu,rss,comm'])
        elif cmd == 'sh':
            r5qd_client.main(['run'] + rest)
        else:
            r5qd_client.main(rest)
    elif cmd == 'button':
        # (Test 144 kernel) a hardware button for the running iOS: button lock|volup|voldown [MS]
        names = {'lock': 1, 'side': 1, 'power': 1, 'volup': 2, 'voldown': 3}
        if not rest or rest[0] not in names:
            raise SystemExit('usage: button lock|volup|voldown [MS]')
        ms = int(rest[1]) if len(rest) > 1 else 120
        for _ in range(50):
            out = fb('oem', 'touch', *('%x' % n for n in (4, names[rest[0]], 0, 0, 0, ms)), check=False)
            if 'still being played' not in out:
                break
            time.sleep(0.1)
        print(f'button {rest[0]} {ms} ms: ' + (out.strip().splitlines()[0] if out.strip() else 'OKAY'))
    elif cmd == 'charger':
        charger()
    elif cmd == 'smem':
        smem(rest[0] if rest else None)
    elif cmd == 'modemdump':
        modemdump(rest[0] if rest else None)
    elif cmd == 'settime':
        settime()
    elif cmd == 'restart':
        cycle(None, int(rest[0]) if rest else 120)
    elif cmd == 'stop':
        print(fb('oem', 'stop'))
    elif cmd == 'screenshot':
        screenshot(rest[0] if rest else time.strftime('screen-%Y%m%d-%H%M%S.png'))
    elif cmd == 'raw':
        print(fb(*rest, check=False))
    else:
        raise SystemExit(__doc__)


def live(interval):
    """iOS's kernel log as it is written (live mode, Test 79+: iOS on core 7, the loader on core 0 serving USB)."""
    seen = 0
    tmp = pathlib.Path(tempfile.gettempdir()) / 'r5q-live-log.txt'
    # caught in the listening window before iOS: start it (during a run the loader refuses this, harmlessly)
    fb('oem', 'continue', check=False)
    time.sleep(3)
    print('following the kernel log (Ctrl+C to stop following; iOS keeps running)', flush=True)
    while True:
        fb('oem', 'log', check=False)
        if 'OKAY' in fb('get_staged', str(tmp), check=False) or tmp.exists():
            text = tmp.read_bytes().decode(errors='replace')
            mark = text.find('== KERNEL LOG (the kernel is still running) ==')
            body = text[mark:] if mark >= 0 else text
            if len(body) < seen:
                seen = 0            # (the ring wrapped or a new run)
            sys.stdout.write(body[seen:])
            sys.stdout.flush()
            seen = len(body)
        time.sleep(interval)


def gone(seconds):
    """True once the phone has left USB (it is restarting), within `seconds`."""
    for _ in range(seconds):
        if 'fastboot' not in fb('devices', check=False):
            return True
        time.sleep(1)
    return False


STD_ARGS = pathlib.Path(__file__).resolve().parent / 'phone-args.txt'


def merge_args(words):
    """(2026-11-19) +key=value / -key / std: changes to the standard boot-args in phone-args.txt; anything else as is"""
    if not words or not all(w == 'std' or w[:1] in '+-' for w in words):
        return words
    std = STD_ARGS.read_text().split() if STD_ARGS.exists() else []
    keep = {w.split('=')[0]: w for w in std}
    for w in words:
        if w.startswith('+'):
            keep[w[1:].split('=')[0]] = w[1:]
        elif w.startswith('-'):
            keep.pop(w[1:].split('=')[0], None)
    return list(keep.values())


def set_args(words):
    """(in the listening window) the extra boot-args kept on the phone"""
    words = merge_args(words)
    tmp = pathlib.Path(tempfile.gettempdir()) / 'r5q-args.txt'
    tmp.write_bytes(' '.join(words).encode())
    fb('stage', str(tmp))
    print('    args: ' + ' '.join(words) + ': ' + fb('oem', 'args', 'set').splitlines()[0])


def cycle(image, follow, new_args=None):
    """The whole test cycle from the PC, the cable left in: end the iOS run (or leave maintenance mode), catch the
    phone in its listening window as it starts again, flash IMAGE to BOOT and read it back, restart, and follow the
    new run's kernel log for `follow` seconds into OUT (port/build/cycles/)."""
    import hashlib
    if image is not None:
        data = image.read_bytes()
        if len(data) != 64 << 20 or data[:8] != b'ANDROID!':
            raise SystemExit('not a 64 MiB Android boot image')
        want = hashlib.sha256(data).hexdigest()
    out = pathlib.Path(__file__).parent / 'port/build/cycles' / time.strftime(
        f'%Y%m%d-%H%M%S-{image.stem if image is not None else "restart"}')
    out.mkdir(parents=True)
    print('1/5 ending the current run', flush=True)
    fb('oem', 'stop', check=False)                      # (live mode: iOS returns, the firmware restarts the phone)
    if not gone(45):
        fb('reboot', check=False)                       # (maintenance mode: just restart)
        gone(30)
    print('2/5 catching the phone as it starts again', flush=True)
    wait_for_phone(unstick=True)                        # (the one place a stuck link is restarted)
    if image is None:
        print('3/5 (restart only: BOOT left as it is)', flush=True)
        settime()
        fb('oem', 'continue', check=False)
        follow_run(out, follow)
        return
    print('3/5 flashing BOOT and reading it back', flush=True)
    for attempt in range(3):
        out_flash = fb('flash', 'boot', str(image), check=attempt == 2)
        # (2026-11-20, Tests 402-403) Every first cycle from a running iOS: the catch in step 2 met iOS's own live-mode
        # listener before the restart, and the flash was refused. End the run again, wait for the real restart, retry.
        if 'a kernel is running' not in out_flash:
            break
        print('    refused: "a kernel is running" -- ending the run again and catching the restart', flush=True)
        fb('oem', 'stop', check=False)
        if not gone(60):
            fb('reboot', check=False)
            gone(30)
        wait_for_phone()
    print(out_flash)
    back = out / 'boot-readback.bin'
    staged(f'readpart boot 0 {64 << 20:x}', back)
    got = hashlib.sha256(back.read_bytes()).hexdigest()
    back.unlink()
    if got != want:
        raise SystemExit(f'BOOT read back as {got[:16]}, not {want[:16]}: not restarting into it')
    print(f'    BOOT = {want[:16]} (verified)')
    print('4/5 restarting into it', flush=True)
    fb('reboot', check=False)
    gone(30)
    wait_for_phone()                                    # (caught in the new start's listening window)
    if new_args:
        set_args(new_args)
    settime()
    fb('oem', 'continue', check=False)
    follow_run(out, follow)


def merge_log(have, snap):
    """the new part of a ring snapshot appended to what we have: found by the overlap of our last 4 KB in it"""
    if not have:
        return snap
    for size in (4096, 1024, 256):
        tail = have[-size:]
        at = snap.rfind(tail)
        if at >= 0:
            return have + snap[at + len(tail):]
    return have + '\n== [log gap: the ring wrapped between two snapshots] ==\n' + snap


def follow_run(out, follow):
    print(f'5/5 following the new run for {follow} s -> {out / "live.txt"}', flush=True)
    deadline, text = time.time() + follow, ''
    tmp = out / 'live.tmp'
    time.sleep(5)
    misses = 0
    while time.time() < deadline:
        # (2026-11-19, Tests 306-310) short timeouts: one stuck call used to end the capture silently (600 s each), so
        # the log stopped at ~35 s while iOS ran on for minutes; a gap is now written into the log as it happens
        got = 'FAIL' not in fb('oem', 'log', check=False, timeout=20)
        got = got and 'FAIL' not in fb('get_staged', str(tmp), check=False, timeout=40)
        if not got:
            misses += 1
            text += f'\n== [follower: USB snapshot failed at {time.strftime("%H:%M:%S")} ({misses} in a row)] ==\n'
            (out / 'live.txt').write_text(text, encoding='utf-8', errors='replace')
            time.sleep(2)
            continue
        misses = 0
        if tmp.exists():
            # (2026-11-19) every snapshot merged into one growing log: the ring is 2 MB and wraps in seconds once
            # userland logs, so keeping only the last snapshot lost a run's early kernel lines (Test 252)
            text = merge_log(text, tmp.read_bytes().decode(errors='replace'))
            # (2026-11-19, Test 299) live.txt kept current, so a run can be read while it is followed (it used to be
            # written only when the window closed; live.tmp is only the last snapshot)
            (out / 'live.txt').write_text(text, encoding='utf-8', errors='replace')
        time.sleep(2)
    (out / 'live.txt').write_text(text, encoding='utf-8', errors='replace')
    tmp.unlink(missing_ok=True)
    for marker in ['clock: ', 'exec /sbin/launchd.ios', 'systemShellDidFinishLaunching', 'panic(', 'HUNG',
                   'ufs: /dev/rufs_darwin takes writes', 'no write will be asked for', 'var_store']:
        print(f'    {marker!r}: {"seen" if marker in text else "-"}')
    for line in text.splitlines():
        if line.startswith('clock: ') or ('boot-args = ' in line and ' epoch=' in line):
            print('    ' + (line if line.startswith('clock') else 'boot-args: epoch=' + line.split(' epoch=')[1].split()[0]))
    lines = text.splitlines()
    for i, line in enumerate(lines):
        if 'ends the system' in line or 'XNU RETURN' in line:
            # (2026-11-19) a test that ends the system: said here, with what came before it; `phone.py rescue` next
            print('    THE SYSTEM ENDED during this run. The lines before it:')
            for before in [x for x in lines[max(0, i - 15):i] if x.strip()]:
                print('      | ' + before[:200])
            print('    -> the phone is likely in the loader now: `python phone.py rescue` saves the crash record and '
                  'starts iOS with the standard boot-args')
            break
    print(f'cycle done: {out}')


def unlock():
    """(2026-11-19) wake the screen and swipe the lock screen away (no passcode is set)"""
    touch('tap', ['375', '900'])
    time.sleep(1.5)
    touch('swipe', ['375', '1615', '375', '400', '200'])
    time.sleep(1.5)


def home():
    """the home gesture: a quick swipe up from the bottom edge"""
    touch('swipe', ['375', '1620', '375', '900', '120'])
    time.sleep(1.5)


def r5qd_out(line):
    """one r5qd request, its whole answer back as text"""
    sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent / 'wifi'))
    import r5qd_client
    return r5qd_client.request(r5qd_client.IP_FILE.read_text().strip(), line)


def open_app(bundle):
    """(2026-11-19) an app opened from the PC. openurl first; only if iOS refuses it (exit 1: a locked screen) is the
    screen unlocked and openurl tried again -- an unlock tap while an app is already in front would tap the app"""
    target = bundle if ':' in bundle else 'app:' + bundle
    for attempt in range(2):
        out = r5qd_out(f'run /var/r5q/bin/openurl {target}')
        if 'exit 0' in out:
            print(f'opened {bundle}')
            time.sleep(3)
            return
        if attempt == 0:
            unlock()
    print(f'{bundle} not opened: ' + out.strip().splitlines()[-1] if out.strip() else '(no answer)')


def usb_ok():
    out = fb('getvar', 'product', check=False)
    return 'product: xnuloader' in out


def r5qd(*words):
    sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent / 'wifi'))
    import r5qd_client
    return r5qd_client.main(list(words))


def wlog(out_path, seconds):
    """(2026-11-19, camera Tests 306-325) the kernel log over Wi-Fi, independent of the loader's USB, STREAMED: wifi/klog.c
    in follow mode (`klog -f`: every 50 ms only what is new, ~60 KB a run, r5qd relays it as it comes) started again
    at once each time it ends; every chunk is written to FILE the moment it arrives, so a hang loses at most ~50 ms.
    Puts klog on the phone first. Ends after `seconds`, or 60 s after r5qd stops answering (the phone gone)."""
    import socket
    sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent / 'wifi'))
    import r5qd_client
    out = pathlib.Path(out_path)
    out.parent.mkdir(parents=True, exist_ok=True)
    ip = r5qd_client.IP_FILE.read_text().strip()
    klog = pathlib.Path(__file__).resolve().parent / 'port/build/phone-tools/klog'
    deadline, silent_since, put = time.time() + seconds, None, False
    with open(out, 'ab') as f:
        while time.time() < deadline:
            try:
                if not put:
                    data = klog.read_bytes()
                    r5qd_client.request(ip, f'put /var/r5q/bin/klog {len(data)}', data, timeout=30)
                    put = True
                with socket.create_connection((ip, r5qd_client.PORT), timeout=10) as s:
                    s.settimeout(5)
                    s.sendall(f'{r5qd_client.TOKEN} run /var/r5q/bin/klog -f 30\n'.encode())
                    got_any = False
                    while True:
                        chunk = s.recv(65536)
                        if not chunk:
                            break
                        f.write(chunk)
                        f.flush()
                        got_any = True
                silent_since = None if got_any else silent_since
            except Exception as e:      # (a dropped connection or silence: the phone may be going down)
                now = time.strftime('%H:%M:%S')
                silent_since = silent_since or time.time()
                f.write(f'\n== [wlog: r5qd silent at {now}: {type(e).__name__}] ==\n'.encode())
                f.flush()
                if time.time() - silent_since > 60:
                    print(f'wlog: r5qd silent for 60 s since {time.strftime("%H:%M:%S", time.localtime(silent_since))}: '
                          f'the phone is likely gone; {out}')
                    return
                time.sleep(1)
    print(f'wlog: {out}')


def tools():
    """every built on-phone tool into /var/r5q/bin over Wi-Fi"""
    src = pathlib.Path(__file__).resolve().parent / 'port/build/phone-tools'
    built = sorted(p for p in src.iterdir() if p.is_file() and not p.name.startswith('.')) if src.exists() else []
    if not built:
        raise SystemExit(f'nothing in {src}: run pipeline/build_phone_tools.sh (in WSL) first')
    for p in built:
        print(f'{p.name}: ', end='', flush=True)
        r5qd('put', str(p), f'/var/r5q/bin/{p.name}')


def rescue():
    """back to a running iOS: a stuck USB link cleared from iOS, the crash record saved, iOS started (standard args)"""
    if not usb_ok():
        print('USB: no answer from the loader ("no link"?): restarting the phone from iOS over Wi-Fi', flush=True)
        try:
            r5qd('put', str(pathlib.Path(__file__).resolve().parent / 'port/build/phone-tools/r5q_reboot'),
                 '/var/r5q/bin/r5q_reboot')
            r5qd('run', '/var/r5q/bin/r5q_reboot')
        except Exception as e:      # (the connection drops as the phone goes down)
            print(f'    ({type(e).__name__}: the phone went down, as it should)')
        for _ in range(60):
            time.sleep(3)
            if usb_ok():
                break
        if not usb_ok():
            raise SystemExit('the loader does not answer on USB after the restart either: the cable or the PC side')
        print('USB: the loader answers again')
    def restart():
        fb('oem', 'stop', check=False)                  # (live mode: iOS returns, the phone restarts)
        if not gone(45):
            fb('reboot', check=False)                   # (maintenance mode)
            gone(30)
        wait_for_phone()                                # (caught in the listening window)

    print('restarting into the listening window to save the log slots (the last run\'s record)', flush=True)
    restart()
    logs(pathlib.Path(__file__).parent / 'port/build/phone-logs')
    print('starting iOS with the standard boot-args', flush=True)
    restart()
    set_args(['std'])
    settime()
    print(fb('oem', 'continue', check=False).splitlines()[0])


def alive():
    """(2026-11-19, after Tests 292-295 were misread as deaths) what the phone is doing now. In live mode the loader keeps
    serving USB on core 0 while iOS runs on core 7, so "xnuloader fastboot" answers either way; the loader's
    `oem args show` works only in maintenance mode (FAIL while iOS runs), and r5qd answers over Wi-Fi only while iOS
    runs. Exit 0 = iOS running, 1 = maintenance mode (no iOS), 2 = nothing answers."""
    usb = live_ios = False
    try:
        r = subprocess.run([FASTBOOT, 'getvar', 'product'], capture_output=True, text=True, timeout=8)
        usb = 'xnuloader' in r.stdout + r.stderr
        if usb:
            r = subprocess.run([FASTBOOT, 'oem', 'args', 'show'], capture_output=True, text=True, timeout=8)
            live_ios = 'FAIL' in r.stdout + r.stderr
    except subprocess.TimeoutExpired:
        pass
    wifi = False
    try:
        r = subprocess.run([sys.executable, str(pathlib.Path(__file__).resolve().parent / 'phone.py'), 'rd', 'ping'],
                           capture_output=True, text=True, timeout=15)
        wifi = r.returncode == 0 and 'Traceback' not in r.stderr
    except subprocess.TimeoutExpired:
        pass
    if live_ios or wifi:
        link = 'yes' if live_ios else ('STUCK: the cable is connected but the loader does not answer; run '
                                       '`phone.py unstick` (restart over Wi-Fi), do not ask for a replug'
                                       if usb_present() else 'no USB device: the cable may be out')
        print(f'iOS RUNNING (USB live mode: {link}; r5qd over Wi-Fi: {"answers" if wifi else "silent"})')
        sys.exit(0)
    if usb:
        print('MAINTENANCE MODE: the loader answers, iOS is not running')
        sys.exit(1)
    print('NOTHING ANSWERS: no USB (fastboot) and no r5qd over Wi-Fi')
    sys.exit(2)


def screenshot(dest):
    geometry = fb('getvar', 'screen')
    nums = [int(x) for x in geometry.split('screen:')[-1].split()[:3]] if 'screen:' in geometry else []
    if len(nums) != 3:
        raise SystemExit(f'no screen geometry: {geometry}')
    width, height, stride = nums
    raw = pathlib.Path(tempfile.gettempdir()) / 'r5q-screen.raw'
    fb('oem', 'screenshot')
    fb('get_staged', str(raw))
    data = raw.read_bytes()
    from PIL import Image          # (Pillow)
    img = Image.frombuffer('RGBA', (width, height), data, 'raw', 'BGRA', stride, 1).convert('RGB')
    img.save(dest)
    print(f'{dest}: {width}x{height}')


if __name__ == '__main__':
    main()
