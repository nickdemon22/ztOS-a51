#!/usr/bin/env python3
"""Run the real MCT programming function against delayed MMIO test doubles."""
from pathlib import Path
import re
import subprocess
import tempfile

source = (Path(__file__).parent / "zrn-idle.c").read_text()
start = source.index("static int zrn_bc_set_next(")
end = source.index("\nstatic int zrn_bc_stop(", start)
function = source[start:end]
defines = "\n".join(re.findall(r"^#define MCT_(?:G_(?:INT_CSTAT|TCON|COMP0_L|COMP0_U|INT_ENB)|TCON_COMP0_EN)\s+.*$", source, re.M))
test = r"""
#include <stdint.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
typedef uint64_t u64;
typedef uint32_t u32;
typedef int64_t s64;
#define raw_spin_lock_irqsave(lock, flags) do { (flags) = 0; } while (0)
#define raw_spin_unlock_irqrestore(lock, flags) do { (void)(flags); } while (0)
struct clock_event_device { int unused; };
static char registers[4096];
static void *mct = registers;
static u64 ticks;
static unsigned long mct_late_events;
static unsigned int stopped;
static u64 mct_count(void) { return ticks; }
static u32 readl_relaxed(void *p) { return 0; }
static void writel_relaxed(u32 value, void *p) { ticks += 10; }
static void mct_wr(u32 value, u32 offset, u32 status) { ticks += 10; }
static void mct_comp0_stop(void) { stopped++; }
""" + defines + "\n" + function + r"""
int main(void) {
    ticks=1000;
    assert(zrn_bc_set_next(15, 0)==-ETIME);
    assert(stopped==1 && mct_late_events==1);
    ticks=1000;
    assert(zrn_bc_set_next(5200, 0)==0);
    assert(stopped==1 && mct_late_events==1);
    puts("MCT: missed deadlines request retry; usable deadlines succeed");
}
"""
with tempfile.TemporaryDirectory(prefix="ztos-mct-test-") as tmp:
    tmp = Path(tmp)
    (tmp / "test.c").write_text(test)
    subprocess.run(["cc", "-std=gnu11", "-O2", str(tmp / "test.c"),
                    "-o", str(tmp / "test")], check=True)
    subprocess.run([str(tmp / "test")], check=True)
