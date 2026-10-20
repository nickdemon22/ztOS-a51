// SPDX-License-Identifier: GPL-2.0
/*
 * The CP's power sequences on the Exynos 9810, as Samsung's PMU CAL runs them
 * (drivers/soc/samsung/cal-if: pmucal_cp.c, pmucal_rae.c and
 * exynos9810/pmucal_cp_exynos9810.h): writes and waits on PMU registers, the
 * two CP_CTRL registers through the secure monitor, which owns them.
 */
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/module.h>

#define PMU_BASE	0x14060000
#define GICD_BASE	0x10100000
#define CP_CTRL_NS	0x0030
#define CP_CTRL_S	0x0034
#define READ_CTRL	0x3
#define WRITE_CTRL	0x4

enum op { OP_WRITE, OP_READ, OP_WAIT, OP_DELAY };

struct step {
	enum op op;
	const char *name;
	u32 base, offset, mask, value;
};

#define W(n, o, m, v)	{ OP_WRITE, n, PMU_BASE, o, m, v }
#define R(n, o, m)	{ OP_READ, n, PMU_BASE, o, m, 0 }
#define T(n, b, o, m, v){ OP_WAIT, n, b, o, m, v }
#define D(us)		{ OP_DELAY, "DELAY", 0, 0, 0, us }

static const struct step cp_init[] = {
	W("CP_CTRL_NS", 0x0030, 1 << 1, 1 << 1),
	W("CP_CTRL_S", 0x0034, 1 << 3, 1 << 3),
	D(0x400),
	T("RESET_SEQUENCER_STATUS", PMU_BASE, 0x0504, 0x7, 0x5),
	T("INTERRUPT_SET_PENDING", GICD_BASE, 0x1238, 1 << 28, 1 << 28),
};
static const struct step cp_status[] = {
	R("RESET_SEQUENCER_STATUS", 0x0504, 0x7),
};
static const struct step cp_reset_assert[] = {
	W("RESET_AHEAD_CP_SYS_PWR_REG", 0x1320, 0x3, 0x0),
	W("CLEANY_BUS_CP_SYS_PWR_REG", 0x1324, 0x1, 0x0),
	W("LOGIC_RESET_CP_SYS_PWR_REG", 0x1328, 0x3, 0x0),
	W("TCXO_GATE_CP_SYS_PWR_REG", 0x132c, 0x1, 0x0),
	W("CP_DISABLE_ISO_SYS_PWR_REG", 0x1330, 0x1, 0x1),
	W("CP_RESET_ISO_SYS_PWR_REG", 0x1334, 0x1, 0x0),
	W("CP_DUMP_PC_SYS_PWR_REG", 0x1338, 0x1, 0x0),
	W("CP_CTRL_NS", 0x0030, 1 << 2, 1 << 2),
	W("CENTRAL_SEQ_CP_CONFIGURATION", 0x0280, 1 << 16, 0x0),
	D(0x400),
	T("CENTRAL_SEQ_CP_STATUS", PMU_BASE, 0x0284, 0xff << 16, 0x80 << 16),
};
static const struct step cp_reset_release[] = {
	W("CP_CTRL_S", 0x0034, 1 << 3, 1 << 3),
	T("CENTRAL_SEQ_CP_STATUS", PMU_BASE, 0x0284, 0xff << 16, 0),
	T("INTERRUPT_SET_PENDING", GICD_BASE, 0x1238, 1 << 28, 1 << 28),
};
static const struct step cp_active_clear[] = {
	W("CP_CTRL_NS", 0x0030, 1 << 6, 1 << 6),
};
static const struct step cp_reset_req_clear[] = {
	W("CP_CTRL_NS", 0x0030, 1 << 8, 1 << 8),
};
static const struct step cp_dump_pc_on[] = {
	W("CP_CTRL_NS", 0x0030, 1 << 17, 1 << 17),
};
static const struct step cp_dump_pc_off[] = {
	W("CP_CTRL_NS", 0x0030, 1 << 17, 0),
};

static void __iomem *pmu, *gicd;

static void __iomem *base_of(u32 base)
{
	if (!pmu) {
		pmu = ioremap(PMU_BASE, 0x2000);
		gicd = ioremap(GICD_BASE, 0x2000);
	}
	return base == GICD_BASE ? gicd : pmu;
}

/* CP_CTRL_S / CP_CTRL_NS through the monitor: 16 bits at a time, the answer
 * in the upper half of x0 and an error in the lower */
static int ctrl_read(u32 offset, u32 *out)
{
	u32 reg = offset == CP_CTRL_S ? 0 : 1, lo, hi;
	int r = exynos_smc(SMC_ID, READ_CTRL, 0, reg);
	if (r & 0xffff)
		return -EIO;
	lo = (u32)r >> 16;
	r = exynos_smc(SMC_ID, READ_CTRL, 1, reg);
	if (r & 0xffff)
		return -EIO;
	hi = (u32)r >> 16;
	*out = hi << 16 | lo;
	return 0;
}

static int ctrl_write(u32 offset, u32 mask, u32 value)
{
	u32 reg = offset == CP_CTRL_S ? 0 : 1, now = 0;
	if (mask != U32_MAX && ctrl_read(offset, &now))
		return -EIO;
	value = (value & mask) | (now & ~mask);
	if (exynos_smc(SMC_ID, WRITE_CTRL, value, reg) > 0)
		return -EIO;
	pr_info("zrn-modem: pmu %s = 0x%08x\n", offset == CP_CTRL_S ? "CP_CTRL_S" : "CP_CTRL_NS", value);
	return 0;
}

static int run(const struct step *s, int n, u32 *read)
{
	for (int i = 0; i < n; i++, s++) {
		void __iomem *reg = base_of(s->base) + s->offset;
		bool secure = s->base == PMU_BASE && (s->offset == CP_CTRL_S || s->offset == CP_CTRL_NS);
		switch (s->op) {
		case OP_WRITE:
			if (secure) {
				if (ctrl_write(s->offset, s->mask, s->value))
					return -EIO;
			} else {
				writel((readl(reg) & ~s->mask) | s->value, reg);
			}
			break;
		case OP_READ:
			if (secure) {
				u32 v;
				if (ctrl_read(s->offset, &v))
					return -EIO;
				*read = v & s->mask;
			} else {
				*read = readl(reg) & s->mask;
			}
			break;
		case OP_WAIT: {
			int us = 0;
			while ((readl(reg) & s->mask) != s->value) {
				if (++us > 1000) {
					pr_err("zrn-modem: pmu %s timed out (0x%08x)\n", s->name, readl(reg));
					return -ETIMEDOUT;
				}
				udelay(1);
			}
			break;
		}
		case OP_DELAY:
			udelay(s->value);
			break;
		}
	}
	return 0;
}

#define RUN(seq)	run(seq, ARRAY_SIZE(seq), NULL)

int cal_cp_init(void)			{ pr_info("zrn-modem: cp init\n"); return RUN(cp_init); }
int cal_cp_reset_assert(void)		{ pr_info("zrn-modem: cp reset assert\n"); return RUN(cp_reset_assert); }
int cal_cp_reset_release(void)		{ pr_info("zrn-modem: cp reset release\n"); return RUN(cp_reset_release); }
void cal_cp_active_clear(void)		{ RUN(cp_active_clear); }
void cal_cp_reset_req_clear(void)	{ RUN(cp_reset_req_clear); }
void cal_cp_enable_dump_pc_no_pg(void)	{ RUN(cp_dump_pc_on); }
void cal_cp_disable_dump_pc_no_pg(void)	{ RUN(cp_dump_pc_off); }

/* 1 when the CP has been initialised (the reset sequencer reads 5), else 0 */
int cal_cp_status(void)
{
	u32 v = 0;
	int ret = run(cp_status, ARRAY_SIZE(cp_status), &v);
	return ret < 0 ? ret : v == 0x5;
}

#include <linux/netdevice.h>
struct napi_struct *zrn_napi;	/* see zrn_compat.h */
