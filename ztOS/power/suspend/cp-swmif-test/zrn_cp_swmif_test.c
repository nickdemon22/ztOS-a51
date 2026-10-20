// SPDX-License-Identifier: GPL-2.0
/*
 * Test: does CP_CTRL_NS.SET_SW_MIF_REQ (bit 12) hold the CP's memory request?
 * Clears the bit on load, puts the old value back on unload. CP_CTRL_NS is
 * owned by the secure monitor: read and written 16 bits at a time through
 * SMC 0x82000700 (READ_CTRL 3, WRITE_CTRL 4), as zrn-modem's zrn_cp_pmu.c.
 */
#include <linux/arm-smccc.h>
#include <linux/module.h>

#define SMC_ID		0x82000700
#define READ_CTRL	0x3
#define WRITE_CTRL	0x4
#define CTRL_NS		1
#define SET_SW_MIF_REQ	(1u << 12)

static u32 saved;
static bool changed;
static uint before, after;
module_param(before, uint, 0444);
module_param(after, uint, 0444);

static long smc(unsigned long a1, unsigned long a2, unsigned long a3)
{
	struct arm_smccc_res res;
	arm_smccc_smc(SMC_ID, a1, a2, a3, 0, 0, 0, 0, &res);
	return (long)res.a0;
}

static int ctrl_read(u32 *out)
{
	long lo = smc(READ_CTRL, 0, CTRL_NS), hi;
	if (lo & 0xffff)
		return -EIO;
	hi = smc(READ_CTRL, 1, CTRL_NS);
	if (hi & 0xffff)
		return -EIO;
	*out = ((u32)hi >> 16) << 16 | (u32)lo >> 16;
	return 0;
}

static int ctrl_write(u32 value)
{
	return smc(WRITE_CTRL, value, CTRL_NS) > 0 ? -EIO : 0;
}

static int __init swmif_init(void)
{
	if (ctrl_read(&saved))
		return -EIO;
	before = saved;
	if (!(saved & SET_SW_MIF_REQ)) {
		pr_info("zrn-cp-swmif: CP_CTRL_NS 0x%08x, SET_SW_MIF_REQ already clear\n", saved);
		return 0;
	}
	if (ctrl_write(saved & ~SET_SW_MIF_REQ))
		return -EIO;
	changed = true;
	ctrl_read(&after);
	pr_info("zrn-cp-swmif: CP_CTRL_NS 0x%08x -> 0x%08x\n", before, after);
	return 0;
}

static void __exit swmif_exit(void)
{
	u32 now = 0;
	if (changed && !ctrl_read(&now))
		ctrl_write(now | SET_SW_MIF_REQ);
	ctrl_read(&now);
	pr_info("zrn-cp-swmif: restored CP_CTRL_NS 0x%08x\n", now);
}

module_init(swmif_init);
module_exit(swmif_exit);
MODULE_LICENSE("GPL");
