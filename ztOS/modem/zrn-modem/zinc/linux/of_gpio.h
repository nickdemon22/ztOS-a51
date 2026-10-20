/* stand-in: mainline dropped the integer GPIO lookups from the device tree.
 * The S9's modem node has no GPIOs (stock/twrp-live.dts): every lookup finds
 * none, as the stock driver did. */
#pragma once
#include <linux/gpio.h>
#include <linux/of.h>
static inline int of_get_named_gpio(const struct device_node *np, const char *name, int index)
{
	return -ENOENT;
}
static inline int of_get_gpio(const struct device_node *np, int index)
{
	return -ENOENT;
}
static inline int of_gpio_count(const struct device_node *np)
{
	return 0;
}
