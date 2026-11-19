#!/usr/bin/env python3
"""camsensor_v14.py MODULE.bin OUT.tab|-: the S10 Lite's camera sensor module files ("QTI Chromatix Header /
Parameter Parser V1.4.0", /vendor/lib64/camera/com.samsung.sensormodule.<n>_<maker>_<sensor>.bin) made into the
kernel camera driver's register tables ("RWCTAB02", the format of the donor's tools/camsensor.py, which reads the
POCO X5 Pro's V3.0.1 files only). 2026-11-19, from s5k3l6 (see wifi/CAMERA-PLAN.md).

The module files are the vendor's: kept outside the repository, and so is what this makes of them.

V1.4.0 against V3.0.1: the same tree of symbols, every field 64 bits wide.
  - Entry table at the offset in the u32 at 0x98 (count at 0x9c): {id, offset, size} u64 each; entry 0 the symbols,
    entry 1 the data pool.
  - A symbol: {id u64, name[32], kind i64, parent i64, offset in the pool i64, size i64} (72 bytes).
  - The sensor driver's structure at the start of the pool, u64 fields: [5] slave (8-bit), [6] address bytes and
    [7] data bytes of the ID read, [8] ID address, [9] ID, [13]/[14] power-up (count, symbol), [15]/[16] power-down.
  - A power step: {kind, value, delay ms} u64 (CamX kinds; 10 here is the reset line).
  - A resolution: 264 bytes = 33 u64: [0] colour filter order, [2] stream configuration symbol ({?, data type, x,
    y, width, height, bits, ?} u64), [3] line length, [4] frame length, [7] output pixel clock, [10] fps (a
    double), [11] lanes, [14] settle, [15] C-PHY, [18] register settings count, [19] their symbol.
  - A register setting: 80 bytes = 10 u64 {has slave, slave symbol, address, count, data symbol, address bytes,
    data bytes, operation, has delay, delay symbol}; data and delay u64 values.
  - The other tables are found as camsensor.py finds them: by the symbol before them (regToRealGain: on, off, holdon,
    holdoff; initSettings: init; testPatternData: tpgoff, tpgon).
"""
import struct
import sys

CFA = {0: 'BGGR', 1: 'GBRG', 2: 'GRBG', 3: 'RGGB', 4: 'mono'}


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    data = open(sys.argv[1], 'rb').read()
    if not data.startswith(b'QTI Chromatix Header') or b'Parameter Parser V1.4.0' not in data[:0x60]:
        sys.exit('camsensor_v14.py: not a V1.4.0 module file')
    table, count = struct.unpack_from('<II', data, 0x98)
    entries = [struct.unpack_from('<3Q', data, table + 24 * i) for i in range(count)]
    sym_at, sym_size = entries[0][1], entries[0][2]
    pool = entries[1][1]
    syms, order = {}, []
    for at in range(sym_at, sym_at + sym_size - 71, 72):
        sid = struct.unpack_from('<Q', data, at)[0]
        name = data[at + 8:at + 40].split(b'\0')[0].decode('latin1')
        _, _, ref, size = struct.unpack_from('<4q', data, at + 40)
        syms[sid] = (name, ref, size)
        order.append(sid)

    def value(sid):
        _, ref, size = syms[sid]
        return data[pool + ref:pool + ref + size]

    def u64s(sid):
        v = value(sid)
        return struct.unpack('<%dQ' % (len(v) // 8), v)

    def regsetting(sid):
        _, ref, size = syms[sid]
        writes = []
        for k in range(size // 80):
            (has_slave, slave_sid, address, cnt, data_sid, address_bytes, data_bytes, operation, has_delay,
             delay_sid) = struct.unpack_from('<10Q', data, pool + ref + 80 * k)
            values = u64s(data_sid)
            delay = u64s(delay_sid)[0] if has_delay and syms[delay_sid][2] >= 8 else 0
            for v in values[:max(cnt, 1)]:
                writes.append((address, v, address_bytes, data_bytes, operation, delay))
        return writes

    drv = struct.unpack_from('<20Q', data, pool)
    slave, address_bytes, data_bytes, id_address, chip_id = drv[5], drv[6], drv[7], drv[8], drv[9]
    power = {}
    for name, (cnt, sid) in (('pwrup', drv[13:15]), ('pwrdn', drv[15:17])):
        w = u64s(sid)
        power[name] = [w[i:i + 3] for i in range(0, min(len(w), 3 * cnt), 3)]
    res_sid = next(s for s in order if syms[s][0] == 'resolutionData')
    raw = value(res_sid)
    modes, mode_tables, cfa, mode_sids = [], [], [], set()
    for k in range(len(raw) // 264):
        w = struct.unpack_from('<33Q', raw, 264 * k)
        fps = struct.unpack_from('<d', raw, 264 * k + 80)[0]
        stream = u64s(w[2])
        data_type, x, y, width, height = stream[1:6]
        modes.append((width, height, x, y, w[11], w[15], w[14], w[7], w[3], w[4], int(fps * 1000), data_type))
        cfa.append(CFA.get(w[0], str(w[0])))
        mode_sids.add(w[19])
        mode_tables.append(('res%d' % k, regsetting(w[19])))
    lane_sid = next((s for s in order if syms[s][0] == 'laneAssign'), None)
    lane_assign = 0
    if lane_sid is not None and syms[lane_sid][2] >= 2:
        lane_assign = struct.unpack('<H', value(lane_sid)[:2])[0]

    roles = {'regToRealGain': ['on', 'off', 'holdon', 'holdoff'], 'initSettings': ['init'], 'testPatternData': ['tpgoff', 'tpgon']}
    tables, after, used = [], '', {}
    for sid in order:
        name = syms[sid][0]
        if name != 'regSetting':
            if name not in ('slaveAddr', 'registerData', 'delayUs'):
                after = name
            continue
        if sid in mode_sids:
            continue
        names = roles.get(after, [])
        n = used.get(after, 0)
        used[after] = n + 1
        if n < len(names):
            tables.append((names[n], regsetting(sid)))
    tables = mode_tables + tables

    print(f'camsensor_v14.py: slave {slave:#x} (7-bit {slave >> 1:#x}), id {chip_id:#x} at {id_address:#x} '
          f'({address_bytes}/{data_bytes} bytes), lane assignment {lane_assign:#x}')
    for name in ('pwrup', 'pwrdn'):
        print(f'  {name}: ' + ' '.join(f'{t}={v}+{d}ms' for t, v, d in power[name]))
    for k, m in enumerate(modes):
        print(f'  mode {k}: {m[0]}x{m[1]} at {m[2]},{m[3]} dt {m[11]:#x}, {m[4]} {"trio(s), C-PHY" if m[5] else "lane(s), D-PHY"}, '
              f'settle {m[6]}, pixel clock {m[7]}, line {m[8]}, frame {m[9]}, {m[10] / 1000:.2f} fps, {cfa[k]}')
    for name, writes in tables:
        print(f'  {name}: {len(writes)} writes: ' + ' '.join(f'{a:x}={v:x}' + (f'+{d}us' if d else '') for a, v, _, _, _, d in writes[:8]) +
              (' ...' if len(writes) > 8 else ''))
    if sys.argv[2] == '-':
        return
    out = bytearray(b'RWCTAB02')
    out += struct.pack('<8I', slave, address_bytes, data_bytes, id_address, chip_id, 0xffff, lane_assign, len(modes))
    for m in modes:
        out += struct.pack('<12I', *m)
    for name in ('pwrup', 'pwrdn'):
        out += name.encode().ljust(8, b'\0') + struct.pack('<I', len(power[name]))
        for t, v, d in power[name]:
            out += struct.pack('<HHBBBBI', t, v // 1000 if t == 0 else v, 0, 0, 0, 0, d * 1000)
    out += struct.pack('<I', len(tables))
    for name, writes in tables:
        out += name.encode().ljust(8, b'\0') + struct.pack('<I', len(writes))
        for a, v, ab, db, op, d in writes:
            out += struct.pack('<HHBBBBI', a & 0xffff, v & 0xffff, ab, db, op, 0, d)
    open(sys.argv[2], 'wb').write(out)
    print(f'camsensor_v14.py: {sys.argv[2]}: {len(out)} bytes')


main()
