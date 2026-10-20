// SPDX-License-Identifier: GPL-2.0-only
/* Read-only IRQ evidence at CPU_PM_EXIT after a SICD attempt. */
#include <linux/cpu_pm.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/percpu.h>
#include <linux/smp.h>

static unsigned long attempts_addr, core_addr, error_addr, smc_addr;
module_param(attempts_addr, ulong, 0400);
module_param(core_addr, ulong, 0400);
module_param(error_addr, ulong, 0400);
module_param(smc_addr, ulong, 0400);
static unsigned int samples;
module_param(samples, uint, 0444);
static atomic_t slots = ATOMIC_INIT(0);
static DEFINE_PER_CPU(unsigned long, attempts_before);
static void __iomem *gicd, *gicc, *bid, *mct;

static int observe_return(struct notifier_block *nb, unsigned long event,
			 void *unused)
{
	unsigned long attempts;
	unsigned int cpu = raw_smp_processor_id(), slot;
	u32 pending[16], hppir, control, priority, bid_pending;
	u32 high, low, high_again, attempted_core;
	u64 count;
	int i, error, smc;

	if (event == CPU_PM_ENTER) {
		this_cpu_write(attempts_before,
			READ_ONCE(*(unsigned long *)attempts_addr));
		return NOTIFY_OK;
	}
	if (event != CPU_PM_EXIT)
		return NOTIFY_OK;
	attempts = READ_ONCE(*(unsigned long *)attempts_addr);
	if (attempts == this_cpu_read(attempts_before))
		return NOTIFY_OK;
	slot = atomic_inc_return(&slots);
	if (slot > 16)
		return NOTIFY_OK;

	/* HPPIR and ISPENDR are non-acknowledging reads. Never read IAR or
	 * write the interrupt controller: preserve the original wake policy. */
	hppir = readl_relaxed(gicc + 0x18);
	control = readl_relaxed(gicc);
	priority = readl_relaxed(gicc + 4);
	bid_pending = readl_relaxed(bid + 0x408);
	do {
		high = readl_relaxed(mct + 0x104);
		low = readl_relaxed(mct + 0x100);
		high_again = readl_relaxed(mct + 0x104);
	} while (high != high_again);
	count = ((u64)high << 32) | low;
	attempted_core = READ_ONCE(*(unsigned int *)core_addr);
	for (i = 0; i < ARRAY_SIZE(pending); i++)
		pending[i] = readl_relaxed(gicd + 0x200 + 4 * i) &
			readl_relaxed(gicd + 0x100 + 4 * i);
	error = READ_ONCE(*(int *)error_addr);
	smc = READ_ONCE(*(int *)smc_addr);
	WRITE_ONCE(samples, slot);
	pr_info("zrn-sicd-return: sample=%u cpu=%u sicd_core=%u attempts=%lu error=%d smc=%d hppir=%u ctl=%#x pmr=%#x bid4=%#x mct=%llu\n",
		slot, cpu, attempted_core, attempts, error, smc, hppir, control,
		priority, bid_pending, count);
	for (i = 0; i < ARRAY_SIZE(pending); i++)
		if (pending[i])
			pr_info("zrn-sicd-return: sample=%u irq_base=%u enabled_pending=%#x\n",
				slot, i * 32, pending[i]);
	return NOTIFY_OK;
}

/* Observe after GIC's priority-zero restore notifier, still IRQ-disabled. */
static struct notifier_block observer = {
	.notifier_call = observe_return,
	.priority = -100,
};

static int __init observer_init(void)
{
	int ret, cpu;
	if (!attempts_addr || !core_addr || !error_addr || !smc_addr)
		return -EINVAL;
	gicd = ioremap(0x10101000, 0x1000);
	gicc = ioremap(0x10102000, 0x1000);
	bid = ioremap(0x14070000, 0x1000);
	mct = ioremap(0x10040000, 0x1000);
	if (!gicd || !gicc || !bid || !mct) {
		ret = -ENOMEM;
		goto unmap;
	}
	/* Some CPUs are already in C2 when registration happens; their first
	 * EXIT must not be mistaken for a newly observed SICD attempt. */
	for_each_possible_cpu(cpu)
		per_cpu(attempts_before, cpu) =
			READ_ONCE(*(unsigned long *)attempts_addr);
	ret = cpu_pm_register_notifier(&observer);
	if (!ret)
		return 0;
unmap:
	if (mct)
		iounmap(mct);
	if (bid)
		iounmap(bid);
	if (gicc)
		iounmap(gicc);
	if (gicd)
		iounmap(gicd);
	return ret;
}

static void __exit observer_exit(void)
{
	cpu_pm_unregister_notifier(&observer);
	iounmap(mct);
	iounmap(bid);
	iounmap(gicc);
	iounmap(gicd);
}
module_init(observer_init);
module_exit(observer_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Read-only SICD return interrupt diagnostics");
