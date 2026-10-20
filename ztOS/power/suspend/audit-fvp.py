#!/usr/bin/env python3
"""Read the audited Exynos9810 FVP policy state; never sends IPC or writes SRAM.

Offsets follow the resident firmware IPC handler at logical 0xc5d0:
DATA_INIT/SET_CONST writes 0xcbb4/0xcbb8; RELEASE writes 0xca60.
Each 0x8c-byte policy has five scalar fields and two 15-word tables.
"""
import json
import mmap
import os
from pathlib import Path
import struct

compatible = Path('/proc/device-tree/compatible').read_bytes().split(b'\0')
if b'samsung,exynos9810' not in compatible:
    raise SystemExit('this audit is specific to Exynos9810')
fd = os.open('/dev/mem', os.O_RDONLY | os.O_SYNC)
with mmap.mmap(fd, 8192, flags=mmap.MAP_SHARED, prot=mmap.PROT_READ,
               offset=0x02045000) as mem:
    def words(logical, count=1):
        return list(struct.unpack_from('<' + 'I' * count, mem, logical - 0xc000))
    # Check the firmware jump-table and state pointers before interpreting data.
    if words(0xc948)[0] != 0xca64 or words(0xc95c)[0] != 0xca60:
        raise SystemExit('unsupported FVP firmware layout')
    count = words(0xcbb8)[0]
    report = dict(ap_running=words(0xca60)[0],
                  constraints_initialized=words(0xcbb4)[0], level_count=count,
                  policies={})
    if count > 15:
        raise SystemExit('invalid FVP level count')
    for domain, base in [('mif', 0xca64), ('int', 0xcaf0)]:
        values = words(base, 35)
        report['policies'][domain] = dict(
            raw_scalars=values[:5],
            frequency_table_khz=values[5:5 + count],
            constraint_table_khz=values[20:20 + count])
print(json.dumps(report, indent=2))
os.close(fd)
