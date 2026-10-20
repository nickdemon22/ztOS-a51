// SPDX-License-Identifier: GPL-2.0-only
/* Exynos9810 SYS_SLEEP backend. Default load only maps and validates tables. */
#include <linux/arm-smccc.h>
#include <linux/cpu.h>
#include <linux/cpu_pm.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/suspend.h>
#include <asm/memory.h>
#include "sequence.h"
#include "sleep-tables.h"
#include "init-memory.h"

static bool enable_deep, dry_run = true, usbl2, probe_save;
static bool prepare_only = true;
static bool mif_sleep;
static unsigned int init_changes, init_verified;
module_param(init_changes, uint, 0444);
module_param(init_verified, uint, 0444);
static unsigned int mif_gate_before, mif_requests_before;
module_param(mif_gate_before, uint, 0444);
module_param(mif_requests_before, uint, 0444);
module_param(prepare_only, bool, 0600);
MODULE_PARM_DESC(prepare_only, "prepare and restore inside saved CPU context without SMC");
static unsigned long suspend_enter, resume;
static unsigned int eint_wake = 0xffefbffd;
module_param(enable_deep, bool, 0444);

module_param(probe_save, bool, 0444);
module_param(suspend_enter, ulong, 0400);
module_param(resume, ulong, 0400);
module_param(eint_wake, uint, 0600);
MODULE_PARM_DESC(enable_deep, "register the platform backend (pins module until reboot)");
MODULE_PARM_DESC(dry_run, "refuse the firmware call and leave PMUCAL registers untouched");
static int set_dry_run(const char *value, const struct kernel_param *parameter)
{
	unsigned int flags = lock_system_sleep();
	int ret = param_set_bool(value, parameter);
	unlock_system_sleep(flags);
	return ret;
}
static const struct kernel_param_ops dry_run_ops = {
	.set = set_dry_run, .get = param_get_bool,
};
module_param_cb(dry_run, &dry_run_ops, &dry_run, 0600);
module_param_cb(usbl2, &dry_run_ops, &usbl2, 0600);
module_param_cb(mif_sleep, &dry_run_ops, &mif_sleep, 0600);
MODULE_PARM_DESC(mif_sleep, "permit MIF shutdown only at the final saved-context SMC handoff");

struct region { u32 physical; void __iomem *mapped; };
static struct region regions[128];
static unsigned int region_count, last_index;
static int last_error, last_smc, last_restore_error;
static unsigned long attempts, entries;
static unsigned int wake_stat, wake_stat2, wake_stat3, bid_pending;
module_param(wake_stat, uint, 0444);
module_param(wake_stat2, uint, 0444);
module_param(wake_stat3, uint, 0444);
module_param(bid_pending, uint, 0444);
module_param(region_count, uint, 0444);
module_param(last_index, uint, 0444);
module_param(last_error, int, 0444);
module_param(last_smc, int, 0444);
module_param(last_restore_error, int, 0444);
module_param(attempts, ulong, 0444);
module_param(entries, ulong, 0444);

struct table { struct zrn_seq *seq; unsigned int count; };
#define TABLE(n) { n, ARRAY_SIZE(n) }
static struct table modes[2][4] = {
	{ TABLE(enter_sleep), TABLE(save_sleep), TABLE(exit_sleep), TABLE(earlywkup_sleep) },
	{ TABLE(enter_sleep_usbl2), TABLE(save_sleep_usbl2), TABLE(exit_sleep_usbl2), TABLE(earlywkup_sleep_usbl2) },
};
static const u32 wake_offsets[] = { 0x610, 0x614, 0x618, 0x644 };
static u32 saved_wake[4], saved_idle[4], saved_eint, saved_eint2, saved_hint;
static bool prepared, early;
/* Boot setup configures USI outside the vendor clock save tables. Preserve
 * its I2C selection, reset and clock-request state across domain reset. */
static const u32 usi_base[] = { 0x10410000, 0x14210000, 0x104b0000, 0x104b0000, 0x14360000, 0x14360000 };
static const u32 usi_offset[] = { 0x101c, 0x2030, 0xc8, 0xc4, 0xc8, 0xc4 };
static u32 saved_usi[ARRAY_SIZE(usi_base)];
static u32 saved_eint_mask[5], saved_wdt0[3];
static unsigned int pending_before[16], pending_after[16];
static unsigned int pending_count = 16;
module_param_array(pending_before, uint, &pending_count, 0444);
module_param_array(pending_after, uint, &pending_count, 0444);
static const unsigned int eint_offset[] = { 0, 4, 8, 12, 36 };
static unsigned int eint_before[5];
static unsigned int eint_pending[5];
static unsigned int eint_before_count = 5;
module_param_array(eint_before, uint, &eint_before_count, 0444);
static unsigned int eint_pending_count = ARRAY_SIZE(eint_pending);
module_param_array(eint_pending, uint, &eint_pending_count, 0444);

static void __iomem *mapped_base(u32 base)
{
	unsigned int i;
	for (i = 0; i < region_count; i++)
		if (regions[i].physical == base)
			return regions[i].mapped;
	return NULL; /* all addresses are validated and mapped at initialization */
}

static int map_base(u32 base)
{
	void __iomem *mapped;
	if (!base || mapped_base(base))
		return 0;
	if (region_count == ARRAY_SIZE(regions) || (base & 0xffff))
		return -EINVAL;
	mapped = ioremap(base, 0x10000);
	if (!mapped)
		return -ENOMEM;
	regions[region_count++] = (struct region) { base, mapped };
	return 0;
}

static u32 register_read(void *context, u32 base, u32 offset)
{
	return readl_relaxed(mapped_base(base) + offset);
}

static void register_write(void *context, u32 base, u32 offset, u32 value)
{
	writel_relaxed(value, mapped_base(base) + offset);
}

static void seq_delay(void *context, unsigned int usec) { udelay(usec); }
static const struct zrn_seq_io io = {
	.read = register_read, .write = register_write, .delay_us = seq_delay,
};

static int run_table(unsigned int index, enum zrn_seq_phase phase)
{
	struct table *table = &modes[usbl2][index];
	int ret = zrn_sequence_run(table->seq, table->count, phase, &io, &last_index);
	if (ret)
		pr_err("zrn-system-sleep: table=%u index=%u register=%s error=%d\n",
		       index, last_index, table->seq[last_index].name, ret);
	return ret;
}

static void restore_platform(void)
{
	void __iomem *pmu = mapped_base(0x14060000);
	int ret, i;
	if (!prepared)
		return;
	ret = run_table(early ? 3 : 2, ZRN_HANDLE);
	last_restore_error = run_table(1, ZRN_RESTORE);
	if (!last_restore_error)
		last_restore_error = ret;
	for (i = 0; i < ARRAY_SIZE(wake_offsets); i++) {
		writel_relaxed(saved_wake[i], pmu + wake_offsets[i]);
		writel_relaxed(saved_idle[i], pmu + 0x3f0 + 4 * i);
	}
	for (i = 0; i < ARRAY_SIZE(eint_offset); i++)
		writel_relaxed(saved_eint_mask[i], mapped_base(0x14050000) + 0x900 + eint_offset[i]);
	/* The unbound cluster-0 watchdog must retain its boot-disabled state. */
	writel_relaxed(0, mapped_base(0x10050000));
	writel_relaxed(saved_wdt0[1], mapped_base(0x10050000) + 4);
	writel_relaxed(saved_wdt0[2], mapped_base(0x10050000) + 8);
	writel_relaxed(saved_wdt0[0], mapped_base(0x10050000));
	for (i = 0; i < ARRAY_SIZE(usi_base); i++)
		writel_relaxed(saved_usi[i], mapped_base(usi_base[i]) + usi_offset[i]);
	writel_relaxed(saved_eint, pmu + 0x60c);
	writel_relaxed(saved_eint2, pmu + 0x61c);
	writel_relaxed(saved_hint, pmu + 0x860);
	wmb();
	prepared = false;
}

static int prepare_platform(void)
{
	void __iomem *pmu = mapped_base(0x14060000);
	int ret, i;
	if (pm_suspend_target_state != PM_SUSPEND_MEM || dry_run)
		return 0;
	if (num_online_cpus() != 1 || raw_smp_processor_id() != 0)
		return -EBUSY;
	for (i = 0; i < ARRAY_SIZE(usi_base); i++)
		saved_usi[i] = readl_relaxed(mapped_base(usi_base[i]) + usi_offset[i]);
	for (i = 0; i < 3; i++)
		saved_wdt0[i] = readl_relaxed(mapped_base(0x10050000) + i * 4);
	for (i = 0; i < ARRAY_SIZE(eint_offset); i++) {
		saved_eint_mask[i] = readl_relaxed(mapped_base(0x14050000) + 0x900 + eint_offset[i]);
		writel_relaxed(saved_eint_mask[i] | (i == 4 ? 0xff : ((eint_wake >> (i * 8)) & 0xff)), mapped_base(0x14050000) + 0x900 + eint_offset[i]);
	}
	saved_eint = readl_relaxed(pmu + 0x60c);
	saved_eint2 = readl_relaxed(pmu + 0x61c);
	saved_hint = readl_relaxed(pmu + 0x860);
	for (i = 0; i < ARRAY_SIZE(wake_offsets); i++) {
		saved_wake[i] = readl_relaxed(pmu + wake_offsets[i]);
		saved_idle[i] = readl_relaxed(pmu + 0x3f0 + 4 * i);
		/* Stock SYS_SLEEP idle-IP masks are all ones. RTC alarm is
		 * wake-status bit1 (SPI31); retain the independent timed wake. */
		writel_relaxed(~0U, pmu + 0x3f0 + 4 * i);
		writel_relaxed(i == 0 ? ((usbl2 ? 0xd00d7e7e : 0xd00f7e7e) & ~BIT(1)) :
			       (i == 3 ? 0 : 0xffff00ff), pmu + wake_offsets[i]);
	}
	writel_relaxed(eint_wake, pmu + 0x60c);
	writel_relaxed(~0U, pmu + 0x61c);
	writel_relaxed(usbl2 ? 5 : 4, pmu + 0x860);
	prepared = true;
	early = true;
	last_restore_error = 0;
	ret = run_table(1, ZRN_SAVE);
	if (!ret)
		ret = run_table(0, ZRN_HANDLE);
	wmb();
	if (ret)
		restore_platform();
	return ret;
}

/* The live ACPM image copies debug MIF_en (0x95c) into its runtime
 * enable field: a zero bypasses MIF sequencing (Thumb offsets 0x9190,
 * 0x949a, 0x93e0). Hold zero throughout Linux's suspend preparation.
 * No firmware request counts, owner flags or sequencer state are edited. */
static void hold_mif(void)
{
	writel_relaxed(0, mapped_base(0x02040000) + 0xf95c);
	dsb(sy);
}

static int begin_platform(void)
{
	mif_gate_before = readl_relaxed(mapped_base(0x02040000) + 0xf95c);
	if (mif_gate_before > 1)
		return -EINVAL;
	hold_mif();
	return 0;
}

static int sleep_finisher(unsigned long state)
{
	register unsigned long a0 asm("x0");
	register unsigned long a1 asm("x1");
	register unsigned long a2 asm("x2");
	register unsigned long a3 asm("x3");
	register unsigned long a4 asm("x4");
	register unsigned long a5 asm("x5");
	register unsigned long a6 asm("x6");
	register unsigned long a7 asm("x7");
	void __iomem *gate = mapped_base(0x02040000) + 0xf95c;
	unsigned long enable = mif_sleep;
	int i, ret = prepare_platform();
	if (ret)
		return ret;
	if (prepare_only) {
		restore_platform();
		return -EAGAIN;
	}
	/* Discard only latched events from explicitly masked diagnostic sources;
	 * preserve the power-key latch and all enabled wake sources. */
	for (i = 0; i < ARRAY_SIZE(eint_offset); i++) {
		u32 mask = i == 4 ? 0xff : ((eint_wake >> (i * 8)) & 0xff);
		eint_before[i] = readl_relaxed(mapped_base(0x14050000) + 0xa00 + eint_offset[i]);
		writel_relaxed(eint_before[i] & mask, mapped_base(0x14050000) + 0xa00 + eint_offset[i]);
	}
	wmb();
	for (i = 0; i < 16; i++)
		pending_before[i] = readl_relaxed(mapped_base(0x10100000) + 0x1200 + i * 4) & readl_relaxed(mapped_base(0x10100000) + 0x1100 + i * 4);
	mif_requests_before = (readl_relaxed(mapped_base(0x02040000) + 0xf8ac) >> 16) & 0xff;
	/* Keep the enable/SMC/abort-disable instructions in one cache line.
	 * x19 is preserved by the secure-call ABI. Do not call or delay in
	 * the interval where ACPM is permitted to remove DRAM power. */
	a0 = 0xc4000001;
	a1 = state;
	a2 = resume;
	a3 = a4 = a5 = a6 = a7 = 0;
	asm volatile(
		"mov x19, %8\n"
		".balign 64\n"
		"str %w9, [x19]\n"
		"dsb sy\n"
		"smc #0\n"
		"str wzr, [x19]\n"
		"dsb sy\n"
		: "+r"(a0), "+r"(a1), "+r"(a2), "+r"(a3),
		  "+r"(a4), "+r"(a5), "+r"(a6), "+r"(a7)
		: "r"(gate), "r"(enable)
		: "x19", "x8", "x9", "x10", "x11", "x12", "x13",
		  "x14", "x15", "x16", "x17", "memory");
	last_smc = (int)a0;
	return last_smc;
}

static int enter_sleep_state(suspend_state_t state)
{
	int (*suspend_fn)(unsigned long, int (*)(unsigned long)) = (void *)suspend_enter;
	int ret, i;
	if (dry_run) {
		pr_info("zrn-system-sleep: dry run reached platform enter; no firmware call\n");
		return -EAGAIN;
	}
	ret = cpu_pm_enter();
	if (ret)
		return ret;
	ret = cpu_cluster_pm_enter();
	if (ret)
		goto cpu_exit;
	attempts++;
	ret = suspend_fn(0x03000000, sleep_finisher);
	hold_mif();
	last_error = ret;
	for (i = 0; i < 16; i++)
		pending_after[i] = readl_relaxed(mapped_base(0x10100000) + 0x1200 + i * 4) & readl_relaxed(mapped_base(0x10100000) + 0x1100 + i * 4);
	wake_stat = readl_relaxed(mapped_base(0x14060000) + 0x600);
	wake_stat2 = readl_relaxed(mapped_base(0x14060000) + 0x604);
	wake_stat3 = readl_relaxed(mapped_base(0x14060000) + 0x608);
	bid_pending = readl_relaxed(mapped_base(0x14070000) + 0x408);
	for (i = 0; i < ARRAY_SIZE(eint_pending); i++)
		eint_pending[i] = readl_relaxed(mapped_base(0x14050000) + 0xa00 + eint_offset[i]);
	early = ret != 0;
	if (!ret)
		entries++;
	/* Undo PMUCAL before CPU PM notifiers touch restored clocks. */
	restore_platform();
	cpu_cluster_pm_exit();
cpu_exit:
	cpu_pm_exit();
	return ret;
}
static const struct platform_suspend_ops platform_ops = {
	.valid = suspend_valid_only_mem, .prepare = begin_platform, .enter = enter_sleep_state,
};

static void unmap_all(void)
{
	while (region_count)
		iounmap(regions[--region_count].mapped);
}

/* Samsung's low-power init is distinct from its save/enter/exit tables.
 * Apply the audited subset once while CPUs and DRAM are fully awake. */
static int initialize_sleep_defaults(void)
{
	u32 original[ARRAY_SIZE(init_settings)];
	unsigned int i, j;
	for (i = 0; i < ARRAY_SIZE(init_settings); i++) {
		const struct zrn_init_setting *setting = &init_settings[i];
		void __iomem *reg = mapped_base(setting->base) + setting->offset;
		u32 old = readl_relaxed(reg);
		u32 value = (old & ~setting->mask) | setting->value;
		u32 required = setting->mask | setting->value;
		/* For MIF SYSREG DRCG, verify observed set bits and bit12; reserved bits
		 * read zero even after Samsung's all-ones write. Live S9
		 * readback is 0x80000e00, with the vendor-required bit12 clear. */
		if (setting->offset == 0x104 &&
		    (setting->base == 0x1b810000 || setting->base == 0x1b910000 ||
		     setting->base == 0x1ba10000 || setting->base == 0x1bb10000))
			required = 0x80001e00;
		original[i] = old;
		if (value != old) {
			writel_relaxed(value, reg);
			init_changes++;
		}
		if ((readl_relaxed(reg) & required) != (setting->value & required)) {
			pr_err("zrn-system-sleep: init verification failed: %s read=%08x expected=%08x mask=%08x\n", setting->name, readl_relaxed(reg), setting->value, required);
			for (j = i + 1; j > 0; j--) {
				const struct zrn_init_setting *undo = &init_settings[j - 1];
				writel_relaxed(original[j - 1], mapped_base(undo->base) + undo->offset);
			}
			dsb(sy);
			return -EIO;
		}
		init_verified++;
	}
	dsb(sy);
	pr_info("zrn-system-sleep: initialized %u Samsung prerequisites, changed %u registers\n",
		init_verified, init_changes);
	return 0;
}

static int __init system_sleep_init(void)
{
	unsigned int mode, table, index;
	int ret;
	if (!of_machine_is_compatible("samsung,exynos9810"))
		return -ENODEV;
	if (probe_save && enable_deep)
		return -EINVAL;
#ifdef ZRN_TABLE_CHECK_ONLY
	if (enable_deep || !probe_save)
		return -EINVAL;
#endif
	if (enable_deep && (!suspend_enter || !resume))
		return -EINVAL;
	if (resume >= 0xffff000000000000UL)
		resume = __pa_symbol(resume);
	for (mode = 0; mode < ARRAY_SIZE(modes); mode++)
		for (table = 0; table < ARRAY_SIZE(modes[0]); table++)
			for (index = 0; index < modes[mode][table].count; index++) {
				struct zrn_seq *s = &modes[mode][table].seq[index];
				if (!s->base || s->offset >= 0x10000 ||
				    s->cond_offset >= 0x10000) {
					ret = -EINVAL;
					goto fail;
				}
				ret = map_base(s->base);
				if (!ret)
					ret = map_base(s->cond_base);
				if (ret)
					goto fail;
			}
	for (index = 0; index < ARRAY_SIZE(usi_base); index++) {
		ret = map_base(usi_base[index]);
		if (ret)
			goto fail;
	}
	for (index = 0; index < ARRAY_SIZE(init_settings); index++) {
		ret = map_base(init_settings[index].base);
		if (ret)
			goto fail;
	}
	ret = map_base(0x02040000);
	if (ret)
		goto fail;
	ret = map_base(0x10100000);
	if (ret)
		goto fail;
	ret = map_base(0x10050000);
	if (ret)
		goto fail;
	ret = map_base(0x14050000);
	if (ret)
		goto fail;
	pr_info("zrn-system-sleep: validated sleep tables, mapped %u regions; enabled=%d dry_run=%d\n",
		region_count, enable_deep, dry_run);
	if (probe_save) {
		unsigned int saved = 0;
		ret = run_table(1, ZRN_SAVE);
		if (ret)
			goto fail;
		for (index = 0; index < modes[usbl2][1].count; index++)
			saved += modes[usbl2][1].seq[index].valid;
		pr_info("zrn-system-sleep: read-only save probe completed, %u registers captured; no restore or writes\n",
			saved);
	}
	if (enable_deep) {
		/* suspend_set_ops() has no module owner and does not remove the
		 * advertised deep state on unregister. Pin until reboot. */
		hold_mif();
		ret = initialize_sleep_defaults();
		if (ret)
			goto fail;
		__module_get(THIS_MODULE);
		suspend_set_ops(&platform_ops);
	}
	return 0;
fail:
	unmap_all();
	return ret;
}
static void __exit system_sleep_exit(void) { unmap_all(); }
module_init(system_sleep_init);
module_exit(system_sleep_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Exynos9810 SYS_SLEEP platform suspend backend");
