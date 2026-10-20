#!/usr/bin/env python3
"""Execute the patched kernel's actual hash macro under qemu-aarch64.

Checks both suspend/resume register assignments, all eight S9 hardware
MPIDRs, and the unchanged behavior when the Exynos-specific flag is off.
Run after patch-suspend-mpidr.py has patched build/linux-7.1.
"""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parent.parent
source = (root / "build/linux-7.1/arch/arm64/kernel/sleep.S").read_text()
macro = re.search(r"\t\.macro compute_mpidr_hash .*?\t\.endm", source, re.S).group(0)
if "exynos9810_suspend_mpidr_normalize" not in macro:
    raise SystemExit("kernel hash patch has not been applied")
cc = root / "build/toolchain/arm-gnu-toolchain-14.2.rel1-x86_64-aarch64-none-linux-gnu/bin/aarch64-none-linux-gnu-gcc"
qemu = shutil.which("qemu-aarch64") or shutil.which("qemu-aarch64-static")
if not qemu:
    raise SystemExit("qemu-aarch64 is required")
assembly = r"""
.macro ldr_l dst, sym
    adrp \dst, \sym
    ldr \dst, [\dst, :lo12:\sym]
.endm
""" + macro + r"""
.text
.global save_hash
save_hash:
    mov x7, x0
    mov x3, #0
    mov x4, #6
    mov x5, #14
    mov x6, #30
    mov x10, #0x103
    compute_mpidr_hash x8, x3, x4, x5, x6, x7, x10
    mov x0, x8
    ret
.global resume_hash
resume_hash:
    mov x1, x0
    mov x3, #0
    mov x4, #6
    mov x5, #14
    mov x6, #30
    mov x2, #0x103
    compute_mpidr_hash x7, x3, x4, x5, x6, x1, x2
    mov x0, x7
    ret
.section .note.GNU-stack,"",%progbits
"""
c = """
#include <assert.h>
#include <stdio.h>
unsigned long exynos9810_suspend_mpidr_normalize;
extern unsigned long save_hash(unsigned long), resume_hash(unsigned long);
int main(void) {
    unsigned long ids[] = {0x81000000,0x81000100,0x81000200,0x81000300,
                           0x80000100,0x80000101,0x80000102,0x80000103};
    unsigned long old[] = {0,4,0,4,4,5,6,7};
    for (unsigned long enabled=0; enabled<2; enabled++) {
        exynos9810_suspend_mpidr_normalize=enabled;
        for (unsigned long cpu=0; cpu<8; cpu++) {
            unsigned long expected=enabled ? cpu : old[cpu];
            assert(save_hash(ids[cpu])==expected);
            assert(resume_hash(ids[cpu])==expected);
        }
    }
    puts("Actual suspend/resume assembly: eight unique slots; platform-off behavior preserved");
}
"""
with tempfile.TemporaryDirectory(prefix="ztos-mpidr-test-") as tmp:
    tmp = Path(tmp)
    (tmp / "hash.S").write_text(assembly)
    (tmp / "test.c").write_text(c)
    subprocess.run([str(cc), "-static", "-O2", str(tmp / "test.c"),
                    str(tmp / "hash.S"), "-o", str(tmp / "test")], check=True)
    subprocess.run([qemu, str(tmp / "test")], check=True)
