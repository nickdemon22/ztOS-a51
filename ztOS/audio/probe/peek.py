#!/usr/bin/env python3
"""peek BASE OFFSET [OFFSET...]: read 32-bit registers through /dev/mem.

For looking at the audio block before there is a driver. Reading a block whose
power domain is off or whose bus clock is gated stops the kernel, so check the
PMU status first (0x14060000 + 0x4004 must read xxxxxxxF for audio)."""
import mmap, os, struct, sys
base = int(sys.argv[1], 0)
fd = os.open('/dev/mem', os.O_RDONLY | os.O_SYNC)
page = base & ~0xfff
for arg in sys.argv[2:]:
    off = int(arg, 0)
    start = (base + off) & ~0xfff
    m = mmap.mmap(fd, 0x1000, mmap.MAP_SHARED, mmap.PROT_READ, offset=start)
    print('%08x + %05x = %08x' % (base, off, struct.unpack_from('<I', m, (base + off) & 0xfff)[0]), flush=True)
    m.close()
