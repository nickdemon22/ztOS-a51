#!/usr/bin/env python3
"""pd-ab.py: what each SoC power domain costs. Run as root on the phone.

For each domain given (cumulative), power it off through the PMU (the same two
registers Samsung's kernel uses: CONFIGURATION, then wait for STATUS) and read
the battery current. With the cable in, the charger's input is constant, so a
rise in the battery's current is the same fall in what the phone uses.

    pd-ab.py [--keep] NAME...     (names as in pmu-domains.py; --keep leaves them off)

Each step is written to /var/tmp/pd-ab.log (flushed) *before* it is done, so a
freeze shows which domain caused it."""
import mmap, os, struct, sys, time
D = {"AUD": 0x4000, "DCF": 0x4008, "DCPOST": 0x4010, "DCRD": 0x4018, "DPU": 0x4020, "DSPM": 0x4028,
     "DSPS": 0x4030, "G2D": 0x4038, "G3D": 0x4040, "ISPPRE": 0x4048, "ISPHQ": 0x4050, "ISPLP": 0x4058,
     "IVA": 0x4060, "MFC": 0x4068, "VTS": 0x4078, "CHUB": 0x4080}
BAT = "/sys/class/power_supply/max170xx_battery/"
keep = "--keep" in sys.argv
names = [a for a in sys.argv[1:] if not a.startswith("--")]
log = open("/var/tmp/pd-ab.log", "a", buffering=1)
def say(s):
    print(s, flush=True); log.write(s + "\n"); log.flush(); os.fsync(log.fileno())
fd = os.open("/dev/mem", os.O_RDWR | os.O_SYNC)
m = mmap.mmap(fd, 0x5000, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE, offset=0x14060000)
def cur():
    n = 8; s = 0
    for _ in range(n):
        s += int(open(BAT + "current_now").read()); time.sleep(0.5)
    return s / n / 1000.0
def status(off): return struct.unpack_from("<I", m, off + 4)[0] & 0xf
def config(off, v):
    old = struct.unpack_from("<I", m, off)[0]
    struct.pack_into("<I", m, off, (old & ~0xf) | v)
say(f"== pd-ab {time.strftime('%H:%M:%S')}  battery {open(BAT+'capacity').read().strip()} %")
base = cur(); say(f"{'baseline':10s} {base:8.1f} mA (battery current; + = charging)")
prev = base
turned = []
for n in names:
    off = D[n]
    if status(off) == 0:
        say(f"{n:10s} already off"); continue
    say(f"{n:10s} powering off ...")
    config(off, 0)
    for _ in range(200):
        if status(off) == 0: break
        time.sleep(0.001)
    if status(off) != 0:
        say(f"{n:10s} STAYED ON (busy): config put back"); config(off, 0xf); continue
    turned.append(n)
    time.sleep(2)
    c = cur()
    say(f"{n:10s} off: {c:8.1f} mA   step {c - prev:+7.1f}   total {c - base:+7.1f} (less phone load)")
    prev = c
if not keep:
    for n in reversed(turned):
        config(D[n], 0xf)
        for _ in range(300):
            if status(D[n]) == 0xf: break
            time.sleep(0.001)
        say(f"{n:10s} back on (status {status(D[n]):#x})")
say("done")
