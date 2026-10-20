// SPDX-License-Identifier: GPL-2.0-only
/* Temporary correction for the first RTC build, pinned by alarmtimer.
 * Routes stock alarm SPI31 to that loaded driver's existing IRQ handler.
 * No kernel text/data patches, calendar changes, or replacement RTC device.
 * Not needed with the corrected main RTC driver after the next boot. */
#include <linux/cpumask.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irqdomain.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/rtc.h>

static unsigned long handler_addr;
module_param(handler_addr, ulong, 0400);
static struct rtc_device *rtc;
static unsigned int virq;

static int __init bridge_init(void)
{
	struct device_node *gic;
	struct irq_fwspec spec = { .param_count = 3, .param = { 0, 31, IRQ_TYPE_LEVEL_HIGH } };
	int ret;
	if (!handler_addr)
		return -EINVAL;
	rtc = rtc_class_open("rtc0");
	if (!rtc)
		return -ENODEV;
	if (strcmp(dev_name(rtc->dev.parent), "zrn-exynos9810-rtc")) {
		ret = -ENODEV;
		goto close;
	}
	gic = of_find_compatible_node(NULL, NULL, "arm,gic-400");
	if (!gic) {
		ret = -ENODEV;
		goto close;
	}
	spec.fwnode = of_fwnode_handle(gic);
	virq = irq_create_fwspec_mapping(&spec);
	of_node_put(gic);
	if (!virq) {
		ret = -ENODEV;
		goto close;
	}
	ret = request_irq(virq, (irq_handler_t)handler_addr, IRQF_NO_SUSPEND,
			  "zrn-rtc-alarm-corrected", rtc);
	if (ret)
		goto dispose;
	ret = irq_set_irq_wake(virq, 1);
	if (ret)
		goto free;
	irq_set_affinity(virq, cpumask_of(0));
	pr_info("zrn-rtc-bridge: alarm routed through GIC63, IRQ %u\n", virq);
	return 0;
free:
	free_irq(virq, rtc);
dispose:
	irq_dispose_mapping(virq);
close:
	rtc_class_close(rtc);
	return ret;
}
static void __exit bridge_exit(void)
{
	irq_set_irq_wake(virq, 0);
	free_irq(virq, rtc);
	irq_dispose_mapping(virq);
	rtc_class_close(rtc);
}
module_init(bridge_init);
module_exit(bridge_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Temporary IRQ route correction for pinned RTC prototype");
