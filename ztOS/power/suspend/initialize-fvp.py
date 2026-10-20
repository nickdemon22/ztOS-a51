#!/usr/bin/env python3
"""Initialize the audited Samsung FVP constraint table, explicitly opt-in.

Defaults to inspection. Requires guarded MIF sequencing disabled and dry-run
platform entry. Sends vendor IPC messages; never directly edits policy SRAM.
"""
import argparse
import json
import mmap
import os
from pathlib import Path
import runpy
import struct

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('manifest', type=Path)
p.add_argument('--transport', type=Path, required=True)
p.add_argument('--apply', action='store_true')
a = p.parse_args()
manifest = json.loads(a.manifest.read_text())
expected = [(1794000, 400000), (1539000, 336000), (1352000, 336000),
            (1014000, 107000), (845000, 107000), (676000, 107000),
            (546000, 107000), (421000, 107000)]
messages = [[i, mif, 5, limit] for i, (mif, limit) in enumerate(expected)]
messages.append([8, 1794000, 5, 1])
if manifest.get('channel') != 1 or manifest.get('messages') != messages:
    raise SystemExit('manifest differs from the audited firmware policy')
if manifest.get('sboot_sha256') != '022de35476d5c938c228ea2cc03b31f963071e12778ead0cb589021c130ad59c':
    raise SystemExit('unsupported calibration source')
fd = os.open('/dev/mem', os.O_RDONLY | os.O_SYNC)
with mmap.mmap(fd, 8192, flags=mmap.MAP_SHARED, prot=mmap.PROT_READ,
               offset=0x02045000) as mem:
    def read(logical):
        return struct.unpack_from('<I', mem, logical - 0xc000)[0]
    if read(0xc948) != 0xca64 or read(0xc95c) != 0xca60:
        raise SystemExit('unsupported firmware layout')
    def state():
        return dict(ap_running=read(0xca60), initialized=read(0xcbb4),
                    count=read(0xcbb8),
                    mif=[read(0xca78 + i * 4) for i in range(8)],
                    int_min=[read(0xcab4 + i * 4) for i in range(8)])
    before = state()
    print(json.dumps(dict(phase='before', state=before)), flush=True)
    if a.apply:
        module = Path('/sys/module/zrn_sleep_init/parameters')
        if not module.is_dir() or (module / 'dry_run').read_text().strip() not in ('Y', '1'):
            raise SystemExit('requires guarded backend with dry_run=1')
        with mmap.mmap(fd, 4096, flags=mmap.MAP_SHARED, prot=mmap.PROT_READ,
                       offset=0x0204f000) as debug:
            if struct.unpack_from('<I', debug, 0x95c)[0] != 0:
                raise SystemExit('MIF sequencing must remain disabled during initialization')
        if before['initialized'] or before['count']:
            raise SystemExit('policy already initialized; leaving it unchanged')
        if before['ap_running'] != 1:
            raise SystemExit('AP must be marked running')
        transport = runpy.run_path(str(a.transport), run_name='fvp_transport')
        channel = transport['find_channel'](1)
        for i, command in enumerate(messages):
            sequence = transport['send'](channel, command)
            reply = transport['receive'](channel, sequence)
            print(json.dumps(dict(phase='message', command=command, reply=reply)), flush=True)
            if reply[0] & 0xffff:
                raise SystemExit('firmware rejected FVP initialization')
            if i < 8:
                if read(0xca78 + i * 4) != command[1] or read(0xcab4 + i * 4) != command[3]:
                    raise SystemExit('table readback mismatch before commit; no commit sent')
        after = state()
        if after['initialized'] != 1 or after['count'] != 8:
            raise SystemExit('FVP initialization commit did not verify')
        if after['mif'] != [x[0] for x in expected] or after['int_min'] != [x[1] for x in expected]:
            raise SystemExit('committed FVP table does not match calibration')
        print(json.dumps(dict(phase='verified', state=after)), flush=True)
os.close(fd)
