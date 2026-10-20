#!/usr/bin/env python3
"""Initialize only an invalid Exynos RTC calendar from the current UTC clock."""
from datetime import datetime, timezone
import json
import mmap
import os
import struct

offsets = [0x70, 0x74, 0x78, 0x7c, 0x84, 0x88]
now = datetime.now(timezone.utc)
if not 2000 <= now.year <= 2099:
    raise SystemExit('System clock must be corrected before initializing RTC')
fd = os.open('/dev/mem', os.O_RDWR | os.O_SYNC)
try:
    with mmap.mmap(fd, 4096, flags=mmap.MAP_SHARED,
                   prot=mmap.PROT_READ | mmap.PROT_WRITE,
                   offset=0x141e0000) as mem:
        old = [mem[x] for x in offsets]
        valid = all((x & 15) < 10 and (x >> 4) < 10 for x in old)
        if valid:
            decoded = [(x >> 4) * 10 + (x & 15) for x in old]
            try:
                datetime(decoded[5] + 2000, decoded[4], decoded[3],
                         decoded[2], decoded[1], decoded[0])
            except ValueError:
                valid = False
        if valid:
            print(json.dumps({'initialized': False, 'reason': 'calendar already valid'}))
        else:
            if mem[0x50] & 64:
                raise SystemExit('Refusing calendar change while an alarm is enabled')
            control = struct.unpack_from('<H', mem, 0x40)[0]
            values = [now.second, now.minute, now.hour, now.day, now.month, now.year - 2000]
            try:
                struct.pack_into('<H', mem, 0x40, control | 1)
                # Seconds last so the newly written date starts coherently.
                for i in [5, 4, 3, 2, 1, 0]:
                    mem[offsets[i]] = (values[i] // 10) * 16 + values[i] % 10
            finally:
                struct.pack_into('<H', mem, 0x40, control)
            print(json.dumps({'initialized': True, 'old_bcd': old,
                              'utc': now.isoformat()}))
finally:
    os.close(fd)
