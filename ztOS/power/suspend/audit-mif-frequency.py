#!/usr/bin/env python3
"""Match read-only MIF clock registers to the captured Samsung FV map.

The input image is the 0x16c00..0x1f800 DVFS plugin snapshot; its FV map
starts at +0x6700. Register masks come from Exynos9810 cmucal-sfr.c.
This tool never maps hardware or sends frequency/voltage requests.
"""
import argparse
import json
from pathlib import Path
import struct

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--dvfs-image', type=Path, required=True)
p.add_argument('--registers', type=Path, required=True)
a = p.parse_args()
image = a.dvfs_image.read_bytes()
if len(image) != 0x8c00:
    raise SystemExit('expected the audited 35840-byte DVFS plugin snapshot')
data = image[0x6700:]
header = struct.unpack_from('<6BH4B6H', data)
kind, levels, members, plls, muxes, divs = header[:6]
if (kind, members, plls, muxes, divs) != (0, 7, 3, 2, 2) or not 1 <= levels <= 32:
    raise SystemExit('unsupported MIF FV-map layout')
member_offset, rates_offset, table_offset = header[-3:]
member_data = struct.unpack_from('<7H', data, member_offset)
registers = {int(k, 0): int(v, 0) if isinstance(v, str) else v
             for k, v in json.loads(a.registers.read_text()).items()}
addresses, pll_values = [], []
for entry in member_data[:3]:
    address, _, count = struct.unpack_from('<IHH', data, entry)
    if not 1 <= count <= 32:
        raise SystemExit('invalid PLL table length')
    addresses.append(address - 0x90000000)
    pll_values.append(struct.unpack_from('<' + 'I' * count, data, entry + 8))
block = header[11] << 16
addresses.extend(block + offset - 0x90000000 for offset in member_data[3:])
expected_addresses = [0x1b800120, 0x1a2401a0, 0x1a2402a0,
                      0x1a2410a4, 0x1a24102c, 0x1a2418a0, 0x1a241828]
if addresses != expected_addresses:
    raise SystemExit('unexpected clock members; inspect new firmware layout')
# PMS: M bits16..25, P bits8..13, S bits0..2. Exclude enable,
# lock/status and output-mux bits when comparing PLL parameters.
masks = [0x03ff3f07] * 3 + [1, 7, 15, 15]
actual = [registers[address] & mask for address, mask in zip(addresses, masks)]
matches, supported = [], []
for level in range(levels):
    rate, voltage = struct.unpack_from('<II', data, rates_offset + level * 8)
    row = list(data[table_offset + level * members:table_offset + (level + 1) * members])
    if len(row) != members:
        raise SystemExit('truncated MIF clock row')
    expected = [pll_values[i][row[i]] for i in range(plls)] + row[plls:]
    expected = [value & mask for value, mask in zip(expected, masks)]
    supported.append(dict(level=level, rate_khz=rate, table_voltage_uv=voltage))
    if expected == actual:
        matches.append(supported[-1])
print(json.dumps(dict(matches=matches, supported=supported,
                     matched_all_seven_clocks=len(matches) == 1,
                     note='FV-map voltages are table values, not live voltage measurements'), indent=2))
if len(matches) != 1:
    raise SystemExit('clock snapshot does not uniquely match a firmware MIF level')
