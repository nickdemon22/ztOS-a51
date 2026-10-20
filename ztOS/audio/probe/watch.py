#!/usr/bin/env python3
"""watch.py BASE OFFSET COUNT INTERVAL_MS: sample one register quickly, with times."""
import mmap, os, struct, sys, time
base, off, n, iv = int(sys.argv[1], 0), int(sys.argv[2], 0), int(sys.argv[3]), float(sys.argv[4]) / 1000
fd = os.open('/dev/mem', os.O_RDONLY | os.O_SYNC)
m = mmap.mmap(fd, 0x1000, mmap.MAP_SHARED, mmap.PROT_READ, offset=(base + off) & ~0xfff)
t0 = time.monotonic(); out = []
for _ in range(n):
    out.append((time.monotonic() - t0, struct.unpack_from('<I', m, (base + off) & 0xfff)[0]))
    time.sleep(iv)
for t, v in out:
    print('%7.1f ms  %08x  off=%04x cnt=%04x' % (t * 1000, v, (v >> 16) & 0x1fff, v & 0x1fff))
