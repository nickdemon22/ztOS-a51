// SPDX-License-Identifier: GPL-2.0
/* Read-only cpuidle diagnostic. Addresses come from this kernel's kallsyms. */
#include <linux/cpuidle.h>
#include <linux/module.h>
#include <linux/percpu.h>
#include <linux/smp.h>
#include <linux/tick.h>
#include <linux/clockchips.h>
#include "tick-internal.h"

static unsigned long initialized_addr, enabled_addr, poll_addr, devices_addr;
static unsigned long force_addr, pending_addr, oneshot_addr, broadcast_fn;
module_param(initialized_addr, ulong, 0400);
module_param(enabled_addr, ulong, 0400);
module_param(poll_addr, ulong, 0400);
module_param(devices_addr, ulong, 0400);
module_param(force_addr, ulong, 0400);
module_param(pending_addr, ulong, 0400);
module_param(oneshot_addr, ulong, 0400);
module_param(broadcast_fn, ulong, 0400);

static void show_mask(const char *name, unsigned long address)
{
	struct cpumask *mask;
	if (!address)
		return;
#ifdef CONFIG_CPUMASK_OFFSTACK
	mask = *(struct cpumask **)address;
#else
	mask = (struct cpumask *)address;
#endif
	pr_info("zrn-idle-diag: %s=%*pbl\n", name, cpumask_pr_args(mask));
}

static void inspect(void *unused)
{
	struct cpuidle_device *dev;
	struct cpuidle_driver *drv;
	int cpu = smp_processor_id();

	dev = *per_cpu_ptr((struct cpuidle_device * __percpu *)devices_addr, cpu);
	drv = cpuidle_get_cpu_driver(dev);
	pr_info("zrn-idle-diag: cpu%d dev=%px enabled=%d registered=%d drv=%s\n",
		cpu, dev, dev ? dev->enabled : -1, dev ? dev->registered : -1,
		drv ? drv->name : "none");
}

static int __init diagnostic_init(void)
{
	if (!initialized_addr || !enabled_addr || !poll_addr || !devices_addr)
		return -EINVAL;
	pr_info("zrn-idle-diag: initialized=%d enabled_devices=%d force_poll=%d\n",
		READ_ONCE(*(int *)initialized_addr), READ_ONCE(*(int *)enabled_addr),
		READ_ONCE(*(int *)poll_addr));
	show_mask("broadcast force", force_addr);
	show_mask("broadcast pending", pending_addr);
	show_mask("broadcast oneshot", oneshot_addr);
	if (broadcast_fn) {
		struct tick_device *td = ((struct tick_device *(*)(void))broadcast_fn)();
		struct clock_event_device *bc = td->evtdev;
		pr_info("zrn-idle-diag: broadcast=%s mode=%d state=%d next=%lld now=%lld\n",
			bc ? bc->name : "none", td->mode, bc ? bc->state_use_accessors : -1,
			bc ? bc->next_event : 0, ktime_get());
	}
	on_each_cpu(inspect, NULL, 1);
	return -EAGAIN; /* no state, hooks, or references left behind */
}
module_init(diagnostic_init);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Read-only CPU idle and broadcast timer diagnostics");
