#!/usr/bin/env python3
"""Restore opted-in Exynos9810 GPIO controllers before PCI resume_noirq."""
from pathlib import Path
import sys

path = Path(sys.argv[1]) / 'drivers/pinctrl/samsung/pinctrl-samsung.c'
s = path.read_text()
marker = 'static bool __maybe_unused samsung_pinctrl_resume_in_noirq'
old = '''static const struct dev_pm_ops samsung_pinctrl_pm_ops = {
	SET_LATE_SYSTEM_SLEEP_PM_OPS(samsung_pinctrl_suspend,
				     samsung_pinctrl_resume)
};'''
new = '''/* PCI config accesses happen in resume_noirq. The WLAN PERST/enable
 * GPIOs must have their saved mux/data restored before that phase. Other
 * controllers and other SoCs keep the existing late/early ordering. */
static bool __maybe_unused samsung_pinctrl_resume_in_noirq(struct device *dev)
{
	return of_device_is_compatible(dev->of_node, "samsung,exynos9810-pinctrl") &&
		of_property_read_bool(dev->of_node, "samsung,noirq-resume");
}

static int __maybe_unused samsung_pinctrl_resume_noirq(struct device *dev)
{
	if (samsung_pinctrl_resume_in_noirq(dev))
		return samsung_pinctrl_resume(dev);
	return 0;
}

static int __maybe_unused samsung_pinctrl_resume_early(struct device *dev)
{
	if (samsung_pinctrl_resume_in_noirq(dev))
		return 0;
	return samsung_pinctrl_resume(dev);
}

static const struct dev_pm_ops samsung_pinctrl_pm_ops = {
	SET_LATE_SYSTEM_SLEEP_PM_OPS(samsung_pinctrl_suspend,
				     samsung_pinctrl_resume_early)
	SET_NOIRQ_SYSTEM_SLEEP_PM_OPS(NULL, samsung_pinctrl_resume_noirq)
};'''
if marker in s:
    if new not in s:
        raise SystemExit('pinctrl noirq patch differs; inspect source drift')
elif s.count(old) == 1:
    path.write_text(s.replace(old, new))
else:
    raise SystemExit('pinctrl PM anchor changed; inspect before patching')
