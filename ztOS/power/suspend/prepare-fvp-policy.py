#!/usr/bin/env python3
"""Generate stock FVP bootstrap messages offline from CSE3 calibration.

No hardware access. MINLOCK layout follows ect_parse_minlock_header/domain;
message format follows exynos9810_mif_constraint_parse().
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
sha = hashlib.sha256(b).hexdigest()
if sha != '022de35476d5c938c228ea2cc03b31f963071e12778ead0cb589021c130ad59c':
    raise SystemExit('this parser requires the audited CSE3 sboot image')
base = 0x596b8
if struct.unpack_from('<I', b, base)[0] != 1 or struct.unpack_from('<I', b, base + 8)[0] != 4:
    raise SystemExit('unexpected MINLOCK block')
pos = base + 12
constraints = None
for _ in range(4):
    length = struct.unpack_from('<I', b, pos)[0]
    pos += 4
    if not 1 <= length <= 128 or b[pos + length] != 0:
        raise SystemExit('invalid MINLOCK domain')
    name = b[pos:pos + length].decode('ascii')
    pos += (length + 4) & ~3
    offset = struct.unpack_from('<I', b, pos)[0]
    pos += 4
    if name == 'dvfs_mif':
        count = struct.unpack_from('<I', b, base + offset)[0]
        if not 1 <= count <= 15:
            raise SystemExit('invalid MIF constraint count')
        constraints = [struct.unpack_from('<II', b, base + offset + 4 + i * 8)
                       for i in range(count)]
if constraints is None:
    raise SystemExit('MIF constraints missing')
# Every ASV row in this firmware has these limits and boot frequency.
rows = [(mif, interconnect) for mif, interconnect in constraints if 421000 <= mif <= 1794000]
if len(rows) != 8 or rows[0][0] != 1794000 or rows[-1] != (421000, 107000):
    raise SystemExit('unexpected filtered MIF policy')
messages = [[i, mif, 5, interconnect] for i, (mif, interconnect) in enumerate(rows)]
messages.append([len(rows), 1794000, 5, 1])
print(json.dumps(dict(sboot_sha256=sha, channel=1,
                     constraints=[dict(mif_khz=mif, int_min_khz=interconnect)
                                  for mif, interconnect in rows],
                     messages=messages), indent=2))
