#!/usr/bin/env python3
"""poke.py BASE OFFSET VALUE: write one 32-bit register through /dev/mem."""
import mmap, os, struct, sys
base, off, val = int(sys.argv[1], 0), int(sys.argv[2], 0), int(sys.argv[3], 0)
fd = os.open('/dev/mem', os.O_RDWR | os.O_SYNC)
m = mmap.mmap(fd, 0x1000, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE, offset=(base + off) & ~0xfff)
struct.pack_into('<I', m, (base + off) & 0xfff, val)
print('%08x + %05x <- %08x, reads %08x' % (base, off, val, struct.unpack_from('<I', m, (base + off) & 0xfff)[0]))
