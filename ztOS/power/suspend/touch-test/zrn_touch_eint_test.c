// SPDX-License-Identifier: GPL-2.0-only
/* Bounded full-sleep diagnostic: disconnect only touch EINT while CPU0
 * enters platform sleep. Power-key and sensor-hub pin functions stay intact. */
#include <linux/module.h>
#include <linux/io.h>
#include <linux/cpu.h>
#include <linux/cpu_pm.h>
#include <linux/suspend.h>
static void __iomem *pins;
static u32 saved;
static bool changed;
static int notify(struct notifier_block *nb, unsigned long event, void *data)
{
 if (event == CPU_PM_ENTER && pm_suspend_target_state == PM_SUSPEND_MEM &&
     raw_smp_processor_id() == 0 && num_online_cpus() == 1) {
  saved = readl(pins + 0x40);
  writel(saved & ~0xfU, pins + 0x40);
  changed = true;
 } else if ((event == CPU_PM_EXIT || event == CPU_PM_ENTER_FAILED) && changed) {
  writel(saved, pins + 0x40);
  changed = false;
 }
 return NOTIFY_OK;
}
static struct notifier_block notifier = { .notifier_call = notify };
static int __init start(void)
{
 pins = ioremap(0x14050000, 0x1000);
 if (!pins) return -ENOMEM;
 return cpu_pm_register_notifier(&notifier);
}
static void __exit stop(void)
{
 cpu_pm_unregister_notifier(&notifier);
 if (changed) writel(saved, pins + 0x40);
 iounmap(pins);
}
module_init(start);
module_exit(stop);
MODULE_LICENSE("GPL");
