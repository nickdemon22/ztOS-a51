// SPDX-License-Identifier: GPL-2.0-only
/* Clear only Samsung CP_CTRL_NS SET_SW_MIF_REQ, never MASK_MIF_REQ. */
#include <linux/module.h>
#include <linux/arm-smccc.h>
static bool clear_force, changed;
static unsigned int before, after;
module_param(clear_force, bool, 0444);
module_param(before, uint, 0444);
module_param(after, uint, 0444);
static int read_ctrl(u32 *value)
{
 struct arm_smccc_res lo, hi;
 arm_smccc_smc(0x82000700, 3, 0, 1, 0, 0, 0, 0, &lo);
 arm_smccc_smc(0x82000700, 3, 1, 1, 0, 0, 0, 0, &hi);
 if ((lo.a0 & 0xffff) || (hi.a0 & 0xffff)) return -EIO;
 *value = ((lo.a0 >> 16) & 0xffff) | (hi.a0 & 0xffff0000);
 return 0;
}
static int write_ctrl(u32 value)
{
 struct arm_smccc_res result;
 arm_smccc_smc(0x82000700, 4, value, 1, 0, 0, 0, 0, &result);
 return result.a0 ? -EIO : 0;
}
static int __init start(void)
{
 int ret = read_ctrl(&before);
 if (ret) return ret;
 if (clear_force && (before & BIT(12))) {
  ret = write_ctrl(before & ~BIT(12));
  if (ret) return ret;
  changed = true;
 }
 ret = read_ctrl(&after);
 if (ret || (changed && (after & BIT(12)))) {
  if (changed) write_ctrl(before);
  return ret ? ret : -EIO;
 }
 return 0;
}
static void __exit stop(void)
{
 u32 now;
 if (changed && !read_ctrl(&now)) write_ctrl(now | BIT(12));
}
module_init(start);
module_exit(stop);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Bounded CP software MIF-force diagnostic with restoration");
