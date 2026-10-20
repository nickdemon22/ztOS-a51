// SPDX-License-Identifier: GPL-2.0
/* zrn-idleprobe: run the pieces of zrn-idle's power-down path one at a time
 * on one CPU, without powering anything down, to see which one faults. */
#include <linux/cpu_pm.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/smp.h>
#include <asm/cputype.h>

static int cpu = 3, step;
module_param(cpu, int, 0444);
module_param(step, int, 0444);	/* 1: PMU writes, 2: cpu_pm hooks */
static void __iomem *pmu, *bid;

/*
 * The PMU's core number, as Samsung's linear_phycpu(): cluster * 4 + core.
 * The little Cortex-A55s report a multithreading-style MPIDR (MT set, core
 * in Aff1, cluster in Aff2); the big cores the plain one (core in Aff0).
 */
static unsigned int zrn_core(u64 mpidr)
{
	int lvl = (mpidr & MPIDR_MT_BITMASK) ? 1 : 0;

	return (MPIDR_AFFINITY_LEVEL(mpidr, lvl + 1) << 2) | MPIDR_AFFINITY_LEVEL(mpidr, lvl);
}

static void rmw(void __iomem *reg, u32 mask, u32 value)
{
	writel_relaxed((readl_relaxed(reg) & ~mask) | value, reg);
}

static void run(void *unused)
{
	u64 mpidr = read_cpuid_mpidr();
	unsigned int core = zrn_core(mpidr);
	u32 bit = BIT((core + 4) & 7);

	pr_emerg("probe: mpidr %llx -> core %u\n", mpidr, core);
	if (core > 7)
		return;
	if (step == 1) {
		pr_emerg("probe: core %u inform write\n", core); writel(1, pmu + 0x860 + 4 * core); dsb(sy); isb();
		pr_emerg("probe: inform back\n"); writel(0, pmu + 0x860 + 4 * core); dsb(sy);
		pr_emerg("probe: bid enable\n"); rmw(bid + 0x200, bit, bit); dsb(sy);
		pr_emerg("probe: bid disable\n"); rmw(bid + 0x200, bit, 0); dsb(sy);
		pr_emerg("probe: grp1 clear\n"); if (readl(bid + 0x108) & bit) rmw(bid + 0x10c, bit, bit); dsb(sy);
		pr_emerg("probe: event enable\n"); rmw(pmu + 0x7f08, bit, bit); dsb(sy);
		pr_emerg("probe: step 1 done\n");
	} else if (step == 2) {
		pr_emerg("probe: cpu_pm_enter\n");
		int ret = cpu_pm_enter();
		dsb(sy); isb();
		pr_emerg("probe: cpu_pm_enter=%d, exit\n", ret);
		if (!ret)
			cpu_pm_exit();
		pr_emerg("probe: step 2 done\n");
	}
}

static int __init probe_init(void)
{
	pmu = ioremap(0x14060000, 0x8000);
	bid = ioremap(0x14070000, 0x1000);
	smp_call_function_single(cpu, run, NULL, 1);
	iounmap(bid);
	iounmap(pmu);
	return -EAGAIN;	/* nothing to keep loaded */
}
module_init(probe_init);
MODULE_DESCRIPTION("zrn-idle probe");
MODULE_LICENSE("GPL");
