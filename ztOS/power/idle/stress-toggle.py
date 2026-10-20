#!/usr/bin/env python3
"""Regression test: C2 must keep advancing after repeated state toggles.

Run as root on the phone. Restores every state flag even on failure. This
reproduces the lost MCT deadline that stranded CPUs in broadcast polling.
"""
from pathlib import Path
import time

paths = list(Path("/sys/devices/system/cpu").glob("cpu[0-9]*/cpuidle/state1/disable"))
saved = {p: p.read_text().strip() for p in paths}
active = [p for p in paths if saved[p] == "0"]
if not active:
    raise SystemExit("no enabled C2 states")


def counters():
    return {str(p.parent): int((p.parent / "usage").read_text()) for p in active}


try:
    for _ in range(100):
        for p in active:
            p.write_text("1")
        time.sleep(0.01)
        for p in active:
            p.write_text("0")
        time.sleep(0.05)
    before = counters()
    time.sleep(3)
    after = counters()
    stuck = []
    for name in before:
        delta = after[name] - before[name]
        print(f"{name}: {delta} C2 transitions after stress", flush=True)
        if delta <= 0:
            stuck.append(name)
    if stuck:
        raise SystemExit("C2 stopped advancing: " + ", ".join(stuck))
    print("100 C2 disable/enable rounds passed")
finally:
    for p, value in saved.items():
        p.write_text(value)
