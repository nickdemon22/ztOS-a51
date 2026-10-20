// SPDX-License-Identifier: GPL-2.0-only
/* Exynos9810 always-on RTC, using the stock exynos8-rtc register layout. */
#include <linux/bcd.h>
#include <linux/cpumask.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/irqdomain.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/rtc.h>
#include <linux/suspend.h>

static void __iomem *registers;
static struct platform_device *device;
static struct rtc_device *rtc;
static unsigned int alarm_irq;
static unsigned long alarm_fires;
static bool registration_complete;
module_param(alarm_fires, ulong, 0444);
static u16 old_control;
static u8 old_alarm[7];
static DEFINE_SPINLOCK(alarm_lock);
static const unsigned int time_offsets[] = { 0x70, 0x74, 0x78, 0x7c, 0x84, 0x88 };
static const unsigned int alarm_offsets[] = { 0x54, 0x58, 0x5c, 0x60, 0x64, 0x68 };

static void decode_time(struct rtc_time *time, const u8 *values)
{
	time->tm_sec = bcd2bin(values[0]);
	time->tm_min = bcd2bin(values[1]);
	time->tm_hour = bcd2bin(values[2]);
	time->tm_mday = bcd2bin(values[3]);
	time->tm_mon = bcd2bin(values[4]) - 1;
	time->tm_year = bcd2bin(values[5]) + 100;
}

static int read_calendar(struct rtc_time *time)
{
	u8 values[6];
	unsigned int retry, i;
	for (retry = 0; retry < 3; retry++) {
		for (i = 0; i < ARRAY_SIZE(values); i++)
			values[i] = readb(registers + time_offsets[i]);
		if (values[0] == readb(registers + 0x70)) {
			for (i = 0; i < ARRAY_SIZE(values); i++)
				if ((values[i] & 15) > 9 || (values[i] >> 4) > 9)
					return -EINVAL;
			decode_time(time, values);
			return rtc_valid_tm(time);
		}
	}
	return -EAGAIN;
}

static int read_time(struct device *unused, struct rtc_time *time)
{
	/* This module is brought up after the system clock is established.
	 * Suppress RTC_HCTOSYS during synchronous class registration. */
	if (!registration_complete)
		return -EAGAIN;
	return read_calendar(time);
}

static int set_time(struct device *unused, struct rtc_time *time)
{
	u8 values[6];
	unsigned long flags;
	unsigned int i;
	if (rtc_valid_tm(time) || time->tm_year < 100 || time->tm_year > 199)
		return -EINVAL;
	values[0] = bin2bcd(time->tm_sec);
	values[1] = bin2bcd(time->tm_min);
	values[2] = bin2bcd(time->tm_hour);
	values[3] = bin2bcd(time->tm_mday);
	values[4] = bin2bcd(time->tm_mon + 1);
	values[5] = bin2bcd(time->tm_year - 100);
	spin_lock_irqsave(&alarm_lock, flags);
	if (readb(registers + 0x50) & BIT(6)) {
		spin_unlock_irqrestore(&alarm_lock, flags);
		return -EBUSY;
	}
	for (i = 0; i < ARRAY_SIZE(values); i++)
		writeb(values[i], registers + time_offsets[i]);
	spin_unlock_irqrestore(&alarm_lock, flags);
	return 0;
}

static int read_alarm(struct device *unused, struct rtc_wkalrm *alarm)
{
	u8 values[6];
	unsigned long flags;
	unsigned int i;
	spin_lock_irqsave(&alarm_lock, flags);
	for (i = 0; i < ARRAY_SIZE(values); i++)
		values[i] = readb(registers + alarm_offsets[i]);
	alarm->enabled = !!(readb(registers + 0x50) & BIT(6));
	alarm->pending = !!(readl(registers + 0x30) & BIT(1));
	spin_unlock_irqrestore(&alarm_lock, flags);
	decode_time(&alarm->time, values);
	return 0;
}

static int set_alarm(struct device *unused, struct rtc_wkalrm *alarm)
{
	struct rtc_time *time = &alarm->time;
	u8 values[6];
	unsigned long flags;
	unsigned int i;
	int ret = rtc_valid_tm(time);
	if (ret || time->tm_year < 100 || time->tm_year > 199)
		return -EINVAL;
	values[0] = bin2bcd(time->tm_sec);
	values[1] = bin2bcd(time->tm_min);
	values[2] = bin2bcd(time->tm_hour);
	values[3] = bin2bcd(time->tm_mday);
	values[4] = bin2bcd(time->tm_mon + 1);
	values[5] = bin2bcd(time->tm_year - 100);
	spin_lock_irqsave(&alarm_lock, flags);
	writeb(0, registers + 0x50);
	for (i = 0; i < ARRAY_SIZE(values); i++)
		writeb(values[i], registers + alarm_offsets[i]);
	writel(BIT(1), registers + 0x30);
	writeb(alarm->enabled ? 0x7f : 0x3f, registers + 0x50);
	spin_unlock_irqrestore(&alarm_lock, flags);
	return 0;
}

static int enable_alarm(struct device *unused, unsigned int enable)
{
	unsigned long flags;
	u8 value;
	spin_lock_irqsave(&alarm_lock, flags);
	value = readb(registers + 0x50) & ~BIT(6);
	writeb(value | (enable ? BIT(6) : 0), registers + 0x50);
	spin_unlock_irqrestore(&alarm_lock, flags);
	return 0;
}

static const struct rtc_class_ops rtc_ops = {
	.read_time = read_time, .set_time = set_time, .read_alarm = read_alarm,
	.set_alarm = set_alarm, .alarm_irq_enable = enable_alarm,
};

static irqreturn_t alarm_interrupt(int irq, void *unused)
{
	unsigned long flags;
	if (!(readl(registers + 0x30) & BIT(1)))
		return IRQ_NONE;
	spin_lock_irqsave(&alarm_lock, flags);
	writeb(readb(registers + 0x50) & ~BIT(6), registers + 0x50);
	writel(BIT(1), registers + 0x30);
	alarm_fires++;
	spin_unlock_irqrestore(&alarm_lock, flags);
	rtc_update_irq(rtc, 1, RTC_IRQF | RTC_AF);
	pm_system_wakeup();
	return IRQ_HANDLED;
}

static int __init exynos_rtc_init(void)
{
	struct device_node *gic;
	/* Stock platform_get_irq(pdev, 0): SPI31 alarm; SPI32 is the tick. */
	struct irq_fwspec spec = { .param_count = 3, .param = { 0, 31, IRQ_TYPE_LEVEL_HIGH } };
	struct rtc_time time;
	int ret, i;
	if (!of_machine_is_compatible("samsung,exynos9810"))
		return -ENODEV;
	registers = ioremap(0x141e0000, 0x1000);
	if (!registers)
		return -ENOMEM;
	/* Refuse to take over an alarm owned by firmware or another driver. */
	if ((readb(registers + 0x50) & BIT(6)) ||
	    (readl(registers + 0x30) & BIT(1))) {
		ret = -EBUSY;
		goto unmap;
	}
	ret = read_calendar(&time);
	if (ret && ret != -EINVAL)
		goto unmap;
	if (ret)
		pr_warn("zrn-rtc: calendar needs initialization through RTC_SET_TIME\n");
	old_control = readw(registers + 0x40);
	old_alarm[0] = readb(registers + 0x50);
	for (i = 0; i < ARRAY_SIZE(alarm_offsets); i++)
		old_alarm[i + 1] = readb(registers + alarm_offsets[i]);
	gic = of_find_compatible_node(NULL, NULL, "arm,gic-400");
	if (!gic) {
		ret = -ENODEV;
		goto unmap;
	}
	spec.fwnode = of_fwnode_handle(gic);
	alarm_irq = irq_create_fwspec_mapping(&spec);
	of_node_put(gic);
	if (!alarm_irq) {
		ret = -ENODEV;
		goto unmap;
	}
	device = platform_device_register_simple("zrn-exynos9810-rtc", -1, NULL, 0);
	if (IS_ERR(device)) {
		ret = PTR_ERR(device);
		goto dispose;
	}
	device_init_wakeup(&device->dev, true);
	rtc = devm_rtc_allocate_device(&device->dev);
	if (IS_ERR(rtc)) {
		ret = PTR_ERR(rtc);
		goto unregister;
	}
	rtc->ops = &rtc_ops;
	rtc->owner = THIS_MODULE;
	rtc->range_min = RTC_TIMESTAMP_BEGIN_2000;
	rtc->range_max = RTC_TIMESTAMP_END_2099;
	ret = request_irq(alarm_irq, alarm_interrupt, IRQF_NO_SUSPEND,
			  "zrn-rtc-alarm", rtc);
	if (ret)
		goto unregister;
	ret = irq_set_irq_wake(alarm_irq, 1);
	if (ret)
		goto free;
	irq_set_affinity(alarm_irq, cpumask_of(0));
	/* Enable register access; leave the running calendar and tick intact. */
	writew(old_control | BIT(0), registers + 0x40);
	ret = devm_rtc_register_device(rtc);
	if (!ret) {
		registration_complete = true;
		pr_info("zrn-rtc: running calendar, alarm on IRQ %u (GIC 63)\n", alarm_irq);
		return 0;
	}
	writew(old_control, registers + 0x40);
	irq_set_irq_wake(alarm_irq, 0);
free:
	free_irq(alarm_irq, rtc);
unregister:
	device_init_wakeup(&device->dev, false);
	platform_device_unregister(device);
dispose:
	irq_dispose_mapping(alarm_irq);
unmap:
	iounmap(registers);
	return ret;
}

static void __exit exynos_rtc_exit(void)
{
	int i;
	enable_alarm(NULL, 0);
	irq_set_irq_wake(alarm_irq, 0);
	free_irq(alarm_irq, rtc);
	writeb(0, registers + 0x50);
	writel(BIT(1), registers + 0x30);
	for (i = 0; i < ARRAY_SIZE(alarm_offsets); i++)
		writeb(old_alarm[i + 1], registers + alarm_offsets[i]);
	writeb(old_alarm[0], registers + 0x50);
	writew(old_control, registers + 0x40);
	device_init_wakeup(&device->dev, false);
	platform_device_unregister(device);
	irq_dispose_mapping(alarm_irq);
	iounmap(registers);
}
module_init(exynos_rtc_init);
module_exit(exynos_rtc_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Exynos9810 running RTC calendar and one-shot wake alarm");
