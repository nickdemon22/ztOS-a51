// SPDX-License-Identifier: GPL-2.0
/*
 * zrn-led: the Galaxy S9's front notification LED, for a kernel booted
 * without a device-tree node for it.
 *
 * The LED is three channels of the MAX77705's RGB driver (registers 0x30..0x36
 * at the PMIC's I2C address). The MAX77705 core driver already creates a
 * "max77705-rgb" platform device; mainline's leds-max77705 would bind to it
 * but reads its LEDs from the device tree, which ours does not describe. This
 * binds to the same device and registers the three channels as fixed LEDs:
 * red:status, green:status, blue:status. Brightness is the channel's current
 * (0..255), scaled down: at its full current this LED is too bright for
 * anything it is used for, so 255 here is max_percent (60) of what the chip
 * can give. Blinking is done by the chip, so a blinking LED costs the
 * processor nothing; the blink period is shared by the three channels.
 *
 * Breathing. The chip can also ramp the light up and down instead of
 * switching it (LEDRMP: 0..800 ms in 100 ms steps, then 1000..2200 ms in
 * 200 ms steps, for both ways). Each LED has a `ramp` attribute beside its
 * brightness, "UP DOWN" in milliseconds (shared by the three channels, like
 * the period): with a ramp set, the timer trigger breathes instead of
 * blinking.
 *
 *	echo "400 400" > /sys/class/leds/red:status/ramp
 *	echo timer > /sys/class/leds/red:status/trigger
 *	echo 500 > /sys/class/leds/red:status/delay_on
 *	echo 500 > /sys/class/leds/red:status/delay_off
 *
 * is one breath a second: up for 400 ms, lit for 100, down for 400, dark 100.
 *
 * Channel numbers as on the Snapdragon S9 (same board design): 1 red,
 * 2 green, 3 blue. Register handling follows leds-max77705.
 */
#include <linux/i2c.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#define REG_BASE	0x30
#define REG_LEDEN	0x00
#define REG_BRT(ch)	(0x01 + (ch))
/* Not where mainline's leds-max77705 has them (0x35 and 0x36, counted on from
 * the brightness registers): on this chip 0x35 and 0x37 read 0xff and keep
 * nothing, the ramp is at 0x36 and the blink times at 0x38, as in Samsung's
 * own driver. Written to 0x36, blink times set a ramp and no blinking. */
#define REG_LEDRMP	0x06
#define REG_LEDBLNK	0x08
#define EN_SHIFT(ch)	((ch) * 2)
#define EN_CONSTANT	1
#define EN_BLINK	2

static unsigned int max_percent = 60;
module_param(max_percent, uint, 0644);
MODULE_PARM_DESC(max_percent, "the LED's full brightness, per cent of the chip's (default 60)");

struct zrn_led {
	struct led_classdev cdev;
	struct regmap *regmap;
	int channel;
	unsigned int level;	/* the last brightness asked for that was not off */
};

static const struct regmap_config zrn_led_regmap = {
	.reg_base = REG_BASE,
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = REG_LEDBLNK,
};

/* What is written to the chip for a brightness: never more than max_percent
 * of its range, and never nothing for something asked to be on. */
static unsigned int zrn_led_current(unsigned int value)
{
	unsigned int percent = clamp(max_percent, 1u, 100u), scaled = value * percent / 100;

	return value && !scaled ? 1 : scaled;
}

static int zrn_led_set(struct led_classdev *cdev, enum led_brightness value)
{
	struct zrn_led *led = container_of(cdev, struct zrn_led, cdev);
	int ret;

	if (value == LED_OFF)
		return regmap_update_bits(led->regmap, REG_LEDEN, 3 << EN_SHIFT(led->channel), 0);
	led->level = value;
	ret = regmap_write(led->regmap, REG_BRT(led->channel), zrn_led_current(value));
	if (ret)
		return ret;
	return regmap_update_bits(led->regmap, REG_LEDEN, 3 << EN_SHIFT(led->channel),
				  EN_CONSTANT << EN_SHIFT(led->channel));
}

/* The chip's steps: on for 100..400 ms by hundreds, then 500 ms..3.25 s by
 * 250; off for under 500 ms, 500 ms..5 s by 500, then by seconds. The channel
 * is put into the chip's blinking mode at its last brightness (or a modest
 * one if it never had any). */
static int zrn_led_blink(struct led_classdev *cdev, unsigned long *on, unsigned long *off)
{
	struct zrn_led *led = container_of(cdev, struct zrn_led, cdev);
	unsigned int on_v, off_v;
	int ret;

	if (!*on && !*off) {
		*on = 500;
		*off = 1500;
	}
	on_v = *on < 100 ? 0 : *on < 500 ? *on / 100 - 1 : *on < 3250 ? (*on - 500) / 250 + 4 : 15;
	on_v = min(on_v, 15u);
	off_v = !*off ? 0 : *off < 500 ? 1 : *off < 5000 ? *off / 500 :
		*off < 8000 ? (*off - 5000) / 1000 + 10 : min((*off - 8000) / 2000 + 13, 15ul);
	ret = regmap_write(led->regmap, REG_LEDBLNK, on_v << 4 | off_v);
	if (ret)
		return ret;
	if (!led->level)
		led->level = 60;
	ret = regmap_write(led->regmap, REG_BRT(led->channel), zrn_led_current(led->level));
	if (ret)
		return ret;
	return regmap_update_bits(led->regmap, REG_LEDEN, 3 << EN_SHIFT(led->channel),
				  EN_BLINK << EN_SHIFT(led->channel));
}

/* A ramp time in milliseconds as the chip counts it. */
static unsigned int zrn_led_ramp_code(unsigned int ms)
{
	return ms <= 800 ? ms / 100 : min((ms - 800) / 200 + 8, 15u);
}

static ssize_t ramp_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct zrn_led *led = container_of(dev_get_drvdata(dev), struct zrn_led, cdev);
	unsigned int v = 0, up, down;

	regmap_read(led->regmap, REG_LEDRMP, &v);
	up = v >> 4;
	down = v & 15;
	return sysfs_emit(buf, "%u %u\n", up <= 8 ? up * 100 : 800 + (up - 8) * 200,
			  down <= 8 ? down * 100 : 800 + (down - 8) * 200);
}

static ssize_t ramp_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	struct zrn_led *led = container_of(dev_get_drvdata(dev), struct zrn_led, cdev);
	unsigned int up = 0, down = 0;
	int ret;

	if (sscanf(buf, "%u %u", &up, &down) < 1)
		return -EINVAL;
	if (sscanf(buf, "%u %u", &up, &down) == 1)
		down = up;
	ret = regmap_write(led->regmap, REG_LEDRMP, zrn_led_ramp_code(up) << 4 | zrn_led_ramp_code(down));
	return ret ? ret : count;
}
static DEVICE_ATTR_RW(ramp);

static struct attribute *zrn_led_attrs[] = {
	&dev_attr_ramp.attr,
	NULL,
};
ATTRIBUTE_GROUPS(zrn_led);

static int zrn_led_probe(struct platform_device *pdev)
{
	static const char *const names[] = { "red:status", "green:status", "blue:status" };
	struct device *dev = &pdev->dev;
	struct i2c_client *i2c = to_i2c_client(dev->parent);
	struct regmap *regmap;
	int ret;

	regmap = devm_regmap_init_i2c(i2c, &zrn_led_regmap);
	if (IS_ERR(regmap))
		return dev_err_probe(dev, PTR_ERR(regmap), "no regmap\n");
	/* all off, no blinking, whatever the last system left */
	regmap_write(regmap, REG_LEDEN, 0);
	regmap_write(regmap, REG_LEDBLNK, 0);
	regmap_write(regmap, REG_LEDRMP, 0);

	for (int i = 0; i < 3; i++) {
		struct zrn_led *led = devm_kzalloc(dev, sizeof(*led), GFP_KERNEL);

		if (!led)
			return -ENOMEM;
		led->regmap = regmap;
		led->channel = i + 1;
		led->cdev.name = names[i];
		led->cdev.max_brightness = 255;
		led->cdev.brightness_set_blocking = zrn_led_set;
		led->cdev.blink_set = zrn_led_blink;
		led->cdev.groups = zrn_led_groups;
		ret = devm_led_classdev_register(dev, &led->cdev);
		if (ret)
			return ret;
	}
	return 0;
}

static void zrn_led_remove(struct platform_device *pdev)
{
	struct regmap *regmap = dev_get_regmap(&pdev->dev, NULL);

	if (regmap)
		regmap_write(regmap, REG_LEDEN, 0);
}

static const struct platform_device_id zrn_led_ids[] = {
	{ "max77705-rgb" },
	{ }
};
MODULE_DEVICE_TABLE(platform, zrn_led_ids);

static struct platform_driver zrn_led_driver = {
	.driver = { .name = "zrn-led" },
	.probe = zrn_led_probe,
	.remove = zrn_led_remove,
	.id_table = zrn_led_ids,
};
module_platform_driver(zrn_led_driver);

MODULE_DESCRIPTION("Galaxy S9 front RGB LED (MAX77705) without a device-tree node");
MODULE_LICENSE("GPL");
