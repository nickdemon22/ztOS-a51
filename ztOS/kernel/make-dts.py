#!/usr/bin/env python3
"""Derive the Galaxy S9 (starlte, SM-G960N) device tree from the S9+ port's.

  make-dts.py <exynos9810-star2lte.dts> <out.dts> [--ufs] [--power] [--touch]
              [--gpu] [--decon] [--wlan] [--fallback boot|recovery]

The two phones share the board design; what differs is listed here, and every
edit is asserted, so a changed upstream file fails the build instead of
silently producing an S9+ tree.

  memory     4 GB: the bank at 0x9_00000000 does not exist (bootloader FDT)
  battery    3000 mAh, 4.35 V float (stock battery,chg_float_voltage = 4350)
  fallback   which INFORM3 magic the kernel arms first thing. "boot" means a
             watchdog reset or panic comes up from BOOT; "recovery" from
             RECOVERY. Point it at whichever one holds TWRP.

Blocks are off unless asked for, because a read into a gated or unpowered
block on this SoC is a silent bus hang rather than an error:

  --ufs      the UFS host (the root filesystem lives there)
  --power    MAX77705 PMIC, charger and fuel gauge
  --touch    the S6SY761 touch controller (its rails are cycled by s9p-acpm)
  --gpu      Mali-G72, its power domain and the G3D clock provider
  --decon    DECON scanout, taking the display from the bootloader framebuffer
  --wlan     the PCIe root complex the BCM4361 sits on
"""
import argparse
import sys
from pathlib import Path

ap = argparse.ArgumentParser()
ap.add_argument("src")
ap.add_argument("out")
ap.add_argument("--ufs", action="store_true")
ap.add_argument("--power", action="store_true")
ap.add_argument("--touch", action="store_true")
ap.add_argument("--gpu", action="store_true")
ap.add_argument("--decon", action="store_true")
ap.add_argument("--wlan", action="store_true")
ap.add_argument("--fallback", choices=("boot", "recovery"), default="boot")
args = ap.parse_args()

s = Path(args.src).read_text()


def sub(old, new, count=1):
    global s
    if s.count(old) != count:
        sys.exit(f"make-dts: expected {count}x {old!r}, found {s.count(old)}")
    s = s.replace(old, new)


def enable(marker, count=1):
    """Turn on a node the S9+ build switches with a sed of its own."""
    sub(f'status = "disabled"; /* {marker}-STATUS */',
        f'status = "okay"; /* {marker}-STATUS */', count)


def disable(anchor, count=1):
    """Turn off a node that is on in the source, after its last property."""
    sub(anchor, anchor + '\n\t\t\tstatus = "disabled";', count)


sub('model = "Samsung Galaxy S9+ (SM-G965F)";', 'model = "Samsung Galaxy S9 (SM-G960N)";')
sub('compatible = "samsung,star2lte", "samsung,exynos9810";',
    'compatible = "samsung,starlte", "samsung,exynos9810";')

sub("""		      <0x8 0x80000000 0x80000000>,
		      <0x9 0x00000000 0x80000000>;""",
    """		      <0x8 0x80000000 0x80000000>;""")

sub("energy-full-design-microwatt-hours = <13480000>;", "energy-full-design-microwatt-hours = <11550000>;")
sub("charge-full-design-microamp-hours = <3500000>;", "charge-full-design-microamp-hours = <3000000>;")
sub("voltage-max-design-microvolt = <4400000>;", "voltage-max-design-microvolt = <4350000>;")
sub("constant-charge-current-max-microamp = <2150000>;", "constant-charge-current-max-microamp = <1500000>;")
sub("constant-charge-voltage-max-microvolt = <4400000>;", "constant-charge-voltage-max-microvolt = <4350000>;")

if args.fallback == "boot":
    sub("<0x1406080c 0x12345674>,	/* PMU INFORM3 = recovery */",
        "<0x1406080c 0x12345670>,	/* PMU INFORM3 = normal boot: TWRP sits on BOOT */")

if args.ufs:
    enable("UFS", count=2)
if args.gpu:
    enable("GPU")
if args.decon:
    enable("DECON")
if args.wlan:
    enable("WLAN")
    s += '''
/* PCI restores endpoint configuration in resume_noirq. Restore its GPIO
 * providers first, including WL_REG_ON and PERST# mux/data registers. */
&pinctrl_fsys1 { samsung,noirq-resume; };
&pinctrl_peric0 { samsung,noirq-resume; };
'''

if not args.power:
    disable("""			interrupt-controller;
			#interrupt-cells = <1>;""")
    disable("""			interrupt-parent = <&max77705>;
			interrupts = <0>;""")
    disable("""			interrupt-parent = <&max77705>;
			interrupts = <2>;""")

if not args.touch:
    disable("			touchscreen-size-y = <4096>;")

if not args.gpu:
    # A power domain with no consumer is powered off by genpd, and that
    # sequence is only validated with the GPU running.
    sub("""		#power-domain-cells = <0>;
		label = "G3D";""", """		#power-domain-cells = <0>;
		label = "G3D";
		status = "disabled";""")
    sub("""		clock-output-names = "g3d";""", """		clock-output-names = "g3d";
		status = "disabled";""")

Path(args.out).write_text(s)
print("make-dts: {} (ufs={} power={} touch={} gpu={} decon={} wlan={} fallback={})".format(
    args.out, args.ufs, args.power, args.touch, args.gpu, args.decon, args.wlan, args.fallback))
