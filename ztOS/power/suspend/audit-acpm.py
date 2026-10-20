#!/usr/bin/env python3
"""Decode MIF tables from the audited live Exynos9810 ACPM SRAM image.

Offline only. Offsets describe this firmware layout, not arbitrary ACPM builds.
Preserve unknown operation numbers rather than guessing their semantics.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('image', type=Path)
p.add_argument('--boot-image', action='store_true',
               help='decode embedded ACPM before its runtime interface is initialized')
a = p.parse_args()
b = a.image.read_bytes()
if len(b) != 65536:
    raise SystemExit('expected a 64 KiB ACPM SRAM snapshot')

def word(offset):
    if not 0 <= offset <= len(b) - 4:
        raise ValueError('firmware pointer outside snapshot')
    return struct.unpack_from('<I', b, offset)[0]

def offset(pointer):
    value = pointer - 0x7000
    word(value)
    return value

# Follow the callback loaded by BLX at image offset 0x9450.
if a.boot_image:
    # Static operations table, installed into the interface at ACPM startup.
    ops = 0xb1f0
else:
    interface = offset(word(offset(word(0x94dc))))
    ops = offset(word(interface + 4))
callback = offset(word(ops + 12) & ~1)
if callback != 0x8974 or b[callback:callback + 2] != bytes.fromhex('70b5'):
    raise SystemExit('unsupported ACPM callback layout; inspect before decoding')
table = offset(word(0x89c8))
tables = {}
for mode, label in enumerate(('sicd', 'sleep', 'sleep_usbl2', 'stop')):
    row = table + mode * 20
    if word(row) != mode:
        raise SystemExit('unexpected ACPM mode table')
    for direction, pointer_at, count_at in [('up', 4, 12), ('down', 8, 16)]:
        count = word(row + count_at)
        if count > 128:
            raise SystemExit('invalid sequencer length')
        entries = []
        if count:
            start = offset(word(row + pointer_at))
            for i in range(count):
                pos = start + i * 24
                words = [word(pos + j * 4) for j in range(6)]
                entries.append(dict(index=i, image_offset=pos, operation=words[0],
                                    address=words[1], mask=words[2], value=words[3],
                                    auxiliary=words[4:]))
        tables[f'{label}_{direction}'] = entries
print(json.dumps(dict(sha256=hashlib.sha256(b).hexdigest(),
                     mif_down_callback=callback, tables=tables), indent=2))
