#!/usr/bin/env python3
"""Normalize Exynos 9810 A55 MPIDRs for the suspend-context hash.

The DT/PSCI CPU IDs are 0..3 and 0x100..0x103. A55 hardware reports
MT=1 and Aff1=core instead of Aff0=core. Hashing that raw value against
the DT-derived mask aliases CPU0/CPU2 and CPU1/CPU3/CPU4. Normalize only
on this SoC, in both save and resume paths, before hashing.
"""
import sys
from pathlib import Path

root = Path(sys.argv[1])
cpath = root / "arch/arm64/kernel/suspend.c"
spath = root / "arch/arm64/kernel/sleep.S"
c, s = cpath.read_text(), spath.read_text()
marker = "exynos9810_suspend_mpidr_normalize"
if marker in c and marker in s:
    print("patch-suspend-mpidr: already applied")
    sys.exit(0)
if marker in c or marker in s:
    sys.exit("patch-suspend-mpidr: partially applied patch")

changes = [
    ("#include <linux/percpu.h>", "#include <linux/percpu.h>\n#include <linux/of.h>"),
    ("unsigned long *sleep_save_stash;", """unsigned long *sleep_save_stash;

/* Read after the MMU is enabled in both suspend and resume assembly. */
unsigned long exynos9810_suspend_mpidr_normalize __ro_after_init;"""),
    ("static int __init cpu_suspend_init(void)\n{", """static int __init cpu_suspend_init(void)
{
	exynos9810_suspend_mpidr_normalize =
		of_machine_is_compatible("samsung,exynos9810");
	if (exynos9810_suspend_mpidr_normalize)
		pr_info("Exynos9810: normalize MT CPU IDs for suspend context hash\\n");"""),
]
for old, new in changes:
    if c.count(old) != 1:
        sys.exit(f"patch-suspend-mpidr: C anchor changed: {old!r}")
    c = c.replace(old, new)

old = "\tand\t\\mpidr, \\mpidr, \\mask\t\t// mask out MPIDR bits"
new = r"""	/* DT/PSCI uses Aff0=core; the 9810 A55 reports Aff1=core, MT=1.
	 * Keep Aff3 and the non-affinity bits, replacing only Aff[2:0].
	 * This macro is shared by __cpu_suspend_enter and _cpu_resume. */
	ldr_l	\dst, exynos9810_suspend_mpidr_normalize
	cbz	\dst, 991f
	tbz	\mpidr, #24, 991f
	ubfx	\dst, \mpidr, #8, #16
	bfi	\mpidr, \dst, #0, #24
991:
	and	\mpidr, \mpidr, \mask		// mask out MPIDR bits""".lstrip("\n")
if s.count(old) != 1:
    sys.exit("patch-suspend-mpidr: assembly anchor changed")
s = s.replace(old, new)
cpath.write_text(c)
spath.write_text(s)
print("patch-suspend-mpidr: Exynos 9810 suspend/resume hash normalized")
