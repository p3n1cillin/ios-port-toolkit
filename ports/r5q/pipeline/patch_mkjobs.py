#!/usr/bin/env python3
"""patch_mkjobs.py MKJOBS: give the donor userland's launchd job generator (mkjobs.py) two options, idempotently.

JOBENV="label:VAR=value ..." sets environment variables in one job's plist (the value may hold ':' and ','; no
spaces). (r5q Test 256) Bluetooth: bluetoothd's transport is chosen by BT_HCI_TRANSPORT, so
JOBENV="com.apple.bluetoothd:BT_HCI_TRANSPORT=H4:POSIX:com.apple.uart.bluetooth,115200" points it at the kernel's
kernel-control socket instead of the Skywalk one an iPhone has.

RELABEL="old=new ..." gives an on-demand job another label (and plist name); its Mach services stay. (r5q Test 258)
launchd panics the whole system -- "userspace panic: unexpected exec of non-platform binary: /redwood/bin/r5q_bluetool:
com.apple.BlueTool" (Test 257) -- when a job labelled com.apple.* runs a program that is not an Apple platform
binary. The port's own stand-in for BlueTool is therefore com.redwood.BlueTool, still serving com.apple.BlueTool.

Run by pipeline/build_root_app.sh before each root build; a second run changes nothing.
"""
import sys

path = sys.argv[1]
text = open(path).read()
before = text
anchor = "RELOCATE = dict(word.split('=', 1) for word in os.environ.get('RELOCATE', '').split() if '=' in word)\n"
main_writer = "    with open(f'{dest}/{label}.plist', 'wb') as f:\n"
rest_writer = "        with open(f'{dest}/{name}', 'wb') as f:\n"
for needed in (anchor, main_writer, rest_writer):
    assert needed in text, 'mkjobs.py: a line this patch needs moved: ' + needed.strip()

if 'JOBENV' not in text:
    text = text.replace(anchor, anchor + '''
# (r5q, Test 256) JOBENV="label:VAR=value ...": environment variables for one job (pipeline/patch_mkjobs.py) -- for
# bluetoothd's BT_HCI_TRANSPORT, which picks the kernel's com.apple.uart.bluetooth socket
JOBENV = {}
for word in os.environ.get('JOBENV', '').split():
    if ':' in word and '=' in word.split(':', 1)[1]:
        env_label, setting = word.split(':', 1)
        env_var, env_value = setting.split('=', 1)
        JOBENV.setdefault(env_label, {})[env_var] = env_value
''', 1)
    for writer in (main_writer, rest_writer):
        indent = writer[:len(writer) - len(writer.lstrip())]
        text = text.replace(writer, f"{indent}if label in JOBENV:\n"
                                    f"{indent}    job.setdefault('EnvironmentVariables', {{}}).update(JOBENV[label])\n"
                                    f"{indent}    print(f'  job {{label}}: environment {{JOBENV[label]}} (JOBENV)')\n"
                            + writer, 1)

if 'RELABEL' not in text:
    text = text.replace(anchor, anchor + '''
# (r5q, Test 258) RELABEL="old=new ...": an on-demand job under another label, its Mach services kept
# (pipeline/patch_mkjobs.py) -- launchd panics when a com.apple.* job runs a program that is not Apple's
RELABEL = dict(word.split('=', 1) for word in os.environ.get('RELABEL', '').split() if '=' in word)
''', 1)
    text = text.replace(rest_writer, "        if label in RELABEL:\n"
                                     "            job['Label'] = RELABEL[label]\n"
                                     "            name = RELABEL[label] + '.plist'\n"
                                     "            print(f'  job {label}: labelled {RELABEL[label]} (RELABEL)')\n"
                        + rest_writer, 1)

if text != before:
    open(path, 'w').write(text)
    print('mkjobs.py: JOBENV / RELABEL added')
