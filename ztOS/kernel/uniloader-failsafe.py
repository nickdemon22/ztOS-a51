#!/usr/bin/env python3
"""Arm the way back in uniLoader's first board hook.

  uniloader-failsafe.py <board-starlte.c> <INFORM3 value>

The port's kernel arms Samsung's reboot magic and the cluster-1 watchdog in a
postcore initcall, about 0.3 s into the boot. Everything before that (the
kernel copy, the early page tables, the first console) is unprotected: a hang
there leaves the phone on the logo until someone holds the buttons. These are
the same register writes, moved to the first C code that runs after sboot.

  PMU INFORM2 (0x14060808) = 0x12345678   "this was a reset", not a cold boot
  PMU INFORM3 (0x1406080c) = <value>      where sboot goes next
  PMU 0x408 / 0x40c bit 23 cleared        let WDT1 expiry reset the SoC
  WDT1 (0x10060000)                       30 s: 26 MHz / 256 / 128 = 793 Hz
"""
import sys
from pathlib import Path

path, inform3 = Path(sys.argv[1]), int(sys.argv[2], 0)
s = path.read_text()
anchor = """	/* Allow framebuffer to be written to */
	*(int*) (DECON_F_BASE + HW_SW_TRIG_CONTROL) = 0x1281;
"""
if s.count(anchor) != 1:
    sys.exit("uniloader-failsafe: anchor not found in board-starlte.c")
if "zboot failsafe" in s:
    sys.exit("uniloader-failsafe: already applied")
s = s.replace(anchor, f"""	/* zboot failsafe: fallback boot target, then the watchdog */
	*(volatile unsigned int *)0x14060808 = 0x12345678;
	*(volatile unsigned int *)0x1406080c = {inform3:#x};
	*(volatile unsigned int *)0x14060408 &= ~(1u << 23);
	*(volatile unsigned int *)0x1406040c &= ~(1u << 23);
	*(volatile unsigned int *)0x10060000 = 0;
	*(volatile unsigned int *)0x10060004 = 23805;
	*(volatile unsigned int *)0x10060008 = 23805;
	*(volatile unsigned int *)0x10060000 = 0xff39;

""" + anchor)
path.write_text(s)
print(f"uniloader-failsafe: INFORM3={inform3:#x}, WDT1 armed for 30 s")
