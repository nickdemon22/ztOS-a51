// SPDX-License-Identifier: GPL-2.0
/*
 * zrn-hotplug: cores that can be taken offline and brought back, on the
 * Exynos 9810 (Galaxy S9) under mainline.
 *
 * Without it, "echo 0 > /sys/devices/system/cpu/cpuN/online" works and
 * "echo 1" fails ("failed to come online") until the phone is restarted.
 * The firmware powers a core up when this SoC's small power controller (the
 * "flexible PMU") raises that core's wake-up line, and the controller only
 * does that for a core whose line was armed before it went down. Samsung's
 * kernel arms it from the dying core itself and disarms it from the core once
 * it is back (exynos-powermode.c: exynos_hotplug_out_callback and
 * exynos_hotplug_in_callback; the register lists are cal-if's coreNN_off and
 * coreNN_on):
 *
 *   going down	CPU_INFORM[core] = 1 (C2: "about to power down")  0x14060860 + 4 * core
 *		GRP2_INTR_BID_ENABLE |= bit            0x14070200
 *		if GRP1 pending, GRP1_INTR_BID_CLEAR    0x14070108 / 0x1407010c
 *		PMU_EVENT_INTERRUPT_ENABLE_GRP2 |= bit   0x14067f08
 *   back up	CPU_INFORM[core] = 0
 *		GRP2_INTR_BID_ENABLE &= ~bit
 *		if GRP2 pending, GRP2_INTR_BID_CLEAR    0x14070208 / 0x1407020c
 *		PMU_EVENT_INTERRUPT_ENABLE_GRP2 &= ~bit
 *
 * bit is the core's line: 4..7 for the little cores, 0..3 for the big ones,
 * by the core's number as the PMU counts it (cluster * 4 + core, the little
 * cluster first; the little Cortex-A55s report it in the multithreading
 * layout of MPIDR).
 *
 * When matters. The hint and the armed line mean "this core is powering
 * down": from then on the controller cuts its power the first time it waits
 * for an interrupt. Done early in the offline sequence (an ordinary hotplug
 * callback), the core was switched off while the kernel was still moving
 * things away from it, and the kernel died within 50 ms. So it is done in
 * the "dying" part, on the core itself with interrupts off, after which it
 * runs straight into the firmware's CPU_OFF. Those steps have fixed numbers
 * in the kernel and none for us, so the one Xen would use (never present
 * here) is borrowed.
 *
 * The last core of a cluster is not let go: with a whole cluster down the
 * PMU powers the cluster itself off, which needs more (Samsung's
 * cluster_disable: the big cluster's reset control and a mailbox bit) and
 * froze the phone when idle did it. So at least one little and one big core
 * always stay during ordinary hotplug. The explicit suspend_cluster_off
 * option allows the last big CPU to leave only for PM_SUSPEND_MEM, with
 * DFD disabled, using the stock CPD hint/mailbox setup and resume cleanup.
 */
#include <linux/cpu.h>
#include <linux/cpuhotplug.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/spinlock.h>
#include <linux/suspend.h>
#include <asm/cputype.h>

#define PMU_BASE		0x14060000
#define PMU_CPU_INFORM(c)	(0x0860 + 4 * (c))
#define INFORM_C2		1
#define PMU_EVENT_INT_EN_GRP2	0x7f08
#define BID_BASE		0x14070000
#define GRP1_PEND		0x0108
#define GRP1_CLEAR		0x010c
#define GRP2_ENABLE		0x0200
#define GRP2_PEND		0x0208
#define GRP2_CLEAR		0x020c

static void __iomem *pmu, *bid, *cluster_mailbox;
static bool suspend_cluster_off;
module_param(suspend_cluster_off, bool, 0644);
MODULE_PARM_DESC(suspend_cluster_off, "allow last big CPU offline only for suspend-to-RAM, DFD off");
static bool big_cluster_down;
static DEFINE_RAW_SPINLOCK(pmu_lock);
static enum cpuhp_state hp_state;
#define ZRN_DYING_STATE	CPUHP_AP_ARM_XEN_STARTING	/* a "dying" step nobody else has here */

/* The PMU's number for the core this runs on, as Samsung's linear_phycpu(). */
static unsigned int this_core(void)
{
	u64 mpidr = read_cpuid_mpidr();
	int lvl = (mpidr & MPIDR_MT_BITMASK) ? 1 : 0;

	return (MPIDR_AFFINITY_LEVEL(mpidr, (lvl + 1)) << 2) | MPIDR_AFFINITY_LEVEL(mpidr, lvl);
}

static void rmw(void __iomem *reg, u32 mask, u32 value)
{
	writel_relaxed((readl_relaxed(reg) & ~mask) | value, reg);
}

/* On the core that has just come back: its line is disarmed. */
static int zrn_hotplug_in(unsigned int cpu)
{
	unsigned int core = this_core();
	u32 bit = BIT((core + 4) & 7);
	unsigned long flags;

	if (core > 7)
		return 0;
	raw_spin_lock_irqsave(&pmu_lock, flags);
	if (core >= 4 && big_cluster_down) {
		rmw(cluster_mailbox + 0x10, BIT(24), 0);
		big_cluster_down = false;
	}
	writel_relaxed(0, pmu + PMU_CPU_INFORM(core));
	rmw(bid + GRP2_ENABLE, bit, 0);
	if (readl_relaxed(bid + GRP2_PEND) & bit)
		rmw(bid + GRP2_CLEAR, bit, bit);
	rmw(pmu + PMU_EVENT_INT_EN_GRP2, bit, 0);
	raw_spin_unlock_irqrestore(&pmu_lock, flags);
	return 0;
}

/* Early in going offline, while it can still be refused: not the last core
 * of its cluster. */
static int zrn_hotplug_may_go(unsigned int cpu)
{
	unsigned int other, left = 0;

	/* (logical CPUs 0..3 are the little cluster, 4..7 the big one) */
	for_each_online_cpu(other)
		left += other != cpu && (other < 4) == (cpu < 4);
	if (!left) {
		/* Ordinary hotplug keeps the original guard. System suspend
		 * needs CPU0 alone; the dying callback supplies stock CPD setup. */
		if (cpu >= 4 && READ_ONCE(suspend_cluster_off) &&
		    pm_suspend_target_state == PM_SUSPEND_MEM &&
		    !(readl_relaxed(pmu + 0x500) & BIT(0)))
			return 0;
		pr_info("zrn-hotplug: cpu%u is the last of its cluster and stays\n", cpu);
		return -EBUSY;
	}
	return 0;
}

/* On the core in its last moments, interrupts off: the hint, and its line
 * armed so that it can be woken again. */
static int zrn_hotplug_out(unsigned int cpu)
{
	unsigned int core = this_core();
	u32 bit = BIT((core + 4) & 7);
	unsigned long flags;
	unsigned int other;
	bool last_big = core >= 4;

	if (core > 7)
		return 0;
	for_each_online_cpu(other)
		if (other != cpu && other >= 4)
			last_big = false;
	raw_spin_lock_irqsave(&pmu_lock, flags);
	/* the hint first: without it the controller leaves the core powered,
	 * parked in the firmware, and nothing can start it again */
	writel_relaxed(INFORM_C2, pmu + PMU_CPU_INFORM(core));
	rmw(bid + GRP2_ENABLE, bit, bit);
	if (readl_relaxed(bid + GRP1_PEND) & bit)
		rmw(bid + GRP1_CLEAR, bit, bit);
	rmw(pmu + PMU_EVENT_INT_EN_GRP2, bit, bit);
	if (last_big && READ_ONCE(suspend_cluster_off) &&
	    pm_suspend_target_state == PM_SUSPEND_MEM) {
		/* pmucal_cpu_cluster_disable(): CPD hint then AP->APM request. */
		writel_relaxed(2, pmu + PMU_CPU_INFORM(core));
		rmw(cluster_mailbox + 0x10, BIT(24), BIT(24));
		big_cluster_down = true;
	}
	raw_spin_unlock_irqrestore(&pmu_lock, flags);
	wmb();
	return 0;
}

static int __init zrn_hotplug_init(void)
{
	int ret;

	if (!of_machine_is_compatible("samsung,starlte") && !of_machine_is_compatible("samsung,exynos9810")) {
		pr_err("zrn-hotplug: not an Exynos 9810\n");
		return -ENODEV;
	}
	pmu = ioremap(PMU_BASE, 0x8000);
	bid = ioremap(BID_BASE, 0x1000);
	cluster_mailbox = ioremap(0x14100000, 0x1000);
	if (!pmu || !bid || !cluster_mailbox) {
		ret = -ENOMEM;
		goto unmap;
	}
	/* (the cores already up are passed over: their lines are as the boot left them) */
	ret = cpuhp_setup_state_nocalls(CPUHP_AP_ONLINE_DYN, "soc/zrn-hotplug:online",
					NULL, zrn_hotplug_may_go);
	if (ret < 0)
		goto unmap;
	hp_state = ret;
	ret = cpuhp_setup_state_nocalls(ZRN_DYING_STATE, "soc/zrn-hotplug:dying",
					zrn_hotplug_in, zrn_hotplug_out);
	if (ret < 0) {
		cpuhp_remove_state_nocalls(hp_state);
		goto unmap;
	}
	pr_info("zrn-hotplug: cores can be taken offline and brought back (one of each cluster stays)\n");
	return 0;
unmap:
	if (cluster_mailbox)
		iounmap(cluster_mailbox);
	if (pmu)
		iounmap(pmu);
	if (bid)
		iounmap(bid);
	return ret;
}

static void __exit zrn_hotplug_exit(void)
{
	cpuhp_remove_state_nocalls(ZRN_DYING_STATE);
	cpuhp_remove_state_nocalls(hp_state);
	iounmap(cluster_mailbox);
	iounmap(pmu);
	iounmap(bid);
}

module_init(zrn_hotplug_init);
module_exit(zrn_hotplug_exit);
MODULE_DESCRIPTION("CPU hotplug for the Exynos 9810 under mainline");
MODULE_LICENSE("GPL");
