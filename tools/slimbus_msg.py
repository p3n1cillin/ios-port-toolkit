"""SLIMbus messages as a Qualcomm NGD (the SoC's satellite SLIMbus controller) sends and receives them: decode the
bytes a driver logged, or build the ones it must send. For the WCD93xx codecs (WCD9340 "Tavil", WCD9335 "Tasha") on
SDM845/SM8150/SM8250-era phones, where the microphone and the earpiece/headset paths go through SLIMbus.

    python slimbus_msg.py decode 4a 0d ff 51 00 01 50 02 17 02 00
    python slimbus_msg.py decode 06 68 c4 18 80 01            # CHANGE_VALUE: codec register write
    python slimbus_msg.py write LA OFFSET VALUE                # e.g. write 0xc4 0x000 0x06 (the page register)
    python slimbus_msg.py read LA OFFSET [TID] [--bytes N]
    python slimbus_msg.py addr-query ENUMADDR [TID]            # e.g. addr-query 00:01:50:02:17:02
    python slimbus_msg.py selftest

Message layout (Linux slimbus.h SLIM_MSG_ASM_FIRST_WORD, as the NGD's TX FIFO takes it, little-endian bytes):
  byte 0: length (bits 4-0, the whole message in bytes) | message type << 5
  byte 1: message code (bits 6-0) | destination type << 7 (0 = logical address, 1 = enumeration address)
  byte 2: the destination's logical address (0xff = the manager); then the payload.
Value elements (REQUEST_VALUE / CHANGE_VALUE): element code = slice size code | 0x8 (byte access) | address << 4;
a WCD codec's register at offset o of the current page is value element 0x800 + o (the page goes to offset 0).

Lessons from the S10 Lite (r5q Tests 419-433):
  * ADDR_QUERY is a *user* message, type 2 ("destination referred user"), code 0x0d, to the manager (0xff): first byte
    0x4a. Sent as type 5 it is silently ignored and the codec looks dead (Test 419).
  * user messages to the manager (CONNECT_SRC 0x2c, DEF_ACT_CHAN 0x21, CHAN_CTRL 0x23, RECONFIG_NOW 0x24,
    DISCONNECT_PORT 0x2e) are answered by GENERIC_ACK (0x25): tid, then a response byte; bit 5 set = success.
  * tear down in the reverse order you set up (CHAN_CTRL remove, RECONFIG_NOW, DISCONNECT_PORT, RECONFIG_NOW), or the
    next session's capture is silent (Test 433).
"""
from __future__ import annotations

import argparse
import sys

MT = {0: 'core', 1: 'destination-referred class', 2: 'destination-referred user', 5: 'source-referred class',
      6: 'source-referred user'}
CORE_MC = {0x01: 'REPORT_PRESENT', 0x02: 'ASSIGN_LOGICAL_ADDRESS', 0x04: 'RESET_DEVICE', 0x08: 'CHANGE_LOGICAL_ADDRESS',
           0x09: 'CHANGE_ARBITRATION_PRIORITY', 0x0c: 'REQUEST_SELF_ANNOUNCEMENT', 0x0f: 'REPORT_ABSENT',
           0x10: 'CONNECT_SOURCE', 0x11: 'CONNECT_SINK', 0x14: 'DISCONNECT_PORT', 0x18: 'CHANGE_CONTENT',
           0x20: 'REQUEST_INFORMATION', 0x21: 'REQUEST_CLEAR_INFORMATION', 0x24: 'REPLY_INFORMATION',
           0x28: 'CLEAR_INFORMATION', 0x29: 'REPORT_INFORMATION', 0x40: 'BEGIN_RECONFIGURATION',
           0x44: 'NEXT_ACTIVE_FRAMER', 0x45: 'NEXT_SUBFRAME_MODE', 0x46: 'NEXT_CLOCK_GEAR', 0x47: 'NEXT_ROOT_FREQUENCY',
           0x4a: 'NEXT_PAUSE_CLOCK', 0x4b: 'NEXT_RESET_BUS', 0x4c: 'NEXT_SHUTDOWN_BUS', 0x50: 'NEXT_DEFINE_CHANNEL',
           0x51: 'NEXT_DEFINE_CONTENT', 0x54: 'NEXT_ACTIVATE_CHANNEL', 0x55: 'NEXT_DEACTIVATE_CHANNEL',
           0x58: 'NEXT_REMOVE_CHANNEL', 0x5f: 'RECONFIGURE_NOW', 0x60: 'REQUEST_VALUE', 0x61: 'REQUEST_CHANGE_VALUE',
           0x64: 'REPLY_VALUE', 0x68: 'CHANGE_VALUE'}
# Qualcomm's user messages between an NGD and the ADSP's SLIMbus manager (slim-msm.h SLIM_USR_MC_*)
USER_MC = {0x00: 'MASTER_CAPABILITY', 0x01: 'REPORT_SATELLITE', 0x0d: 'ADDR_QUERY', 0x0e: 'ADDR_REPLY',
           0x20: 'DEFINE_CHAN', 0x21: 'DEF_ACT_CHAN', 0x23: 'CHAN_CTRL', 0x24: 'RECONFIG_NOW', 0x25: 'GENERIC_ACK',
           0x28: 'REQ_BW', 0x2c: 'CONNECT_SRC', 0x2d: 'CONNECT_SINK', 0x2e: 'DISCONNECT_PORT'}
SLICE = {1: 0, 2: 1, 3: 2, 4: 3, 6: 4, 8: 5, 12: 6, 16: 7}
SLICE_BACK = {v: k for k, v in SLICE.items()}


def ec(address, nbytes=1):
    if nbytes not in SLICE:
        raise SystemExit(f'a value element slice is 1, 2, 3, 4, 6, 8, 12 or 16 bytes, not {nbytes}')
    return SLICE[nbytes] | 0x8 | (address << 4)


def ec_decode(code):
    return {'address': code >> 4, 'bytes': SLICE_BACK[code & 7], 'byte_access': bool(code & 8)}


def msg(mt, mc, dest, payload, dest_enum=False):
    length = 3 + (5 if dest_enum else 0) + len(payload)
    if length > 31:
        raise SystemExit('a SLIMbus message is at most 31 bytes')
    head = [length | (mt << 5), (mc & 0x7f) | (0x80 if dest_enum else 0)]
    head += list(dest) if dest_enum else [dest]
    return bytes(head + list(payload))


def change_value(la, offset, value, base=0x800):
    c = ec(base + offset)
    return msg(0, 0x68, la, [c & 0xff, c >> 8, value])


def request_value(la, offset, tid, nbytes=1, base=0x800):
    c = ec(base + offset, nbytes)
    return msg(0, 0x60, la, [tid, c & 0xff, c >> 8])


def addr_query(enum_addr, tid):
    return msg(2, 0x0d, 0xff, [tid] + list(enum_addr))


def decode(b):
    b = bytes(b)
    if len(b) < 2:
        return 'too short'
    length, mt = b[0] & 0x1f, b[0] >> 5
    mc, dt = b[1] & 0x7f, b[1] >> 7
    user = mt in (2, 6)
    name = (USER_MC if user else CORE_MC).get(mc, f'mc {mc:#04x}')
    out = [f'{name} ({MT.get(mt, f"type {mt}")}, mc {mc:#04x}), length {length}' +
           ('' if length == len(b) or len(b) >= 4 and length <= len(b) else f' (have {len(b)} bytes)')]
    if dt:
        dest, rest = b[2:8], b[8:length]
        out.append('to enumeration address ' + ':'.join(f'{x:02x}' for x in dest))
    else:
        rest = b[3:length]
        if len(b) > 2:
            who = 'the manager' if b[2] == 0xff else f'logical address {b[2]:#04x}'
            out.append(f'{"from" if mt in (5, 6) else "to"} {who}')
    if mc == 0x0d and mt not in (2, 6) and len(b) > 2 and b[2] == 0xff and len(rest) >= 7:
        out.append('WARNING: this looks like ADDR_QUERY (code 0x0d to the manager) sent as message type '
                   f'{mt}; it must be type 2 (first byte 0x4a) or the manager ignores it (r5q Test 419)')
    elif not user and mc in (0x68, 0x61) and len(rest) >= 3:
        e = ec_decode(rest[0] | rest[1] << 8)
        out.append(f'element {e["address"]:#05x} ({_wcd(e["address"])}), {e["bytes"]} byte(s): value '
                   + ' '.join(f'{x:02x}' for x in rest[2:]))
    elif not user and mc == 0x60 and len(rest) >= 3:
        e = ec_decode(rest[1] | rest[2] << 8)
        out.append(f'tid {rest[0]:#04x}, element {e["address"]:#05x} ({_wcd(e["address"])}), {e["bytes"]} byte(s)')
    elif not user and mc == 0x64 and rest:
        out.append(f'tid {rest[0]:#04x}, value ' + ' '.join(f'{x:02x}' for x in rest[1:]))
    elif user and mc == 0x0d and len(rest) >= 7:
        out.append(f'tid {rest[0]:#04x}, asks for the logical address of ' + ':'.join(f'{x:02x}' for x in rest[1:7]))
        if mt != 2:
            out.append('WARNING: ADDR_QUERY must be message type 2 (first byte 0x4a); this one is ignored by the manager '
                       '(r5q Test 419)')
    elif user and mc == 0x0e and len(rest) >= 8:
        ea, la = rest[1:7], rest[7]
        out.append(f'tid {rest[0]:#04x}: ' + ':'.join(f'{x:02x}' for x in ea) +
                   (' not known yet (all zero: retry after the codec reset)' if not any(ea) else f' has logical address {la:#04x}'))
    elif user and mc == 0x25 and len(rest) >= 2:
        out.append(f'tid {rest[0]:#04x}, response {rest[1]:#04x}: ' + ('success' if rest[1] & 0x20 else 'REFUSED'))
    elif rest:
        out.append('payload ' + ' '.join(f'{x:02x}' for x in rest))
    return '; '.join(out)


def _wcd(addr):
    if 0x800 <= addr < 0x900:
        o = addr - 0x800
        return 'the page register' if o == 0 else f'WCD register offset {o:#04x} of the current page'
    return 'not a WCD value-map address'


def parse_bytes(words):
    s = ''.join(words).replace(',', ' ').replace('0x', ' ')
    parts = s.split() if ' ' in s.strip() else [s[i:i + 2] for i in range(0, len(s), 2)]
    return bytes(int(p, 16) for p in parts)


def selftest():
    cases = [
        # (what, built, expected bytes) -- every expected value is a message the r5q driver sent and the codec obeyed
        ('ADDR_QUERY for the WCD9340 codec (Test 419/420)', addr_query(bytes.fromhex('000150021702'), 0x51),
         bytes.fromhex('4a0dff51000150021702')),
        ('REQUEST_VALUE 4 bytes at 0x821 (the chip id, Test 420)', request_value(0xc4, 0x21, 0x1b, 4),
         bytes([0x06, 0x60, 0xc4, 0x1b, 0x1b, 0x82])),
        ('CHANGE_VALUE page register = 6', change_value(0xc4, 0x000, 0x06), bytes([0x06, 0x68, 0xc4, 0x08, 0x80, 0x06])),
    ]
    bad = 0
    for what, got, want in cases:
        ok = got == want
        bad += not ok
        print(f'{"PASS" if ok else "FAIL"}  {what}: {got.hex(" ")}' + ('' if ok else f' (want {want.hex(" ")})'))
    # Test 419's ignored query (type 5), and GENERIC_ACKs as the manager sends them (type 6, source-referred user)
    for b, needle in ((bytes.fromhex('aa0dff51000150021702'), 'WARNING'),
                      (bytes([0xc5, 0x25, 0xff, 0x81, 0x20]), 'success'),
                      (bytes([0xc5, 0x25, 0xff, 0x81, 0x00]), 'REFUSED'),
                      (bytes.fromhex('4a0dff51000150021702'), 'asks for the logical address')):
        d = decode(b)
        ok = needle in d
        bad += not ok
        print(f'{"PASS" if ok else "FAIL"}  decode {b.hex(" ")} -> {d}')
    return 1 if bad else 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    sub = ap.add_subparsers(dest='cmd', required=True)
    d = sub.add_parser('decode')
    d.add_argument('bytes', nargs='+')
    w = sub.add_parser('write')
    w.add_argument('la', type=lambda s: int(s, 0))
    w.add_argument('offset', type=lambda s: int(s, 0))
    w.add_argument('value', type=lambda s: int(s, 0))
    r = sub.add_parser('read')
    r.add_argument('la', type=lambda s: int(s, 0))
    r.add_argument('offset', type=lambda s: int(s, 0))
    r.add_argument('tid', nargs='?', type=lambda s: int(s, 0), default=0x80)
    r.add_argument('--bytes', type=int, default=1)
    q = sub.add_parser('addr-query')
    q.add_argument('enum_addr')
    q.add_argument('tid', nargs='?', type=lambda s: int(s, 0), default=0x51)
    sub.add_parser('selftest')
    a = ap.parse_args(argv)
    if a.cmd == 'decode':
        print(decode(parse_bytes(a.bytes)))
    elif a.cmd in ('write', 'read', 'addr-query'):
        if a.cmd == 'write':
            m = change_value(a.la, a.offset, a.value)
        elif a.cmd == 'read':
            m = request_value(a.la, a.offset, a.tid, a.bytes)
        else:
            m = addr_query(parse_bytes([a.enum_addr.replace(':', ' ')]), a.tid)
        print(m.hex(' '))
        print(decode(m))
    elif a.cmd == 'selftest':
        return selftest()
    return 0


if __name__ == '__main__':
    sys.exit(main())
