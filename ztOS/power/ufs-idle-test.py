#!/usr/bin/env python3
"""Exercise UFS idle/resume with verified file I/O; never touches raw disks."""
import hashlib
import os
from pathlib import Path
import tempfile
import time

control = Path("/sys/bus/platform/devices/11120000.ufs/clkgate_enable")
if control.read_text().strip() != "1":
    raise SystemExit("UFS clock gating must be enabled for this test")
delay = Path("/sys/bus/platform/devices/11120000.ufs/clkgate_delay_ms")
idle_seconds = max(2, int(delay.read_text().strip()) / 1000 + 1)
payload = os.urandom(4 * 1024 * 1024)
expected = hashlib.sha256(payload).hexdigest()
with tempfile.TemporaryFile(dir="/var/tmp") as file:
    for attempt in range(8):
        file.seek(0)
        file.write(payload)
        file.flush()
        os.fsync(file.fileno())
        os.posix_fadvise(file.fileno(), 0, len(payload), os.POSIX_FADV_DONTNEED)
        time.sleep(idle_seconds)
        file.seek(0)
        actual = hashlib.sha256(file.read(len(payload))).hexdigest()
        if actual != expected:
            raise SystemExit(f"data mismatch after idle on round {attempt + 1}")
        print(f"round {attempt + 1}: fsync, evict cache, idle, read verified", flush=True)
print("Eight UFS idle/I/O rounds passed; check kernel logs for recovery/errors")
