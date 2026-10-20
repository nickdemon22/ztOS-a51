// SPDX-License-Identifier: GPL-2.0
/*
 * zrn-modem.ko: the three vendor drivers in it, registered in the order the
 * stock kernel brought them up (the mailbox, the shared memory, then the
 * modem interface that uses both). Each binds to its node of the device tree,
 * which the module adds itself: the stock tree's nodes, as an overlay
 * (zrn-modem-overlay.dts), so that the boot image stays as it is.
 */
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

extern const u8 zrn_overlay[];
extern const u32 zrn_overlay_size;
static int overlay_id = -1;

extern struct platform_driver *zrn_mcu_ipc_driver, *zrn_shmem_driver, *zrn_modem_driver;
int zrn_shm_find_memory(void);

static struct platform_driver **drivers[] = {
	&zrn_mcu_ipc_driver, &zrn_shmem_driver, &zrn_modem_driver,
};

static int __init zrn_modem_init(void)
{
	int i, ret;

	ret = zrn_shm_find_memory();
	if (ret)
		return ret;
	ret = of_overlay_fdt_apply(zrn_overlay, zrn_overlay_size, &overlay_id, NULL);
	if (ret) {
		pr_err("zrn-modem: device tree overlay: %d\n", ret);
		if (overlay_id >= 0)
			of_overlay_remove(&overlay_id);
		return ret;
	}
	for (i = 0; i < ARRAY_SIZE(drivers); i++) {
		ret = platform_driver_register(*drivers[i]);
		if (ret) {
			pr_err("zrn-modem: %s: %d\n", (*drivers[i])->driver.name, ret);
			while (--i >= 0)
				platform_driver_unregister(*drivers[i]);
			of_overlay_remove(&overlay_id);
			return ret;
		}
	}
	pr_info("zrn-modem: drivers registered\n");
	return 0;
}

static void __exit zrn_modem_exit(void)
{
	for (int i = ARRAY_SIZE(drivers) - 1; i >= 0; i--)
		platform_driver_unregister(*drivers[i]);
	if (overlay_id >= 0)
		of_overlay_remove(&overlay_id);
}

module_init(zrn_modem_init);
module_exit(zrn_modem_exit);
MODULE_DESCRIPTION("Samsung Shannon modem interface (exynos9810 vendor kernel) for mainline");
MODULE_LICENSE("GPL");
