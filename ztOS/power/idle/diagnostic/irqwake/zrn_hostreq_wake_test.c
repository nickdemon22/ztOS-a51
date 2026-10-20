// SPDX-License-Identifier: GPL-2.0
/*
 * Temporary A/B diagnostic for the Exynos9810 BCM4773 host-request IRQ.
 *
 * ttyBCM enables IRQ wake while its userspace file is open. This module
 * balances exactly one such enable for one bounded suspend trial; unloading
 * it restores wake on IRQ 105. It never disables the IRQ handler itself.
 */
#include <linux/interrupt.h>
#include <linux/module.h>

#define BCM4773_HOST_REQ_IRQ 105

static bool wake_masked;

static int __init hostreq_wake_test_init(void)
{
	int ret = disable_irq_wake(BCM4773_HOST_REQ_IRQ);

	if (ret) {
		pr_err("zrn-hostreq-wake-test: disable IRQ %u wake failed: %d\n",
		       BCM4773_HOST_REQ_IRQ, ret);
		return ret;
	}
	wake_masked = true;
	pr_info("zrn-hostreq-wake-test: temporarily suppressed IRQ %u wake\n",
		BCM4773_HOST_REQ_IRQ);
	return 0;
}

static void __exit hostreq_wake_test_exit(void)
{
	int ret;

	if (!wake_masked)
		return;
	ret = enable_irq_wake(BCM4773_HOST_REQ_IRQ);
	if (ret)
		pr_err("zrn-hostreq-wake-test: restore IRQ %u wake failed: %d\n",
		       BCM4773_HOST_REQ_IRQ, ret);
	else
		pr_info("zrn-hostreq-wake-test: restored IRQ %u wake\n",
			BCM4773_HOST_REQ_IRQ);
}

module_init(hostreq_wake_test_init);
module_exit(hostreq_wake_test_exit);
MODULE_DESCRIPTION("Bounded BCM4773 host-request wake IRQ A/B diagnostic");
MODULE_LICENSE("GPL");
