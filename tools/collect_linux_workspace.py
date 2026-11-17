"""Archive the project-specific native Linux workspace, preserving symlinks."""
import datetime
import hashlib
import json
import pathlib
import subprocess
import sys
import tarfile

source = pathlib.Path('/var/tmp/codex-ios-r5q').resolve(strict=True)
destination = pathlib.Path(sys.argv[1]).resolve(strict=True)
if source != pathlib.Path('/var/tmp/codex-ios-r5q'):
    raise SystemExit('Unexpected Linux project source')
archive = destination / 'ubuntu-ios-workspace.tar.gz'
partial = destination / 'ubuntu-ios-workspace.tar.gz.partial'
if archive.exists() or partial.exists():
    raise SystemExit('An archive already exists; refusing to overwrite it')

subprocess.run(['/usr/bin/tar', '--sparse', '-I', 'gzip -1', '-cf', str(partial),
                '-C', str(source.parent), source.name], check=True, timeout=900)
count = 0
logical_bytes = 0
with tarfile.open(partial, 'r|gz') as items:
    for member in items:
        path = pathlib.PurePosixPath(member.name)
        if path.is_absolute() or '..' in path.parts or path.parts[0] != source.name:
            raise ValueError('Unexpected archive member path')
        count += 1
        if member.isfile():
            logical_bytes += member.size
digest = hashlib.sha256()
with partial.open('rb') as handle:
    for chunk in iter(lambda: handle.read(4 * 1024 * 1024), b''):
        digest.update(chunk)
partial.rename(archive)
report = {'archive': archive.name, 'source': str(source),
          'created_utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
          'bytes': archive.stat().st_size, 'sha256': digest.hexdigest(),
          'member_count': count, 'uncompressed_regular_file_bytes': logical_bytes,
          'validation': 'gzip/tar stream read through all members; relative names checked; SHA256 calculated',
          'live_workspace': 'kept at original path for Linux build compatibility',
          'contains': ['Mu-Silicium and submodules', 'Linux donor checkout',
                       'extracted phone XBL firmware and reports', 'Theos toolchain and iOS SDK',
                       'Python build environment', 'native board overlays and other project-specific files']}
(destination / 'archive-manifest.json').write_text(json.dumps(report, indent=2) + '\n')
(destination / 'SHA256SUMS').write_text(f'{digest.hexdigest()}  {archive.name}\n')
print(json.dumps(report, indent=2), flush=True)
