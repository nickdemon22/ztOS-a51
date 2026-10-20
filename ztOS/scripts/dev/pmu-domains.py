#!/usr/bin/env python3
"""pmu-domains.py: which SoC power domains are on (read-only; run as root on the phone).
Exynos 9810 PMU 0x14060000: per domain a CONFIGURATION word and the STATUS word after it
(low nibble 0xf: powered, 0: off). Offsets from Samsung's flexpmu_cal_local_exynos9810.h."""
import mmap, os, struct
DOMAINS = [("AUD (audio: ABOX)", 0x4000), ("DCF", 0x4008), ("DCPOST", 0x4010), ("DCRD", 0x4018),
           ("DPU (display)", 0x4020), ("DSPM", 0x4028), ("DSPS", 0x4030), ("G2D", 0x4038),
           ("G3D (GPU)", 0x4040), ("ISPPRE", 0x4048), ("ISPHQ", 0x4050), ("ISPLP", 0x4058),
           ("IVA", 0x4060), ("MFC (video codec)", 0x4068), ("VTS (voice trigger)", 0x4078), ("CHUB", 0x4080)]
fd = os.open("/dev/mem", os.O_RDONLY | os.O_SYNC)
m = mmap.mmap(fd, 0x5000, mmap.MAP_SHARED, mmap.PROT_READ, offset=0x14060000)
for name, off in DOMAINS:
    cfg, st = struct.unpack_from("<II", m, off)
    print(f"{name:22s} config {cfg & 0xf:#x}  status {st & 0xf:#x}  {'ON' if st & 0xf else 'off'}")
