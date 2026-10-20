#!/usr/bin/env python3
# decon-dsc.py: read (only) what DECON0's two DSC encoders are programmed with, to compare
# with the PPS the panel is given. Run as root.
import mmap
import os
import struct

fd = os.open("/dev/mem", os.O_RDONLY | os.O_SYNC)
m = mmap.mmap(fd, 0x10000, mmap.MAP_SHARED, mmap.PROT_READ, offset=0x16030000)
for base in (0x4000, 0x5000):
    print("DSC at +%#x" % base)
    for o in range(0, 0x90, 16):
        print("  %04x: %s" % (base + o, " ".join("%08x" % struct.unpack_from("<I", m, base + o + 4 * i)[0]
                                                  for i in range(4))))
