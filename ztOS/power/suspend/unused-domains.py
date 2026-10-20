#!/usr/bin/env python3
"""Inspect or apply Samsung's boot-time shutdown of three unused domains.

The supplied stock DT disables DCF/DCPOST/DCRD; pmucal_lpm_init shuts them
down in this order. This is an explicit bring-up diagnostic, never autoloaded.
"""
import argparse
import json
import mmap
import os
from pathlib import Path
import struct
import time

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--apply', action='store_true')
p.add_argument('--restore', action='store_true')
p.add_argument('--state', type=Path, default=Path('/run/ztos-unused-domains.json'))
a = p.parse_args()
if a.apply and a.restore:
    p.error('choose apply or restore')
domains = [('DCRD', 0x4018), ('DCPOST', 0x4010), ('DCF', 0x4008)]
fd = os.open('/dev/mem', (os.O_RDWR if a.apply or a.restore else os.O_RDONLY) | os.O_SYNC)
with mmap.mmap(fd, 65536, flags=mmap.MAP_SHARED,
               prot=mmap.PROT_READ | (mmap.PROT_WRITE if a.apply or a.restore else 0),
               offset=0x14060000) as mem:
    def read(offset):
        return struct.unpack_from('<I', mem, offset)[0]

    def change(offset, value):
        struct.pack_into('<I', mem, offset, (read(offset) & ~15) | (value & 15))
        deadline = time.monotonic() + 0.1
        while read(offset + 4) & 15 != value & 15:
            if time.monotonic() >= deadline:
                raise RuntimeError(f'domain {offset:#x} transition timed out')
            time.sleep(0.001)

    initial = {name: read(offset) for name, offset in domains}
    if a.apply:
        if a.state.exists():
            raise SystemExit('saved state already exists; restore or inspect it first')
        # Match the memory policy applied by the audited backend first.
        for offset in (0x4364, 0x42c4, 0x4224):
            if read(offset) & 3 != 2:
                raise SystemExit('Samsung SRAM policy prerequisite is not initialized')
        a.state.write_text(json.dumps(initial))
        changed = []
        try:
            for name, offset in domains:
                changed.append((name, offset))
                change(offset, 0)
        except Exception:
            for name, offset in reversed(changed):
                change(offset, initial[name])
            a.state.unlink()
            raise
    elif a.restore:
        saved = json.loads(a.state.read_text())
        for name, offset in reversed(domains):
            change(offset, saved[name])
        a.state.unlink()
    print(json.dumps({name: dict(configuration=read(offset), status=read(offset + 4))
                      for name, offset in domains}, sort_keys=True))
os.close(fd)
