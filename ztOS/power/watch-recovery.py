#!/usr/bin/env python3
"""Capture the S9's previous kernel log in TWRP before another reboot replaces it."""
import argparse
from datetime import datetime, timezone
from pathlib import Path
import subprocess
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--seconds', type=int, default=180)
parser.add_argument('--reboot-after-capture', action='store_true')
parser.add_argument('--output', type=Path, default=Path('build/power-analysis'))
args = parser.parse_args()

def adb(*command, timeout=8):
    return subprocess.run(['adb', '-d', *command], capture_output=True, timeout=timeout)

deadline = time.monotonic() + args.seconds
print('Waiting for this Galaxy S9 in TWRP...', flush=True)
while time.monotonic() < deadline:
    try:
        recovery = adb('shell', 'getprop', 'ro.twrp.version', timeout=2)
        if recovery.returncode or not recovery.stdout.strip():
            time.sleep(2)
            continue
        model = adb('shell', 'getprop', 'ro.product.model').stdout.decode().strip()
        device = adb('shell', 'getprop', 'ro.product.device').stdout.decode().strip()
        if model != 'SM-G960N' and not device.startswith('starlte'):
            print('Recovery device does not match this project; leaving it untouched.', flush=True)
            raise SystemExit(2)
        target = args.output / datetime.now(timezone.utc).strftime('recovery-%Y%m%dT%H%M%SZ')
        target.mkdir(parents=True, exist_ok=True)
        # Recovery may omit the device node even when its kernel supports
        # /dev/mem. This only creates the conventional node in recovery's /dev.
        adb('shell', 'test -c /dev/mem || mknod /dev/mem c 1 1')
        captures = {
            'pmsg-ramoops-0': ['exec-out', 'cat', '/sys/fs/pstore/pmsg-ramoops-0'],
            'console-ramoops-0': ['exec-out', 'cat', '/sys/fs/pstore/console-ramoops-0'],
            'dmesg-ramoops-0': ['exec-out', 'cat', '/sys/fs/pstore/dmesg-ramoops-0'],
            'recovery-dmesg.txt': ['exec-out', 'dmesg'],
            'acpm-sram.bin': ['exec-out', 'dd', 'if=/dev/mem', 'bs=4096',
                              'skip=8256', 'count=16'],
        }
        for name, command in captures.items():
            result = adb(*command)
            valid = result.returncode == 0
            if name == 'acpm-sram.bin':
                valid = valid and len(result.stdout) == 65536
            if valid:
                (target / name).write_bytes(result.stdout)
                print(f'Captured {name}: {len(result.stdout)} bytes', flush=True)
            else:
                (target / (name + '.error')).write_bytes(
                    result.stderr + (result.stdout if name == 'acpm-sram.bin' else b''))
                print(f'Capture failed: {name}', flush=True)
        print(f'Evidence saved in {target}', flush=True)
        if args.reboot_after_capture:
            result = adb('reboot')
            if result.returncode:
                raise SystemExit(result.stderr.decode(errors='replace'))
            print('Hot reboot requested; waiting for ztOS USB next.', flush=True)
        break
    except subprocess.TimeoutExpired:
        time.sleep(2)
else:
    raise SystemExit('No matching TWRP connection before timeout.')
