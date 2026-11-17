"""Drive iOS on the r5q twin: touch, buttons, screenshots. EMULATION ONLY.

Start a run with input:   TWIN_INPUT=1 TWIN_SMP=4 TWIN_CMDLINE=cpus=4 python3 run_twin.py ios 2700 &
then:                     python3 twin_ctl.py STEP...

  shot NAME                    save the screen to <run out>/drive/<NNN>-NAME.png (540x1200)
  tap X Y                      a short touch; X Y are fractions of the screen (0 0 top left, 1 1 bottom right)
  hold X Y MS                  touch and stay
  swipe X0 Y0 X1 Y1 [MS]       drag (default 300 ms)
  key NAME [MS]                button: home, lock, volup, voldown
  wait SECONDS
  log PATTERN                  print the last 5 serial lines that contain PATTERN
"""
import json
import pathlib
import socket
import sys
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import run_twin  # noqa: E402

WORK = pathlib.Path('/var/tmp/codex-ios-r5q/work/twin-runs-ios')
run_dir, out_dir = (pathlib.Path(p) for p in (WORK / 'current-run').read_text().split())
KEYS = {'home': 'h', 'lock': 'l', 'volup': 'up', 'voldown': 'down'}


class Qmp:
    def __init__(self):
        self.sock = socket.socket(socket.AF_UNIX)
        self.sock.connect(str(run_dir / 'qmp.sock'))
        self.file = self.sock.makefile('rw')
        self.file.readline()
        self.command('qmp_capabilities')

    def command(self, name, **arguments):
        self.file.write(json.dumps({'execute': name, 'arguments': arguments}) + '\n')
        self.file.flush()
        while True:
            reply = json.loads(self.file.readline())
            if 'return' in reply or 'error' in reply:
                if 'error' in reply:
                    raise SystemExit(f'{name}: {reply["error"]["desc"]}')
                return reply['return']

    def pointer(self, x, y, down=None):
        ev = [{'type': 'abs', 'data': {'axis': 'x', 'value': round(x * 0x7fff)}},
              {'type': 'abs', 'data': {'axis': 'y', 'value': round(y * 0x7fff)}}]
        if down is not None:
            ev.append({'type': 'btn', 'data': {'button': 'left', 'down': down}})
        self.command('input-send-event', events=ev)

    def key(self, name, down):
        self.command('input-send-event', events=[{'type': 'key', 'data': {'key': {'type': 'qcode', 'data': name}, 'down': down}}])


def shot(name):
    drive = out_dir / 'drive'
    drive.mkdir(parents=True, exist_ok=True)
    n = len(list(drive.glob('*.png')))
    run_twin.hmp(run_dir / 'mon.sock', f'pmemsave {run_twin.FB:#x} {run_twin.FB_W * run_twin.FB_H * 4:#x} "{run_dir}/drive-fb.bin"')
    path = drive / f'{n:03d}-{name}.png'
    run_twin.png(path, (run_dir / 'drive-fb.bin').read_bytes(), run_twin.FB_W, run_twin.FB_H, 2)
    print(path)


def main(args):
    q = None
    while args:
        step = args.pop(0)
        if step == 'shot':
            shot(args.pop(0))
            continue
        if step == 'wait':
            time.sleep(float(args.pop(0)))
            continue
        if step == 'log':
            pat = args.pop(0)
            lines = [l for l in (out_dir / 'serial.log').read_text(errors='replace').splitlines() if pat in l]
            print('\n'.join(l[:220] for l in lines[-5:]))
            continue
        q = q or Qmp()
        if step in ('tap', 'hold'):
            x, y = float(args.pop(0)), float(args.pop(0))
            ms = float(args.pop(0)) if step == 'hold' else 80
            q.pointer(x, y)
            q.pointer(x, y, True)
            time.sleep(ms / 1000)
            q.pointer(x, y, False)
        elif step == 'swipe':
            x0, y0, x1, y1 = (float(args.pop(0)) for _ in range(4))
            ms = float(args.pop(0)) if args and args[0].replace('.', '').isdigit() else 300
            q.pointer(x0, y0)
            q.pointer(x0, y0, True)
            n = max(4, int(ms / 16))
            for i in range(1, n + 1):
                time.sleep(ms / 1000 / n)
                q.pointer(x0 + (x1 - x0) * i / n, y0 + (y1 - y0) * i / n)
            q.pointer(x1, y1, False)
        elif step == 'key':
            name = KEYS[args.pop(0)]
            ms = float(args.pop(0)) if args and args[0].isdigit() else 120
            q.key(name, True)
            time.sleep(ms / 1000)
            q.key(name, False)
        else:
            raise SystemExit(f'unknown step {step}\n{__doc__}')


if __name__ == '__main__':
    main(sys.argv[1:])
