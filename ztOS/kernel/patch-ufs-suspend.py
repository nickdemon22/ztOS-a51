#!/usr/bin/env python3
"""Preserve UFS device/link state when suspending into Hibern8.

The port asserted device reset even when the core expected the retained
Hibern8 link to resume. Reset/isolation is only valid for a link-off path,
where ufshcd reinitializes the host/device. Keep the existing full-reset
path for link-off; Hibern8 already has the PHY calibration sleep sequence.
"""
from pathlib import Path
import sys

path = Path(sys.argv[1]) / "drivers/ufs/host/ufs-exynos9810.c"
s = path.read_text()
marker = "zboot: retain Hibern8 link"
if marker in s:
    print("patch-ufs-suspend: already applied")
    sys.exit(0)
old = """	hci_writel(ufs, 0, HCI_GPIO_OUT);
	exynos_ufs_ctrl_phy_pwr(ufs, false);

	return 0;
}

static int exynos_ufs_resume"""
new = """	/* zboot: retain Hibern8 link. Reset would destroy the state that
	 * ufshcd expects to leave through DME_HIBER_EXIT on resume. A
	 * link-off transition instead has a full host/device reset path. */
	if (ufshcd_is_link_off(hba)) {
		hci_writel(ufs, 0, HCI_GPIO_OUT);
		exynos_ufs_ctrl_phy_pwr(ufs, false);
	}

	return 0;
}

static int exynos_ufs_resume"""
if s.count(old) != 1:
    sys.exit("patch-ufs-suspend: suspend anchor changed")
path.write_text(s.replace(old, new))
print("patch-ufs-suspend: Hibern8 state preserved across suspend")
