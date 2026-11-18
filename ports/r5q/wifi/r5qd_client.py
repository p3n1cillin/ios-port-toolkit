"""r5qd_client.py: the PC side of r5qd (wifi/r5qd.c), changes to a running iOS without a reboot.

  python wifi/r5qd_client.py [--ip A.B.C.D] ping
  python wifi/r5qd_client.py put LOCAL_FILE /var/r5q/PATH
  python wifi/r5qd_client.py run /path/to/prog ARGS...
  python wifi/r5qd_client.py kill NAME [SIG]
  python wifi/r5qd_client.py inject ["LIB NAME...;LIB NAME..."]
  python wifi/r5qd_client.py reload LIB_FILE PROGRAM     put LIB into /var/r5q/lib, add "LIB PROGRAM" to kern.exec_inject
                                                         (replacing a group with the same library name), kill PROGRAM

The token is work/ios-r5q/r5qd.token (shipped in the image as /redwood/share/r5qd/token); the phone's address is the
last one seen (r5qd.ip), or --ip; the phone says it in its log ("r5qd: listening on ...").
From Git Bash, paths like /bin/ps are rewritten to Windows paths before Python sees them: run with MSYS_NO_PATHCONV=1
there (cmd and PowerShell leave them alone).
"""
import pathlib
import socket
import sys

HERE = pathlib.Path(__file__).resolve().parent.parent
TOKEN = (HERE / 'r5qd.token').read_text().strip()
IP_FILE = HERE / 'r5qd.ip'
PORT = 7777


def request(ip, line, payload=b'', timeout=150):
    with socket.create_connection((ip, PORT), timeout=10) as s:
        s.settimeout(timeout)
        s.sendall(f'{TOKEN} {line}\n'.encode() + payload)
        out = b''
        while True:
            chunk = s.recv(65536)
            if not chunk:
                break
            out += chunk
    return out.decode(errors='replace')


def main(argv):
    ip = None
    if argv[:1] == ['--ip']:
        ip, argv = argv[1], argv[2:]
        IP_FILE.write_text(ip + '\n')
    ip = ip or (IP_FILE.read_text().strip() if IP_FILE.exists() else None)
    if not ip or not argv:
        raise SystemExit(__doc__)
    cmd, rest = argv[0], argv[1:]
    if cmd == 'put':
        data = pathlib.Path(rest[0]).read_bytes()
        print(request(ip, f'put {rest[1]} {len(data)}', data), end='')
    elif cmd == 'reload':
        lib, program = pathlib.Path(rest[0]), rest[1]
        dest = f'/var/r5q/lib/{lib.name}'
        print(request(ip, f'put {dest} {lib.stat().st_size}', lib.read_bytes()), end='')
        groups = [g.split() for g in request(ip, 'inject').strip().split(';') if g.strip()]
        kept = []
        for words in groups:
            library, names = words[0], words[1:]
            if library == dest:
                names = [n for n in names if n != program]          # (our own group from an earlier reload)
            elif library.endswith('/' + lib.name):
                names = [n for n in names if n != program]          # (the image's copy: PROGRAM now gets ours)
            if names:
                kept.append(' '.join([library] + names))
        kept.append(f'{dest} {program}')
        groups = kept
        print(request(ip, 'inject ' + ';'.join(groups)), end='')
        print(request(ip, f'kill {program}'), end='')
    else:
        print(request(ip, ' '.join([cmd] + rest)), end='')


if __name__ == '__main__':
    main(sys.argv[1:])
