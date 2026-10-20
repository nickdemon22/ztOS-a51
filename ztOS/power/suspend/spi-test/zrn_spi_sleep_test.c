// SPDX-License-Identifier: GPL-2.0-only
#include <linux/module.h>
#include <linux/arm-smccc.h>
#include <linux/io.h>
#include <linux/cpu.h>
#include <linux/cpu_pm.h>
#include <linux/suspend.h>
static void __iomem *spi;
static u32 clock, option;
static bool changed;
static unsigned int cp_ctrl;
module_param(cp_ctrl, uint, 0444);
static int notify(struct notifier_block *nb, unsigned long event, void *data)
{
 if (event == CPU_PM_ENTER && pm_suspend_target_state == PM_SUSPEND_MEM &&
     raw_smp_processor_id() == 0 && num_online_cpus() == 1) {
  clock = readl(spi + 4);
  option = readl(spi + 0xc8);
  writel(clock & ~BIT(8), spi + 4);
  writel((option & ~BIT(1)) | BIT(2), spi + 0xc8);
  changed = true;
 } else if ((event == CPU_PM_EXIT || event == CPU_PM_ENTER_FAILED) && changed) {
  writel(option, spi + 0xc8);
  writel(clock, spi + 4);
  changed = false;
 }
 return NOTIFY_OK;
}
static struct notifier_block notifier = { .notifier_call = notify };
static int __init start(void)
{
 {
  struct arm_smccc_res lo, hi;
  arm_smccc_smc(0x82000700, 3, 0, 1, 0, 0, 0, 0, &lo);
  arm_smccc_smc(0x82000700, 3, 1, 1, 0, 0, 0, 0, &hi);
  if ((lo.a0 & 0xffff) || (hi.a0 & 0xffff)) return -EIO;
  cp_ctrl = ((lo.a0 >> 16) & 0xffff) | (hi.a0 & 0xffff0000);
 }
 spi = ioremap(0x13ac0000, 0x1000);
 if (!spi) return -ENOMEM;
 return cpu_pm_register_notifier(&notifier);
}
static void __exit stop(void)
{
 cpu_pm_unregister_notifier(&notifier);
 if (changed) { writel(option, spi + 0xc8); writel(clock, spi + 4); }
 iounmap(spi);
}
module_init(start);
module_exit(stop);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Bounded BCM4773 host SPI clock-stop diagnostic");
