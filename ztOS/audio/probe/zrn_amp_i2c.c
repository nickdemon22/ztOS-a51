// SPDX-License-Identifier: GPL-2.0
/*
 * The speaker amplifier (MAX98512, address 0x39) hangs off an I2C controller
 * the mainline device tree for this phone does not describe (hsi2c@108c0000,
 * pins gpp4-4 and gpp4-5). Rather than a new device tree and boot image, this
 * registers a bit-banged bus on those two pins, so that the amplifier can be
 * reached from /dev/i2c-N like any other chip.
 *
 *	insmod i2c-algo-bit.ko; insmod i2c-gpio.ko; insmod zrn_amp_i2c.ko [swap=1]
 */
#include <linux/gpio/machine.h>
#include <linux/module.h>
#include <linux/platform_data/i2c-gpio.h>
#include <linux/platform_device.h>

static bool swap;
module_param(swap, bool, 0444);
MODULE_PARM_DESC(swap, "data on gpp4-5 and clock on gpp4-4 instead");

static struct gpiod_lookup_table lookup = {
	.dev_id = "i2c-gpio.98",
	.table = {
		GPIO_LOOKUP_IDX("gpp4", 4, NULL, 0, GPIO_ACTIVE_HIGH | GPIO_OPEN_DRAIN),
		GPIO_LOOKUP_IDX("gpp4", 5, NULL, 1, GPIO_ACTIVE_HIGH | GPIO_OPEN_DRAIN),
		{ }
	},
};

static struct i2c_gpio_platform_data pdata = { .udelay = 5 };	/* about 100 kHz */
static struct platform_device *pdev;

static int __init zrn_amp_i2c_init(void)
{
	if (swap) {
		lookup.table[0].chip_hwnum = 5;
		lookup.table[1].chip_hwnum = 4;
	}
	gpiod_add_lookup_table(&lookup);
	pdev = platform_device_register_data(NULL, "i2c-gpio", 98, &pdata, sizeof(pdata));
	if (IS_ERR(pdev)) {
		gpiod_remove_lookup_table(&lookup);
		return PTR_ERR(pdev);
	}
	return 0;
}

static void __exit zrn_amp_i2c_exit(void)
{
	platform_device_unregister(pdev);
	gpiod_remove_lookup_table(&lookup);
}

module_init(zrn_amp_i2c_init);
module_exit(zrn_amp_i2c_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("A bit-banged I2C bus for the Galaxy S9's speaker amplifier");
