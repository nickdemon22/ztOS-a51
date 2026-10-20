#!/usr/bin/env python3
"""Read Samsung FLEXPMU debug counters; never modifies firmware SRAM.

Layout: vendor drivers/soc/samsung/exynos-flexpmu-dbg.c; physical address
and size from the G960NKSU1CSE3 firmware DT (all six revisions).
"""
import json
import mmap
import os
import struct

BASE = 0x0204F800
LINES = {0: 'cpu_status', 1: 'seq_status', 10: 'sw_flag',
         16: 'soc_count', 17: 'mif_count', 21: 'mif_always_on',
         22: 'sleep_ap', 23: 'sleep_mif', 24: 'sicd_ap', 25: 'sicd_mif',
         26: 'current_power_mode'}
fd = os.open('/dev/mem', os.O_RDONLY | os.O_SYNC)
try:
    with mmap.mmap(fd, 4096, flags=mmap.MAP_SHARED, prot=mmap.PROT_READ,
                   offset=BASE & ~4095) as mem:
        values = {name: list(struct.unpack_from('<II', mem,
                  (BASE & 4095) + line * 16 + 8))
                  for line, name in LINES.items()}
        # Live ACPM table runner (image 0x81d0) publishes these fields.
        # They are retained diagnostics, not proof of a currently running step.
        values['sequencer_trace'] = {
            name: struct.unpack_from('<I', mem, (BASE & 4095) + offset)[0]
            for name, offset in [('marker', 0x2c), ('operation', 0x38), ('index', 0x3c)]}
        values['sequencer_trace']['end_sentinel'] = (
            values['sequencer_trace']['index'] == 0x21444e45)  # ASCII END!
finally:
    os.close(fd)
values['mif_requests'] = (values['sw_flag'][1] >> 16) & 255
values['mif_sequence_enabled'] = values['mif_always_on'][1]
values['mif_request_names'] = [name for bit, name in enumerate(['CP', 'AUD', 'GNSS', 'VTS', 'AP'])
                               if values['mif_requests'] & (1 << bit)]
values['counter_fields'] = ['early_wakeup', 'sequence_down']
print(json.dumps(values, sort_keys=True))
