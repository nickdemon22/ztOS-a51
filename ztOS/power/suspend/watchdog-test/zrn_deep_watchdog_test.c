// SPDX-License-Identifier: GPL-2.0-only
/* Bounded 10s trials: rearm the systemd-owned 30s watchdog after its
 * suspend callback stops it. Normal watchdog resume retakes ownership. */
#include <linux/module.h>
#include <linux/io.h>
#include <linux/cpu.h>
#include <linux/cpu_pm.h>
#include <linux/suspend.h>
static void __iomem *wdt;
static unsigned int arms;
module_param(arms, uint, 0444);
static int notify(struct notifier_block *nb, unsigned long event, void *data)
{
 if (event == CPU_PM_ENTER && pm_suspend_target_state == PM_SUSPEND_MEM &&
     raw_smp_processor_id() == 0 && num_online_cpus() == 1) {
  u32 control = readl(wdt);
  if (!(control & BIT(5))) {
   writel(readl(wdt + 4), wdt + 8);
   writel((control | BIT(5) | BIT(0)) & ~BIT(2), wdt);
   arms++;
  }
 }
 return NOTIFY_OK;
}
static struct notifier_block notifier = { .notifier_call = notify };
static int __init start(void)
{
 int ret;
 wdt = ioremap(0x10060000, 0x1000);
 if (!wdt) return -ENOMEM;
 ret = cpu_pm_register_notifier(&notifier);
 if (ret) iounmap(wdt);
 return ret;
}
static void __exit stop(void)
{
 cpu_pm_unregister_notifier(&notifier);
 iounmap(wdt);
}
module_init(start);
module_exit(stop);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Rearm the existing watchdog for bounded full-sleep bringup");
