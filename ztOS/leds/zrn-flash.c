// SPDX-License-Identifier: GPL-2.0
/*
 * zrn-flash: the Galaxy S9's rear camera flash LED, used as a torch.
 *
 * The LED hangs off the S2MPB02 camera PMIC (I2C 0x59, on the stock tree's
 * hsi2c@10920000). That controller is not brought up by our kernel, so, as
 * snd-zrn-abox does for the amplifier, this registers a bit-banged bus
 * (i2c-gpio) on the same two pins the stock tree names for it: gpp6-0 SCL,
 * gpp6-1 SDA.
 *
 * Registers, from Samsung's leds-s2mpb02.c and s2mpb02-private.h:
 *   0x00 ID
 *   0x12 FLED_CTRL1  bits 7:6 = 11 torch on, 10 flash on, 00 off;
 *                    bit 3 = low-voltage mode (Samsung clears it)
 *   0x14 FLED_CUR1   bits 7:4 flash current, bits 3:0 torch current
 * The LED is switched over I2C alone; the torch and flash GPIOs (gpg2-4,
 * gpg3-5) stay low, as Samsung's I2C path leaves them.
 *
 * One LED class device, white:torch, brightness 0..15 (the torch current
 * code). Samsung's torch table tops out at 9 for the user torch; 15 is the
 * chip's limit for continuous torch, not a flash.
 *
 *	echo 9 > /sys/class/leds/white:torch/brightness
 *	echo 0 > /sys/class/leds/white:torch/brightness
 */
#include <linux/gpio/machine.h>
#include <linux/i2c.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/platform_data/i2c-gpio.h>
#include <linux/platform_device.h>

#define FLASH_BUS	97
#define S2MPB02_ADDR	0x59
#define REG_ID		0x00
#define REG_FLED_CTRL1	0x12
#define REG_FLED_CUR1	0x14
#define CTRL1_MODE	0xc0
#define CTRL1_TORCH	0xc0
#define CTRL1_LV_EN	0x08
#define CUR1_TORCH	0x0f

static struct gpiod_lookup_table flash_gpios = {
	.dev_id = "i2c-gpio.97",
	.table = {
		GPIO_LOOKUP_IDX("gpp6", 1, NULL, 0, GPIO_ACTIVE_HIGH | GPIO_OPEN_DRAIN),
		GPIO_LOOKUP_IDX("gpp6", 0, NULL, 1, GPIO_ACTIVE_HIGH | GPIO_OPEN_DRAIN),
		{ }
	},
};
static struct i2c_gpio_platform_data flash_i2c = { .udelay = 5 };	/* about 100 kHz */

static struct platform_device *i2c_pdev;
static struct i2c_client *pmic;
static struct led_classdev torch;
static DEFINE_MUTEX(lock);

static int update(u8 reg, u8 mask, u8 value)
{
	int v = i2c_smbus_read_byte_data(pmic, reg);

	if (v < 0)
		return v;
	return i2c_smbus_write_byte_data(pmic, reg, (v & ~mask) | (value & mask));
}

static int torch_set(struct led_classdev *cdev, enum led_brightness b)
{
	int ret;

	mutex_lock(&lock);
	if (b) {
		ret = update(REG_FLED_CUR1, CUR1_TORCH, b);
		if (!ret)
			ret = update(REG_FLED_CTRL1, CTRL1_MODE, CTRL1_TORCH);
	} else {
		ret = update(REG_FLED_CTRL1, CTRL1_MODE, 0);
	}
	mutex_unlock(&lock);
	return ret;
}

static int __init zrn_flash_init(void)
{
	struct i2c_adapter *bus;
	int id, ret;

	gpiod_add_lookup_table(&flash_gpios);
	i2c_pdev = platform_device_register_data(NULL, "i2c-gpio", FLASH_BUS,
						 &flash_i2c, sizeof(flash_i2c));
	if (IS_ERR(i2c_pdev)) {
		ret = PTR_ERR(i2c_pdev);
		goto err_table;
	}
	bus = i2c_get_adapter(FLASH_BUS);
	if (!bus) {
		pr_err("zrn-flash: no bus %d (is i2c-gpio loaded?)\n", FLASH_BUS);
		ret = -EPROBE_DEFER;
		goto err_pdev;
	}
	pmic = i2c_new_dummy_device(bus, S2MPB02_ADDR);
	i2c_put_adapter(bus);
	if (IS_ERR(pmic)) {
		ret = PTR_ERR(pmic);
		goto err_pdev;
	}
	id = i2c_smbus_read_byte_data(pmic, REG_ID);
	if (id < 0) {
		pr_err("zrn-flash: S2MPB02 does not answer: %d\n", id);
		ret = id;
		goto err_client;
	}
	pr_info("zrn-flash: S2MPB02 id 0x%02x\n", id);
	/* off, normal-voltage mode, as Samsung's setup leaves it */
	ret = update(REG_FLED_CTRL1, CTRL1_MODE | CTRL1_LV_EN, 0);
	if (ret)
		goto err_client;

	torch.name = "white:torch";
	torch.max_brightness = CUR1_TORCH;
	torch.brightness_set_blocking = torch_set;
	ret = led_classdev_register(&i2c_pdev->dev, &torch);
	if (ret)
		goto err_client;
	return 0;

err_client:
	i2c_unregister_device(pmic);
err_pdev:
	platform_device_unregister(i2c_pdev);
err_table:
	gpiod_remove_lookup_table(&flash_gpios);
	return ret;
}

static void __exit zrn_flash_exit(void)
{
	led_classdev_unregister(&torch);
	update(REG_FLED_CTRL1, CTRL1_MODE, 0);
	i2c_unregister_device(pmic);
	platform_device_unregister(i2c_pdev);
	gpiod_remove_lookup_table(&flash_gpios);
}

module_init(zrn_flash_init);
module_exit(zrn_flash_exit);
MODULE_SOFTDEP("pre: i2c-gpio");
MODULE_DESCRIPTION("Galaxy S9 rear flash LED as a torch (S2MPB02 over bit-banged I2C)");
MODULE_LICENSE("GPL");
