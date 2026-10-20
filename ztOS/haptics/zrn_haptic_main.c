// SPDX-License-Identifier: GPL-2.0
/*
 * The Galaxy S9's vibration motor under mainline, as a rumble input device.
 *
 * The motor is a linear resonant actuator driven by the MAX77705's motor
 * driver, which takes a PWM from the SoC and divides it by 128: 20.48 kHz in,
 * the motor's 160 Hz out. The power chip's core driver (MFD_MAX77705) already
 * makes a "max77705-haptic" platform device; mainline's own driver for it
 * (max77693-haptic) wants a PWM and a regulator from a device tree that
 * describes neither here (there is no PWM controller in ours), so this one
 * does the little there is to do by itself, from the stock tree's values
 * (stock/twrp-live.dts: haptic, vib-pwm, motor-boost-en):
 *
 *  - the PWM is the SoC's timer 1 (0x10510000), out on gpp3-1 (function 2);
 *    its clock is the 26 MHz oscillator (the boot loader leaves a prescaler
 *    of 26 there), so 1270 counts are the stock period of 48828 ns;
 *  - the motor's supply is a boost switched on by gpg1-7;
 *  - MCONFIG (0x10) = 0x82: LRA, PWM divided by 128; bit 6 runs the motor;
 *    MAINCTRL1 (0x02) bit 7 is the driver's bias.
 *
 * The stock duty is 70 % of the period; a rumble's strength scales it.
 */
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/workqueue.h>

#define PWM_BASE	0x10510000
#define TCFG0		0x00
#define TCFG1		0x04
#define TCON		0x08
#define TCNTB1		0x18
#define TCMPB1		0x1c
#define TCON_T1_START	BIT(8)
#define TCON_T1_MANUAL	BIT(9)
#define TCON_T1_INVERT	BIT(10)
#define TCON_T1_RELOAD	BIT(11)
#define PERIOD_COUNTS	1270	/* 26 MHz / 1270 = 20.47 kHz; / 128 = 160 Hz */
#define DUTY_PERCENT	95

#define PINS_BASE	0x10430000
#define GPP3		0x060
#define GPG1		0x0a0
#define PIN_CON		0x0
#define PIN_DAT		0x4
#define PIN_PUD		0x8

#define REG_MAINCTRL1	0x02
#define MAINCTRL1_BIASEN BIT(7)
#define REG_MCONFIG	0x10
#define MCONFIG_LRA_DIV128 0x82
#define MCONFIG_MEN	BIT(6)

struct zrn_haptic {
	struct device *dev;
	struct regmap *regmap;
	void __iomem *pwm, *pins;
	struct input_dev *input;
	struct work_struct work;
	unsigned int magnitude;		/* 0 stops it; else up to 0xffff */
	bool running;
};

static const struct regmap_config zrn_haptic_regmap = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = 0xff,
};

static void zrn_haptic_pwm(struct zrn_haptic *h, unsigned int magnitude)
{
	u32 tcon = readl(h->pwm + TCON) & ~(TCON_T1_START | TCON_T1_MANUAL | TCON_T1_RELOAD);

	if (!magnitude) {
		writel(tcon | TCON_T1_INVERT, h->pwm + TCON);	/* stopped, the output low */
		return;
	}
	/* as pwm-samsung does it: the count, and where the output changes; with
	 * the inverter on, the time below the compare value is the high time */
	/* The chip divides this PWM by 128 for the motor's 160 Hz and takes its
	 * strength from how far the duty is from one half: at 50 % the motor is
	 * still, and DUTY_PERCENT is stock's full strength. (Scaling the duty
	 * from nothing, as this first did, gave the same strength at every
	 * setting: 35 % is as far from a half as 65 %.) */
	u32 half = PERIOD_COUNTS / 2;
	u32 duty = half + (PERIOD_COUNTS * DUTY_PERCENT / 100 - half) * (magnitude >> 4) / 0xfff;
	writel(readl(h->pwm + TCFG0) & ~0xffU, h->pwm + TCFG0);	/* timers 0 and 1: no prescaler */
	writel(readl(h->pwm + TCFG1) & ~0xf0U, h->pwm + TCFG1);	/* timer 1: no divider */
	writel(PERIOD_COUNTS - 1, h->pwm + TCNTB1);
	writel(PERIOD_COUNTS - 1 - duty, h->pwm + TCMPB1);
	writel(tcon | TCON_T1_INVERT | TCON_T1_MANUAL, h->pwm + TCON);
	writel(tcon | TCON_T1_INVERT | TCON_T1_RELOAD | TCON_T1_START, h->pwm + TCON);
}

static void zrn_haptic_work(struct work_struct *work)
{
	struct zrn_haptic *h = container_of(work, struct zrn_haptic, work);
	unsigned int magnitude = h->magnitude;

	if (magnitude) {
		zrn_haptic_pwm(h, magnitude);
		if (!h->running)
			regmap_write(h->regmap, REG_MCONFIG, MCONFIG_LRA_DIV128 | MCONFIG_MEN);
		h->running = true;
	} else if (h->running) {
		regmap_write(h->regmap, REG_MCONFIG, MCONFIG_LRA_DIV128);
		zrn_haptic_pwm(h, 0);
		h->running = false;
	}
}

static int zrn_haptic_play(struct input_dev *input, void *data, struct ff_effect *effect)
{
	struct zrn_haptic *h = input_get_drvdata(input);
	unsigned int strong = effect->u.rumble.strong_magnitude, weak = effect->u.rumble.weak_magnitude;

	h->magnitude = strong > weak ? strong : weak;
	schedule_work(&h->work);
	return 0;
}

static void zrn_haptic_close(struct input_dev *input)
{
	struct zrn_haptic *h = input_get_drvdata(input);

	h->magnitude = 0;
	schedule_work(&h->work);
	flush_work(&h->work);
}

static int zrn_haptic_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct i2c_client *i2c = to_i2c_client(dev->parent);
	struct zrn_haptic *h;
	u32 v;
	int ret;

	h = devm_kzalloc(dev, sizeof(*h), GFP_KERNEL);
	if (!h)
		return -ENOMEM;
	h->dev = dev;
	h->regmap = devm_regmap_init_i2c(i2c, &zrn_haptic_regmap);
	if (IS_ERR(h->regmap))
		return dev_err_probe(dev, PTR_ERR(h->regmap), "no regmap\n");
	h->pwm = devm_ioremap(dev, PWM_BASE, 0x100);
	h->pins = devm_ioremap(dev, PINS_BASE, 0x100);
	if (!h->pwm || !h->pins)
		return -ENOMEM;
	INIT_WORK(&h->work, zrn_haptic_work);

	/* gpg1-7: an output, high (the motor's boost); gpp3-1: the timer's output */
	writel(readl(h->pins + GPG1 + PIN_DAT) | BIT(7), h->pins + GPG1 + PIN_DAT);
	v = readl(h->pins + GPG1 + PIN_CON);
	writel((v & ~(0xfU << 28)) | (1U << 28), h->pins + GPG1 + PIN_CON);
	zrn_haptic_pwm(h, 0);
	v = readl(h->pins + GPP3 + PIN_CON);
	writel((v & ~(0xfU << 4)) | (2U << 4), h->pins + GPP3 + PIN_CON);
	v = readl(h->pins + GPP3 + PIN_PUD);
	writel((v & ~(0xfU << 4)) | (1U << 4), h->pins + GPP3 + PIN_PUD);	/* pulled down */

	ret = regmap_update_bits(h->regmap, REG_MAINCTRL1, MAINCTRL1_BIASEN, MAINCTRL1_BIASEN);
	if (!ret)
		ret = regmap_write(h->regmap, REG_MCONFIG, MCONFIG_LRA_DIV128);
	if (ret)
		return dev_err_probe(dev, ret, "the power chip did not take the motor's settings\n");

	h->input = devm_input_allocate_device(dev);
	if (!h->input)
		return -ENOMEM;
	h->input->name = "zrn-haptic";
	h->input->id.bustype = BUS_I2C;
	h->input->close = zrn_haptic_close;
	input_set_drvdata(h->input, h);
	input_set_capability(h->input, EV_FF, FF_RUMBLE);
	ret = input_ff_create_memless(h->input, NULL, zrn_haptic_play);
	if (ret)
		return dev_err_probe(dev, ret, "no rumble device\n");
	ret = input_register_device(h->input);
	if (ret)
		return ret;
	platform_set_drvdata(pdev, h);
	dev_info(dev, "vibration motor ready (160 Hz, %d %% duty)\n", DUTY_PERCENT);
	return 0;
}

static void zrn_haptic_remove(struct platform_device *pdev)
{
	struct zrn_haptic *h = platform_get_drvdata(pdev);

	h->magnitude = 0;
	cancel_work_sync(&h->work);
	regmap_write(h->regmap, REG_MCONFIG, MCONFIG_LRA_DIV128);
	zrn_haptic_pwm(h, 0);
}

static const struct platform_device_id zrn_haptic_ids[] = {
	{ "max77705-haptic" },
	{ }
};
MODULE_DEVICE_TABLE(platform, zrn_haptic_ids);

static struct platform_driver zrn_haptic_driver = {
	.driver = { .name = "zrn-haptic" },
	.probe = zrn_haptic_probe,
	.remove = zrn_haptic_remove,
	.id_table = zrn_haptic_ids,
};
module_platform_driver(zrn_haptic_driver);

MODULE_DESCRIPTION("Galaxy S9 vibration motor (MAX77705 LRA driver, SoC timer PWM)");
MODULE_LICENSE("GPL");
