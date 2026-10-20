#!/usr/bin/env python3
"""linewatch.py SECONDS: print the levels of every pin around the sensor hub
whenever any of them changes (read straight from the GPIO data registers)."""
import mmap, os, struct, sys, time
fd = os.open('/dev/mem', os.O_RDONLY | os.O_SYNC)
def reg(addr):
    m = mmap.mmap(fd, 0x1000, mmap.MAP_SHARED, mmap.PROT_READ, offset=addr & ~0xfff)
    return m, addr & 0xfff
banks = {'gph0': reg(0x13A80004), 'gpa0': reg(0x14050024), 'gpa1': reg(0x14050044), 'gpa4': reg(0x140500c4), 'gpg0': reg(0x10430084)}
names = {('gph0', 0): 'mosi', ('gph0', 1): 'sck', ('gph0', 2): 'miso', ('gph0', 3): 'cs', ('gph0', 4): 'mcu_resp',
         ('gph0', 5): 'mcu_req', ('gph0', 6): 'h6', ('gph0', 7): 'h7', ('gpa1', 7): 'host_req', ('gpa4', 0): 'shub_int',
         ('gpa0', 5): 'ap_sleep', ('gpg0', 2): 'gps_pwr'}
def sample():
    out = {}
    for (bank, pin), name in names.items():
        m, off = banks[bank]
        out[name] = (struct.unpack_from('<I', m, off)[0] >> pin) & 1
    return out
t0 = time.monotonic(); last = None; skip = ('mosi', 'sck', 'cs')
while time.monotonic() - t0 < float(sys.argv[1]):
    s = sample(); key = {k: v for k, v in s.items() if k not in skip}
    if key != last:
        print('%7.1f ms  ' % ((time.monotonic() - t0) * 1000) + ' '.join('%s=%d' % kv for kv in s.items()), flush=True)
        last = key
