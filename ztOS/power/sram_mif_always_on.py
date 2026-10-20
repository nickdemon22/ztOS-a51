#!/usr/bin/env python3
"""Legacy debug-file name: 0 disables MIF sequencing, 1 permits it.
Use the gated platform backend for suspend tests; global enable can race CPU offlining.
"""
import sys
import mmap
import os
import struct

BASE = 0x0204F800
# line 21, second 32-bit word: 0x800 + 21*16 + 8 + 4 = 0x95c
offset = (BASE & 4095) + 21 * 16 + 12

fd = os.open('/dev/mem', (os.O_RDWR if len(sys.argv) > 1 else os.O_RDONLY) | os.O_SYNC)
try:
    with mmap.mmap(fd, 4096, flags=mmap.MAP_SHARED,
                   prot=(mmap.PROT_READ | mmap.PROT_WRITE) if len(sys.argv) > 1 else mmap.PROT_READ,
                   offset=BASE & ~4095) as mem:
        if len(sys.argv) > 1:
            val = int(sys.argv[1])
            if val not in (0, 1):
                raise SystemExit('MIF sequencing enable must be 0 or 1')
            struct.pack_into('<I', mem, offset, val)
            print(f'Set 0x0204F95C = {val}')
        cur = struct.unpack_from('<I', mem, offset)[0]
        print(f'Read 0x0204F95C = {cur}')
finally:
    os.close(fd)
