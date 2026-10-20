#!/usr/bin/env python3
"""Read-only comparison with Samsung's PMUCAL initialization prerequisites."""
import argparse
import json
import mmap
import os
from pathlib import Path
import struct

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--manifest', type=Path,
                    default=Path(__file__).with_name('init-prerequisites.json'))
parser.add_argument('--with-memory-clocks', action='store_true',
                    help='also inspect MIF CMU handshake and bus DRCG registers while RAM is awake')
parser.add_argument('--with-core-clocks', action='store_true',
                    help='also inspect core/CPU clock blocks while both CPU clusters are online')
args = parser.parse_args()
manifest = json.loads(args.manifest.read_text())
# PMU registers remain accessible independently of domain power. Never probe
# clock or peripheral registers in powered-off GPU/camera/media domains.
allowed = {0x14060000}
if args.with_memory_clocks:
    allowed |= {0x1b800000, 0x1b900000, 0x1ba00000, 0x1bb00000,
                0x1b810000, 0x1b910000, 0x1ba10000, 0x1bb10000}
if args.with_core_clocks:
    allowed |= {0x1a240000, 0x1a020000, 0x1d000000, 0x1d100000, 0x10020000}
report = dict(source_sha256=manifest['source_sha256'], table=manifest['table'],
              compared=[], mismatches=[], skipped=[], unverified=[])
fd = os.open('/dev/mem', os.O_RDONLY | os.O_SYNC)
regions = {}
try:
    for entry in manifest['entries']:
        base = entry['base']
        if base not in allowed:
            report['skipped'].append(entry)
            continue
        if base & 0xffff or not 0 <= entry['offset'] < 65533:
            raise ValueError('invalid register range in manifest')
        if base not in regions:
            regions[base] = mmap.mmap(fd, 65536, flags=mmap.MAP_SHARED,
                                     prot=mmap.PROT_READ, offset=base)
        actual = struct.unpack_from('<I', regions[base], entry['offset'])[0]
        # Vendor RMW ORs value after clearing mask, including any extra set
        # bits present in value. Audit both sets of required bits.
        required_mask = entry['mask'] | entry['value']
        if base in {0x1b810000, 0x1b910000, 0x1ba10000, 0x1bb10000} and entry['offset'] == 0x104:
            # Live S9 implements the set bits in 0x80000e00; the vendor
            # explicitly requires bit12 to remain clear. Other bits are reserved.
            required_mask = 0x80001e00
        matches = (actual & required_mask) == (entry['value'] & required_mask)
        if entry['name'] in {'CPUCL0_SHORTSTOP', 'CPUCL1_SHORTSTOP'}:
            report['unverified'].append({**entry, 'actual': actual,
                                         'reason': 'reserved-bit readback mask unknown'})
            continue
        result = {**entry, 'actual': actual, 'matches': matches}
        report['compared'].append(result)
        if not matches:
            report['mismatches'].append(result)
finally:
    for region in regions.values():
        region.close()
    os.close(fd)
report['counts'] = {key: len(report[key]) for key in ('compared', 'mismatches', 'skipped', 'unverified')}
print(json.dumps(report, indent=2))
