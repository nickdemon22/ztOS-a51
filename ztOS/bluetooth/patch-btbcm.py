#!/usr/bin/env python3
"""Patch btbcm.c and hci_bcm.c so the Galaxy S9's BCM4361 comes up, fast.

  patch-btbcm.py <linux source tree>

Its firmware answers LE Read Transmit Power (0x204b) with one byte, and the
host then fails setup with -38 ("unexpected cc 0x204b length: 1 < 3").
btbcm already knows such controllers, but finds them through DMI tables,
which an arm64 phone does not have: the quirk is set for this board too, by
its device-tree compatible. 
hci_bcm.c: attached with btattach (no serdev, no device), the driver has no
operational speed and stays at 115200 baud, too slow for A2DP (SBC wants
~350 kbit/s, LDAC ~1 Mbit/s). This board gets 3 Mbaud, Samsung's own
setting; from the UART's 200 MHz clock that is within 0.5 %.

Asserted, so a changed upstream file fails instead of silently building
without it.
"""
import sys
from pathlib import Path

path = Path(sys.argv[1]) / "drivers/bluetooth/btbcm.c"
src = path.read_text()
old = "\tif (dmi_first_match(disable_broken_read_transmit_power))\n"
new = ("\tif (dmi_first_match(disable_broken_read_transmit_power) ||\n"
       "\t    of_machine_is_compatible(\"samsung,starlte\"))\n")
if new not in src:
    assert src.count(old) == 1, "btbcm.c changed: look at the quirk again"
    src = src.replace(old, new)
    if "#include <linux/of.h>" not in src:
        src = src.replace("#include <linux/module.h>\n", "#include <linux/module.h>\n#include <linux/of.h>\n", 1)
    path.write_text(src)

path = Path(sys.argv[1]) / "drivers/bluetooth/hci_bcm.c"
src = path.read_text()
old = ("\telse if (hu->proto->oper_speed)\n"
       "\t\tspeed = hu->proto->oper_speed;\n"
       "\telse\n"
       "\t\tspeed = 0;\n\n"
       "\tif (speed) {\n"
       "\t\terr = bcm_set_baudrate(hu, speed);\n")
new = ("\telse if (hu->proto->oper_speed)\n"
       "\t\tspeed = hu->proto->oper_speed;\n"
       "\telse if (of_machine_is_compatible(\"samsung,starlte\"))\n"
       "\t\tspeed = 3000000;\t/* attached by btattach: Samsung's speed */\n"
       "\telse\n"
       "\t\tspeed = 0;\n\n"
       "\tif (speed) {\n"
       "\t\terr = bcm_set_baudrate(hu, speed);\n")
if new not in src:
    assert src.count(old) == 1, "hci_bcm.c changed: look at the speed again"
    src = src.replace(old, new)
    if "#include <linux/of.h>" not in src:
        src = src.replace("#include <linux/module.h>\n", "#include <linux/module.h>\n#include <linux/of.h>\n", 1)
    path.write_text(src)
