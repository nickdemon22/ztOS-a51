#!/usr/bin/env python3
"""Load and attach ACPM's TMU plugin (plugin 7), as Samsung's kernel does at boot.

ACPM marks plugin 7 stay-attached but ships without its code; the stock
kernel's plugins_init() copies exynos9810_acpm_tmu.fw (see
extract-acpm-tmu.py) to the plugin's base in ACPM SRAM and sends the
framework a dynamic-plugin attach. Ours never did, so plugin 7 stayed
detached. Nothing happens if it is attached already.

    load-acpm-tmu.py exynos9810_acpm_tmu.fw [s9p-acpm]

ACPM SRAM (0x02039000) takes aligned 32-bit accesses only.
"""
import mmap
import os
import struct
import subprocess
import sys

SRAM = 0x02039000
WINDOW = 0x7000                      # mapped from SRAM + 0x7000 (0x02040000)
INITDATA = 0x7f00                    # stock DT: acpm_ipc initdata-base
PLUGIN, PLUGIN_SIZE, PLUGIN_ENTRY = 7, 0x1400, 48
ATTACH = (1 << 25) | (PLUGIN << 26)  # ACPM_IPC_PROTOCOL_DP_A | id << ACPM_IPC_PROTOCOL_ID
FRAMEWORK_CHANNEL = 4                # stock DT: acpm acpm-ipc-channel

fw = open(sys.argv[1], 'rb').read()
tool = sys.argv[2] if len(sys.argv) > 2 else '/usr/local/libexec/ztos-power/s9p-acpm'
if len(fw) != PLUGIN_SIZE:
    raise SystemExit(f'expected {PLUGIN_SIZE} bytes, got {len(fw)}')
fd = os.open('/dev/mem', os.O_RDWR | os.O_SYNC)
m = mmap.mmap(fd, 0x10000, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE,
              offset=SRAM + WINDOW)


def word(offset):
    return struct.unpack_from('<I', m, offset - WINDOW)[0]


def plugin():
    entry = word(INITDATA) + PLUGIN * PLUGIN_ENTRY
    if not WINDOW <= entry < WINDOW + 0x10000 - PLUGIN_ENTRY:
        # the table sits below the window on this firmware: map it on its own
        page = (SRAM + entry) & ~0xfff
        t = mmap.mmap(fd, 0x1000, mmap.MAP_SHARED, mmap.PROT_READ, offset=page)
        words = [struct.unpack_from('<I', t, SRAM + entry - page + i)[0] for i in range(0, PLUGIN_ENTRY, 4)]
        t.close()
    else:
        words = [word(entry + i) for i in range(0, PLUGIN_ENTRY, 4)]
    raw = struct.pack('<12I', *words)
    pid, base = struct.unpack_from('<2I', raw)
    attached, size = raw[32], struct.unpack_from('<I', raw, 36)[0]
    return pid, base & ~1, attached, size


pid, base, attached, size = plugin()
if pid != PLUGIN or size != PLUGIN_SIZE or not WINDOW <= base <= WINDOW + 0x10000 - PLUGIN_SIZE:
    raise SystemExit(f'unexpected plugin entry: id={pid} base={base:#x} size={size:#x}')
if attached:
    print('ACPM plugin 7 (TMU) already attached')
    raise SystemExit(0)
if any(word(base + i) for i in range(0, PLUGIN_SIZE, 4)):
    raise SystemExit(f'plugin 7 slot at {base:#x} is not empty; not overwriting it')
for i in range(0, PLUGIN_SIZE, 4):
    struct.pack_into('<I', m, base - WINDOW + i, struct.unpack_from('<I', fw, i)[0])
if any(word(base + i) != struct.unpack_from('<I', fw, i)[0] for i in range(0, PLUGIN_SIZE, 4)):
    raise SystemExit('plugin 7 readback differs')
subprocess.run(['python3', tool, 'ipc', str(FRAMEWORK_CHANNEL), hex(ATTACH), '0', '0', '0'],
               check=True, timeout=5)
if not plugin()[2]:
    raise SystemExit('ACPM did not attach plugin 7')
print(f'ACPM plugin 7 (TMU) loaded at SRAM {base:#x} and attached')
