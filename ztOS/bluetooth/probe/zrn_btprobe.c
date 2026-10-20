// SPDX-License-Identifier: GPL-2.0
/*
 * zrn_btprobe: read-only look at the PCAL6524 GPIO expander that switches the
 * BCM4361's Bluetooth on (BT_EN is its pin 7), before any driver touches it.
 *
 * The expander is at 0x22 on the stock tree's hsi2c@10930000, which our
 * kernel does not bring up; as zrn-flash does for the S2MPB02, a bit-banged
 * bus (i2c-gpio) on the pins the stock tree names for it: gpp6-2 SCL,
 * gpp6-3 SDA. Nothing is written to the expander: its input, output and
 * direction registers are printed, and the module refuses to stay loaded.
 *
 * PCAL6524 registers (auto-increment off, one byte each): input 0x00-0x02,
 * output 0x04-0x06, polarity 0x08-0x0a, configuration 0x0c-0x0e (1 = input).
 * Pin 7 is port 0, bit 7.
 */
#include <linux/gpio/machine.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/platform_data/i2c-gpio.h>
#include <linux/platform_device.h>

#define BUS	96
static struct gpiod_lookup_table gpios = {
	.dev_id = "i2c-gpio.96",
	.table = {
		GPIO_LOOKUP_IDX("gpp6", 3, NULL, 0, GPIO_ACTIVE_HIGH | GPIO_OPEN_DRAIN),
		GPIO_LOOKUP_IDX("gpp6", 2, NULL, 1, GPIO_ACTIVE_HIGH | GPIO_OPEN_DRAIN),
		{ }
	},
};
static struct i2c_gpio_platform_data pdata = { .udelay = 5 };

static int __init probe_init(void)
{
	struct platform_device *pdev;
	struct i2c_adapter *bus;
	struct i2c_client *c;
	int r[15];

	gpiod_add_lookup_table(&gpios);
	pdev = platform_device_register_data(NULL, "i2c-gpio", BUS, &pdata, sizeof(pdata));
	if (IS_ERR(pdev))
		goto out_table;
	bus = i2c_get_adapter(BUS);
	if (!bus)
		goto out_pdev;
	c = i2c_new_dummy_device(bus, 0x22);
	i2c_put_adapter(bus);
	if (IS_ERR(c))
		goto out_pdev;
	for (int i = 0; i < 15; i++)
		r[i] = i2c_smbus_read_byte_data(c, i);
	if (r[0] < 0) {
		pr_info("zrn_btprobe: expander does not answer at 0x22: %d\n", r[0]);
	} else {
		pr_info("zrn_btprobe: input %02x %02x %02x output %02x %02x %02x config %02x %02x %02x\n",
			r[0], r[1], r[2], r[4], r[5], r[6], r[12], r[13], r[14]);
		pr_info("zrn_btprobe: BT_EN (pin 7): %s, driven %s, reads %s\n",
			r[12] & 0x80 ? "input" : "output", r[4] & 0x80 ? "high" : "low",
			r[0] & 0x80 ? "high" : "low");
	}
	i2c_unregister_device(c);
out_pdev:
	platform_device_unregister(pdev);
out_table:
	gpiod_remove_lookup_table(&gpios);
	return -EAGAIN;	/* done: nothing to keep loaded */
}
module_init(probe_init);
MODULE_SOFTDEP("pre: i2c-gpio");
MODULE_LICENSE("GPL");
