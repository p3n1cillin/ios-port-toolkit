"""ipa_install.py APP.ipa|APP.app [--out OUT.ipa] [--no-install]: make an application installable on this phone and install it.

The phone's installer (installd) takes an application whose every Mach-O carries a valid ad-hoc code signature and
whose bundles are sealed (_CodeSignature/CodeResources), but not one that asks for Developer Mode (get-task-allow).
So, for every Mach-O in the bundle, innermost first: the arm64 slice kept (no PAC on this CPU: arm64e is not run),
signed by wifi/r5qsign.py -- nested bundles (.framework, .appex) sealed and signed as themselves, loose dylibs plain,
executables with their own entitlements minus get-task-allow and with this port's team prefix. Then zipped, put on
the phone over r5qd and installed by /var/r5q/bin/appinst (wifi/appinst.m, signed with wifi/appinst.entitlements).

An App Store application is encrypted (LC_ENCRYPTION_INFO cryptid 1): it installs but cannot run here -- said, and
skipped. Run from WSL or Windows Python; the upload uses wifi/r5qd_client.py.
"""
import os
import plistlib
import shutil
import struct
import subprocess
import sys
import tempfile
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import r5qsign  # noqa: E402

TEAM = 'R5QDEVTEAM'
CPU_ARM64 = 0x0100000c


def macho_kind(path):
    try:
        with open(path, 'rb') as f:
            magic = f.read(4)
    except OSError:
        return None
    if magic == b'\xcf\xfa\xed\xfe':
        return 'thin'
    if magic in (b'\xca\xfe\xba\xbe', b'\xca\xfe\xba\xbf'):
        return 'fat'
    return None


def thin(path):
    """keep the plain arm64 slice of a fat file (subtype 0 or 1), in place"""
    data = open(path, 'rb').read()
    magic, n = struct.unpack_from('>II', data, 0)
    wide = magic == 0xcafebabf
    at, chosen = 8, None
    for _ in range(n):
        if wide:
            cpu, sub, off, size, _a, _r = struct.unpack_from('>iiQQII', data, at); at += 32
        else:
            cpu, sub, off, size, _a = struct.unpack_from('>iiIII', data, at); at += 20
        if cpu == CPU_ARM64 and (sub & 0xff) in (0, 1):
            chosen = (off, size)
    if chosen is None:
        raise SystemExit(f'{path}: no arm64 slice (arm64e only needs pointer authentication, which this CPU lacks)')
    open(path, 'wb').write(data[chosen[0]:chosen[0] + chosen[1]])


def load_commands(data):
    ncmds = struct.unpack_from('<I', data, 16)[0]
    off = 32
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<II', data, off)
        yield cmd, off, size
        off += size


def encrypted(path):
    data = open(path, 'rb').read()
    for cmd, off, _ in load_commands(data):
        if cmd == 0x2c and struct.unpack_from('<I', data, off + 16)[0] != 0:     # LC_ENCRYPTION_INFO_64 cryptid
            return True
    return False


def old_entitlements(path):
    """the entitlements of an existing signature (XML blob), or {}"""
    data = open(path, 'rb').read()
    for cmd, off, _ in load_commands(data):
        if cmd == 0x1d:
            dataoff, datasize = struct.unpack_from('<II', data, off + 8)
            sb = data[dataoff:dataoff + datasize]
            if len(sb) < 12 or struct.unpack_from('>I', sb, 0)[0] != 0xfade0cc0:
                return {}
            count = struct.unpack_from('>I', sb, 8)[0]
            for i in range(count):
                typ, boff = struct.unpack_from('>II', sb, 12 + 8 * i)
                if typ == 5:
                    length = struct.unpack_from('>I', sb, boff + 4)[0]
                    try:
                        return plistlib.loads(sb[boff + 8:boff + length])
                    except Exception:
                        return {}
    return {}


def entitlements_for(binary, bundle_id):
    ents = old_entitlements(binary)
    keep = {}
    for k, v in ents.items():
        if k in ('get-task-allow', 'com.apple.developer.team-identifier', 'application-identifier', 'beta-reports-active',
                 'aps-environment', 'com.apple.developer.associated-domains'):
            continue
        if k == 'keychain-access-groups':
            v = [f'{TEAM}.{bundle_id}']
        keep[k] = v
    keep['application-identifier'] = f'{TEAM}.{bundle_id}'
    keep['com.apple.developer.team-identifier'] = TEAM
    return keep


def sign_bundle(bundle, report):
    """sign every Mach-O under bundle, nested bundles first, then bundle itself"""
    info = plistlib.load(open(os.path.join(bundle, 'Info.plist'), 'rb'))
    bundle_id, executable = info['CFBundleIdentifier'], info.get('CFBundleExecutable')
    for root, dirs, names in os.walk(bundle):
        # nested bundles: handled as bundles of their own, deepest first
        for d in list(dirs):
            full = os.path.join(root, d)
            if d.endswith(('.framework', '.appex', '.app', '.xpc')) and os.path.isfile(os.path.join(full, 'Info.plist')):
                sign_bundle(full, report)
                dirs.remove(d)
            elif d == '_CodeSignature':
                shutil.rmtree(full)
                dirs.remove(d)
        for name in names:
            full = os.path.join(root, name)
            if root == bundle and name in (executable, 'embedded.mobileprovision'):
                if name == 'embedded.mobileprovision':
                    os.remove(full)
                continue
            kind = macho_kind(full)
            if kind:
                if kind == 'fat':
                    thin(full)
                r5qsign.main([full, '--ident', name])
                report.append(f'  dylib {os.path.relpath(full, bundle)}')
    main = os.path.join(bundle, executable) if executable else None
    if main and macho_kind(main):
        if macho_kind(main) == 'fat':
            thin(main)
        if encrypted(main):
            report.append(f'  ENCRYPTED: {executable} (App Store copy): it will install but not run here')
        ents = entitlements_for(main, bundle_id) if not bundle.endswith('.framework') else {}
        with tempfile.NamedTemporaryFile('wb', suffix='.plist', delete=False) as f:
            plistlib.dump(ents, f)
        r5qsign.main([f.name, '--bundle', bundle])
        os.remove(f.name)
        report.append(f'  bundle {os.path.basename(bundle)} ({bundle_id}): {len(ents)} entitlements')


def main(argv):
    args = list(argv)
    out = None
    install = '--no-install' not in args
    args = [a for a in args if a != '--no-install']
    if '--out' in args:
        i = args.index('--out'); out = args[i + 1]; del args[i:i + 2]
    src = args[0]
    work = tempfile.mkdtemp(prefix='r5q-ipa-')
    payload = os.path.join(work, 'Payload')
    os.makedirs(payload)
    if src.endswith('.app'):
        shutil.copytree(src, os.path.join(payload, os.path.basename(src.rstrip('/'))))
    else:
        with zipfile.ZipFile(src) as z:
            for m in z.infolist():
                if m.filename.startswith('Payload/') and '..' not in m.filename:
                    z.extract(m, work)
                    mode = (m.external_attr >> 16) & 0o777
                    if mode:
                        os.chmod(os.path.join(work, m.filename), mode)
    apps = [d for d in os.listdir(payload) if d.endswith('.app')]
    if len(apps) != 1:
        raise SystemExit(f'expected one .app in Payload/, found {apps}')
    app = os.path.join(payload, apps[0])
    bundle_id = plistlib.load(open(os.path.join(app, 'Info.plist'), 'rb'))['CFBundleIdentifier']
    report = []
    sign_bundle(app, report)
    print('\n'.join(report))
    out = out or os.path.join(work, apps[0][:-4] + '-r5q.ipa')
    with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED) as z:
        for root, _, names in os.walk(payload):
            for name in names:
                full = os.path.join(root, name)
                zi = zipfile.ZipInfo.from_file(full, os.path.relpath(full, work).replace(os.sep, '/'))
                zi.external_attr = ((0o755 if macho_kind(full) else 0o644) | 0o100000) << 16    # (Windows has no x bit)
                with open(full, 'rb') as f:
                    z.writestr(zi, f.read(), zipfile.ZIP_DEFLATED)
    print(f'ipa_install: {out} ({bundle_id})')
    if install:
        client = os.path.join(HERE, 'r5qd_client.py')
        dest = f'/var/r5q/{os.path.basename(out)}'
        env = dict(os.environ, MSYS_NO_PATHCONV='1')
        subprocess.run([sys.executable, client, 'put', out, dest], check=True, env=env)
        subprocess.run([sys.executable, client, 'run', '/var/r5q/bin/appinst', dest, bundle_id, 'Customer'], check=True, env=env)
        print(f'ipa_install: open it with: python phone.py sh /var/r5q/bin/openurl app:{bundle_id}')
    if out.startswith(work):
        print(f'ipa_install: (the signed copy stays in {work} until you delete it)')
    else:
        shutil.rmtree(work)


if __name__ == '__main__':
    main(sys.argv[1:])
