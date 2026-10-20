// SPDX-License-Identifier: GPL-2.0
/*
 * zrn-bt: the board side of the Galaxy S9's Bluetooth, for a kernel whose
 * device tree does not describe it (see ../README.md).
 *
 * The BCM4361's Bluetooth sits on UART_BT (0x10840000, a plain Samsung UART
 * in PERIC1, 4-wire on gpq0-0..3). This module:
 *
 *   1. opens its clock gates in CMU_PERIC1, takes the UART out of its USI
 *      reset (USI_CON +0xc4 = 0, USI_OPTION +0xc8 = CLKREQ_ON), divider 1:1
 *      (200 MHz);
 *   2. puts gpq0-0..3 in their UART function, pulled up as the stock tree has
 *      them;
 *   3. registers the UART with samsung_tty as an "exynos850-uart" platform
 *      device, with a 200 MHz fixed clock and its interrupt (SPI 422): it
 *      becomes /dev/ttySAC0;
 *   4. powers the chip's Bluetooth: BT_EN is pin 7 of the PCAL6524 expander
 *      (I2C 0x22, reached over a bit-banged bus on gpp6-2/gpp6-3, as
 *      zrn_btprobe did), and BT_WAKE (gpg2-0) is held high.
 *
 * The host stack then attaches to the UART from userspace:
 *
 *	btattach -B /dev/ttySAC0 -P bcm -S 115200
 *
 * which loads brcm/BCM4361B2.hcd through btbcm. Unloading powers it off.
 */
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/clkdev.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/machine.h>
#include <linux/i2c.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/irqdomain.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/platform_data/i2c-gpio.h>
#include <linux/platform_device.h>

#define CMU_PERIC1	0x10800000
#define DIV_UART_BT	0x1814
#define UART_BT		0x10840000
#define PINCTRL_ALIVE	0x14050000
#define GPQ0		0x0a0		/* CON +0, DAT +4, PUD +8 */
#define UART_SPI	0x1a6
#define UART_HZ		200000000
#define EXP_BUS		96
#define EXP_ADDR	0x22
#define EXP_OUT0	0x04
#define EXP_CFG0	0x0c
#define BT_EN		BIT(7)

static const u32 gates[] = { 0x20c4, 0x20c8, 0x201c, 0x2034 };

static struct gpiod_lookup_table bus_gpios = {
	.dev_id = "i2c-gpio.96",
	.table = {
		GPIO_LOOKUP_IDX("gpp6", 3, NULL, 0, GPIO_ACTIVE_HIGH | GPIO_OPEN_DRAIN),
		GPIO_LOOKUP_IDX("gpp6", 2, NULL, 1, GPIO_ACTIVE_HIGH | GPIO_OPEN_DRAIN),
		{ }
	},
};
static struct gpiod_lookup_table wake_gpios = {
	.dev_id = "zrn-bt",
	.table = {
		GPIO_LOOKUP("gpg2", 0, "bt-wake", GPIO_ACTIVE_HIGH),
		{ }
	},
};
static struct i2c_gpio_platform_data bus_pdata = { .udelay = 5 };

static struct platform_device *bus_pdev, *uart_pdev, *self;
static struct i2c_client *exp;
static struct clk_hw *uart_clk;
static struct clk_lookup *cl_uart, *cl_baud;
static struct gpio_desc *bt_wake;

static void clocks_on(void)
{
	void __iomem *cmu = ioremap(CMU_PERIC1, 0x4000);

	if (!cmu)
		return;
	for (int i = 0; i < ARRAY_SIZE(gates); i++)
		writel(readl(cmu + gates[i]) | 0x300000, cmu + gates[i]);	/* MANUAL | CG_VAL */
	/* CLKCMU_PERIC1_IP arrives at 200 MHz (SHARED2 800 / 2 / 2): divider
	 * ratio 0, as the bootloader leaves it */
	writel(readl(cmu + DIV_UART_BT) & ~0xf, cmu + DIV_UART_BT);
	pr_info("zrn-bt: UART_BT clock open, divider %#x\n", readl(cmu + DIV_UART_BT));
	iounmap(cmu);
	/* the UART sits behind a USI v2 wrapper (stock: samsung,usi-serial-v2):
	 * held in reset until its USI_CON is cleared, as exynos-usi does */
	cmu = ioremap(UART_BT, 0x100);
	if (cmu) {
		writel(0x2, cmu + 0xc8);	/* USI_OPTION: CLKREQ_ON */
		writel(0x0, cmu + 0xc4);	/* USI_CON: reset off */
		iounmap(cmu);
	}
}

static void pins_uart(void)
{
	void __iomem *p = ioremap(PINCTRL_ALIVE, 0x1000);
	u32 v;

	if (!p)
		return;
	v = readl(p + GPQ0);
	writel((v & ~0xffff) | 0x2222, p + GPQ0);		/* function 2 on 0..3 */
	v = readl(p + GPQ0 + 8);
	writel((v & ~0xffff) | 0x3333, p + GPQ0 + 8);		/* pull-up, as stock */
	pr_info("zrn-bt: gpq0 con %#x pud %#x\n", readl(p + GPQ0), readl(p + GPQ0 + 8));
	iounmap(p);
}

static int uart_register(void)
{
	struct device_node *gic = of_find_compatible_node(NULL, NULL, "arm,gic-400");
	struct irq_fwspec spec = { .param_count = 3, .param = { 0, UART_SPI, IRQ_TYPE_LEVEL_HIGH } };
	struct resource res[2] = {
		DEFINE_RES_MEM(UART_BT, 0x100),
	};
	int irq;

	if (!gic)
		return -ENODEV;
	spec.fwnode = of_fwnode_handle(gic);
	irq = irq_create_fwspec_mapping(&spec);
	of_node_put(gic);
	if (irq <= 0)
		return -EINVAL;
	res[1] = (struct resource)DEFINE_RES_IRQ(irq);

	uart_clk = clk_hw_register_fixed_rate(NULL, "zrn-uart-bt", NULL, 0, UART_HZ);
	if (IS_ERR(uart_clk))
		return PTR_ERR(uart_clk);
	cl_uart = clkdev_hw_create(uart_clk, "uart", "exynos850-uart.0");
	cl_baud = clkdev_hw_create(uart_clk, "clk_uart_baud0", "exynos850-uart.0");
	uart_pdev = platform_device_register_simple("exynos850-uart", 0, res, 2);
	if (IS_ERR(uart_pdev))
		return PTR_ERR(uart_pdev);
	pr_info("zrn-bt: UART_BT registered (irq %d), as ttySAC0 if samsung_tty takes it\n", irq);
	return 0;
}

static int exp_update(u8 reg, u8 mask, u8 val)
{
	int v = i2c_smbus_read_byte_data(exp, reg);

	if (v < 0)
		return v;
	return i2c_smbus_write_byte_data(exp, reg, (v & ~mask) | (val & mask));
}

static int bt_power(bool on)
{
	int ret = exp_update(EXP_CFG0, BT_EN, 0);	/* an output */

	if (!ret)
		ret = exp_update(EXP_OUT0, BT_EN, on ? BT_EN : 0);
	pr_info("zrn-bt: BT_EN %s (%d)\n", on ? "high" : "low", ret);
	return ret;
}

static int __init zrn_bt_init(void)
{
	struct i2c_adapter *bus;
	int ret;

	clocks_on();
	pins_uart();

	gpiod_add_lookup_table(&bus_gpios);
	bus_pdev = platform_device_register_data(NULL, "i2c-gpio", EXP_BUS, &bus_pdata, sizeof(bus_pdata));
	if (IS_ERR(bus_pdev)) {
		ret = PTR_ERR(bus_pdev);
		goto err_table;
	}
	bus = i2c_get_adapter(EXP_BUS);
	if (!bus) {
		ret = -EPROBE_DEFER;
		goto err_bus;
	}
	exp = i2c_new_dummy_device(bus, EXP_ADDR);
	i2c_put_adapter(bus);
	if (IS_ERR(exp)) {
		ret = PTR_ERR(exp);
		goto err_bus;
	}

	gpiod_add_lookup_table(&wake_gpios);
	self = platform_device_register_simple("zrn-bt", -1, NULL, 0);
	if (!IS_ERR(self)) {
		bt_wake = gpiod_get(&self->dev, "bt-wake", GPIOD_OUT_HIGH);
		if (IS_ERR(bt_wake)) {
			pr_warn("zrn-bt: no BT_WAKE: %ld\n", PTR_ERR(bt_wake));
			bt_wake = NULL;
		}
	}

	ret = bt_power(false);	/* a clean reset of its Bluetooth side */
	if (ret)
		goto err_exp;
	msleep(30);
	ret = bt_power(true);
	if (ret)
		goto err_exp;
	msleep(100);

	ret = uart_register();
	if (ret)
		goto err_power;
	return 0;

err_power:
	bt_power(false);
err_exp:
	if (bt_wake)
		gpiod_put(bt_wake);
	if (!IS_ERR_OR_NULL(self))
		platform_device_unregister(self);
	gpiod_remove_lookup_table(&wake_gpios);
	i2c_unregister_device(exp);
err_bus:
	platform_device_unregister(bus_pdev);
err_table:
	gpiod_remove_lookup_table(&bus_gpios);
	return ret;
}

static void __exit zrn_bt_exit(void)
{
	platform_device_unregister(uart_pdev);
	clkdev_drop(cl_baud);
	clkdev_drop(cl_uart);
	clk_hw_unregister_fixed_rate(uart_clk);
	bt_power(false);
	if (bt_wake)
		gpiod_put(bt_wake);
	platform_device_unregister(self);
	gpiod_remove_lookup_table(&wake_gpios);
	i2c_unregister_device(exp);
	platform_device_unregister(bus_pdev);
	gpiod_remove_lookup_table(&bus_gpios);
}

module_init(zrn_bt_init);
module_exit(zrn_bt_exit);
MODULE_SOFTDEP("pre: i2c-gpio");
MODULE_DESCRIPTION("Galaxy S9 Bluetooth board glue: UART_BT, BT_EN, BT_WAKE");
MODULE_LICENSE("GPL");
