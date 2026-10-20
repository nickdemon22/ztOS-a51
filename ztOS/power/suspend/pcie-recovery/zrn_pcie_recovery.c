// SPDX-License-Identifier: GPL-2.0-only
/* Recover the unbound WLAN port using its existing host/PHY callbacks. */
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pci.h>
#include <linux/platform_device.h>
#include <linux/interrupt.h>
#include "pcie-designware.h"

static bool recover;
module_param(recover, bool, 0400);

static int __init recover_port(void)
{
	struct device *host;
	struct pci_dev *root, *wlan;
	struct dw_pcie *pci;
	u32 id;
	int irq, ret = 0;
	if (!of_machine_is_compatible("samsung,exynos9810"))
		return -ENODEV;
	host = bus_find_device_by_name(&platform_bus_type, NULL, "116a0000.pcie");
	if (!host)
		return -ENODEV;
	root = pci_get_domain_bus_and_slot(0, 0, PCI_DEVFN(0, 0));
	wlan = pci_get_domain_bus_and_slot(0, 1, PCI_DEVFN(0, 0));
	if (!root || !wlan) {
		ret = -ENODEV;
		goto put;
	}
	device_lock(host);
	device_lock(&wlan->dev);
	/* The audited host structure embeds dw_pcie as its first member. */
	pci = dev_get_drvdata(host);
	if (!host->driver || strcmp(host->driver->name, "pcie-exynos9810") ||
	    !pci || pci->dev != host || wlan->dev.driver ||
	    wlan->vendor != 0x14e4 || wlan->device != 0x441f) {
		ret = -EBUSY;
		goto unlock;
	}
	pr_info("zrn-pcie-recovery: saved IDs root=%08x wlan=%08x BAR0=%08x BAR2=%08x\n",
		root->saved_config_space[0], wlan->saved_config_space[0],
		wlan->saved_config_space[4], wlan->saved_config_space[6]);
	if (wlan->saved_config_space[0] != 0x441f14e4 ||
	    root->saved_config_space[0] != 0xa598144d ||
	    (wlan->saved_config_space[4] & ~15U) != pci_resource_start(wlan, 0) ||
	    (wlan->saved_config_space[6] & ~15U) != pci_resource_start(wlan, 2)) {
		ret = -EINVAL;
		goto unlock;
	}
	if (!recover)
		goto unlock;
	irq = platform_get_irq(to_platform_device(host), 0);
	if (irq < 0) {
		ret = irq;
		goto unlock;
	}
	disable_irq(irq);
	dw_pcie_stop_link(pci);
	pci->suspended = true;
	ret = dw_pcie_resume_noirq(pci);
	if (!ret) {
		pci_restore_state(root);
		ret = pci_set_power_state(wlan, PCI_D0);
		if (!ret) {
			pci_restore_state(wlan);
			ret = pci_read_config_dword(wlan, PCI_VENDOR_ID, &id);
			if (!ret && id != 0x441f14e4)
				ret = -ENODEV;
		}
	}
	enable_irq(irq);
	pr_info("zrn-pcie-recovery: controller/link/config recovery result=%d\n", ret);
unlock:
	device_unlock(&wlan->dev);
	device_unlock(host);
put:
	pci_dev_put(wlan);
	pci_dev_put(root);
	put_device(host);
	return ret;
}
static void __exit recovery_exit(void) { }
module_init(recover_port);
module_exit(recovery_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Explicit recovery of the detached Exynos9810 WLAN PCIe port");
