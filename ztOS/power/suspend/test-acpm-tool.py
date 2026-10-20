#!/usr/bin/env python3
"""Exercise diagnostic dispatch without mapping hardware or touching rails."""
import ast
from pathlib import Path
from types import SimpleNamespace

path = Path(__file__).resolve().parents[2] / 'third_party/s9plus/extras/debug-tools/files/usr/local/sbin/s9p-acpm'
tree = ast.parse(path.read_text())
main = next(n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name == 'main')
calls = []
ns = dict(find_channel=lambda n: SimpleNamespace(id=n),
          send=lambda ch, cmd: calls.append(('send', ch.id, cmd)) or 7,
          receive=lambda ch, seq: [0, 421000, 0, 0],
          mfd=lambda *args: (_ for _ in ()).throw(AssertionError('unexpected PMIC access')))
exec(compile(ast.Module(body=[main], type_ignores=[]), str(path), 'exec'), ns)
ns['main'](['dvfs', 'mif'])
assert calls == [('send', 0, [0, 0, 1, 0])], calls
print('PASS: MIF frequency query uses request channel 0 and never accesses touch rails')
calls.clear()
ns['main'](['dvfs', 'mif', '421000'])
assert calls == [('send', 0, [0, 421000, 0, 0])], calls
ns['receive'] = lambda ch, seq: [1, 421000, 0, 0]
try:
    ns['main'](['dvfs', 'mif', '421000'])
except SystemExit as error:
    assert 'rejected' in str(error)
else:
    raise AssertionError('firmware rejection was ignored')
print('PASS: MIF frequency writes never access touch rails and propagate firmware rejection')
ns['receive'] = lambda ch, seq: [0, 0, 0, 0]
try:
    ns['main'](['dvfs', 'mif'])
except SystemExit as error:
    assert 'returned no frequency' in str(error)
else:
    raise AssertionError('zero firmware result was reported as a real frequency')
info = next(n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name == 'cmd_info')
ns.update(fw=SimpleNamespace(num_plugins=8, ipc_channels=0x5bfc,
                            num_ipc_channels=18, ipc_ap_max=13, fw_size=0x4f00),
          channels=[SimpleNamespace(id=5, type=2, ap_poll=0, q_len=1,
                                    q_elem_size=16, ap_tx=(0, 0, 0x83f0),
                                    ap_rx=(0, 0, 0x83d0))],
          sram=object(), mbox=object(), INTSR0=0x14, INTMR0=0x10,
          INTSR1=0x28, INTMR1=0x24)
def read_word(mapping, offset):
    assert offset != 0, 'buffer channel has no queue-pointer registers'
    return 0
ns['r32'] = read_word
exec(compile(ast.Module(body=[info], type_ignores=[]), str(path), 'exec'), ns)
ns['cmd_info']()
print('PASS: buffer-channel inspection never dereferences absent queue pointers')
