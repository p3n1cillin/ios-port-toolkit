"""r5q_pull.py PHONE_PATH LOCAL_DIR: a file or a whole tree from the running iOS to the PC, over r5qd (wifi/r5qd.c),
binary-safe, 60000 bytes a request (r5qfs dump; r5qd relays 64 KB of a program's output).
(2026-11-20, Test 373: the unified logs -- /private/var/db/diagnostics and /private/var/db/uuidtext -- to read the
baseband manager's info-level lines on the PC.) Run with MSYS_NO_PATHCONV=1 from Git Bash.
Private data: what is pulled stays local (work/, never pushed)."""
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import r5qd_client  # noqa: E402

CHUNK = 60000
IP = r5qd_client.IP_FILE.read_text().strip()


def raw(line):
    import socket
    with socket.create_connection((IP, r5qd_client.PORT), timeout=10) as s:
        s.settimeout(30)
        s.sendall(f'{r5qd_client.TOKEN} {line}\n'.encode())
        out = b''
        while True:
            c = s.recv(65536)
            if not c:
                break
            out += c
    tail = out.rfind(b'\nexit ')
    if tail < 0:
        raise RuntimeError(f'no exit line for {line!r}: {out[-200:]!r}')
    return out[:tail], int(out[tail + 6:].split()[0])


def entries(path):
    """(kind, size, path) of PATH itself, or of each entry if PATH is a directory"""
    text, _ = raw(f'run /var/r5q/bin/r5qfs ls {path}')
    out = []
    for line in text.decode(errors='replace').splitlines():
        parts = line.split(None, 3)
        if len(parts) == 4 and parts[0][0] in '-dl' and parts[2].isdigit():
            out.append((parts[0][0], int(parts[2]), parts[3]))
    return out


def pull_file(src, size, dst):
    """resumes: a local file shorter than the phone's is continued from where it stops; each chunk tried 4 times"""
    dst.parent.mkdir(parents=True, exist_ok=True)
    off = dst.stat().st_size if dst.exists() else 0
    if off > size:
        off = 0
    with open(dst, 'r+b' if off else 'wb') as f:
        f.seek(off)
        f.truncate()
        while off < size:
            for attempt in range(4):
                try:
                    data, code = raw(f'run /var/r5q/bin/r5qfs dump {src} {off} {CHUNK}')
                    break
                except (OSError, RuntimeError) as e:
                    print(f'  {src} at {off}: {type(e).__name__}, again', flush=True)
            else:
                raise RuntimeError(f'{src} at {off}: gave up')
            if code != 0 or not data:
                raise RuntimeError(f'{src} at {off}: exit {code}')
            f.write(data)
            off += len(data)
    return size


def pull(src, dst):
    total = 0
    for kind, size, path in entries(src):
        rel = path[len(src):].lstrip('/') if path != src else pathlib.PurePosixPath(src).name
        if kind == 'd':
            total += pull(path, dst / rel)
        elif kind == '-':
            total += pull_file(path, size, dst / rel)
    return total


if __name__ == '__main__':
    src, dst = sys.argv[1].rstrip('/'), pathlib.Path(sys.argv[2])
    n = pull(src, dst)
    print(f'{n} bytes from {src} into {dst}')
