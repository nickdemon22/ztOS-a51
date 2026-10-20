#!/usr/bin/env python3
"""Extract Samsung's ACPM TMU plugin (exynos9810_acpm_tmu.fw) from the stock kernel.

ACPM's plugin 7 is marked stay-attached, but its code is not in the firmware:
Samsung's kernel loads it with request_firmware() at boot (acpm.c
plugins_init) and attaches it. Theirs is built in (CONFIG_EXTRA_FIRMWARE): a
struct builtin_fw {name, data, size} reached through R_AARCH64_RELATIVE
relocations of the uncompressed, relocatable kernel in stock/BOOT.img.

    extract-acpm-tmu.py stock/BOOT.img build/power-analysis/exynos9810_acpm_tmu.fw
"""
import struct
import sys

NAME = b'exynos9810_acpm_tmu.fw\0'
VBASE = 0xffffff8008080000      # KIMAGE_VADDR + TEXT_OFFSET of this kernel
RELATIVE = struct.pack('<Q', 0x403)

boot = open(sys.argv[1], 'rb').read()
size, page = struct.unpack_from('<I', boot, 8)[0], struct.unpack_from('<I', boot, 36)[0]
kernel = boot[page:page + size]
name = kernel.find(NAME)
if name < 0:
    raise SystemExit('no TMU plugin in this kernel')
relocs = {}
p = kernel.find(RELATIVE)
while p != -1:
    if p % 8 == 0 and p >= 8:
        relocs[struct.unpack_from('<Q', kernel, p - 8)[0]] = struct.unpack_from('<Q', kernel, p + 8)[0]
    p = kernel.find(RELATIVE, p + 1)
entry = next((where for where, value in relocs.items() if value == VBASE + name), None)
if entry is None or entry + 8 not in relocs:
    raise SystemExit('builtin_fw entry not found')
data = relocs[entry + 8] - VBASE
length = struct.unpack_from('<Q', kernel, entry + 16 - VBASE)[0]
fw = kernel[data:data + length]
if length != 0x1400:
    raise SystemExit(f'unexpected size {length:#x}: ACPM reserves 0x1400 for plugin 7')
open(sys.argv[2], 'wb').write(fw)
print(f'{sys.argv[2]}: {length} bytes')
