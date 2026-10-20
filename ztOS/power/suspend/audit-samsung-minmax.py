#!/usr/bin/env python3
"""Read Samsung ECT MINMAX calibration from the supplied CSE3 sboot image.

Layout follows ect_parse_gen_param_header/table and ect_parser.h. The GEN
offset is specific to the audited CSE3 bootloader; no device access occurs.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('sboot', type=Path)
a = p.parse_args()
b = a.sboot.read_bytes()
base = 0x54548
def word(pos):
    return struct.unpack_from('<I', b, pos)[0]
if word(base) != 1 or b[base + 4:base + 8] != b'0.0 ' or word(base + 8) != 27:
    raise SystemExit('unsupported Samsung GEN calibration layout')
pos = base + 12
tables = {}
for _ in range(word(base + 8)):
    length = word(pos)
    pos += 4
    if not 1 <= length <= 128 or b[pos + length] != 0:
        raise SystemExit('invalid calibration name')
    name = b[pos:pos + length].decode('ascii')
    pos += (length + 4) & ~3
    offset = word(pos)
    pos += 4
    if name in ('MINMAX_dvfs_mif', 'MINMAX_dvfs_int'):
        cols, rows = struct.unpack_from('<II', b, base + offset)
        if cols != 6 or not 1 <= rows <= 32:
            raise SystemExit('unsupported MINMAX table dimensions')
        entries = []
        for i in range(rows):
            values = struct.unpack_from('<6I', b, base + offset + 8 + i * 24)
            entries.append(dict(zip(('asv_version', 'table_version', 'min_mhz',
                                     'max_mhz', 'boot_mhz', 'resume_mhz'), values)))
        tables[name] = entries
if len(tables) != 2:
    raise SystemExit('MIF/INT calibration tables missing')
print(json.dumps(dict(sboot_sha256=hashlib.sha256(b).hexdigest(),
                     gen_offset=base, tables=tables), indent=2))
