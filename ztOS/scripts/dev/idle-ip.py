#!/usr/bin/env python3
"""idle-ip.py: which blocks the PMU thinks are NOT idle (run as root on the phone, read-only).
Samsung's SICD (system idle) needs every unmasked IDLE_IP bit to be 1 (idle). The registers:
PMU 0x14060000 + 0x3E0.. (status, 4 x 32 bits) and + 0x3F0.. (mask: 1 = the PMU ignores that bit).
Names: index into the stock device tree's exynos-powermode idle-ip list (idle-ip-names.txt)."""
import mmap, os, struct, sys
names = {}
for line in open(sys.argv[1] if len(sys.argv) > 1 else "/tmp/zrn/idle-ip-names.txt"):
    i, n = line.split(None, 1); names[int(i)] = n.strip()
fd = os.open("/dev/mem", os.O_RDONLY | os.O_SYNC)
m = mmap.mmap(fd, 0x1000, mmap.MAP_SHARED, mmap.PROT_READ, offset=0x14060000)
stat = struct.unpack_from("<4I", m, 0x3E0); mask = struct.unpack_from("<4I", m, 0x3F0)
print("status", [f"{v:#010x}" for v in stat]); print("mask  ", [f"{v:#010x}" for v in mask])
busy = []
for r in range(4):
    for b in range(32):
        idx = r * 32 + b
        if not (stat[r] >> b) & 1:
            busy.append((idx, "masked" if (mask[r] >> b) & 1 else "COUNTS", names.get(idx, "(hardware / unnamed)")))
print(len(busy), "bits at 0 (non-idle):")
for idx, how, name in busy: print(f"  {idx:3d} {idx:#04x}  {how:6s}  {name}")
