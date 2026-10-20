// SPDX-License-Identifier: GPL-2.0
/*
 * zrn_btgate: step 1 of Bluetooth: open UART_BT's clock gates in CMU_PERIC1
 * (Samsung's cmucal: +0x20c4 IPCLK, +0x20c8 PCLK, +0x201c RSTnSYNC,
 * +0x2034 GATE_CLK_PERIC1_UART_BT; MANUAL | CG_VAL = bits 20 and 21, as the
 * PCIe driver opens its own), then read the UART at 0x10840000. Prints the
 * gates before and after, and the divider. Unloads itself; the gates stay
 * open.
 */
#include <linux/io.h>
#include <linux/module.h>

#define CMU_PERIC1	0x10800000
#define UART_BT		0x10840000
static const u32 gates[] = { 0x20c4, 0x20c8, 0x201c, 0x2034 };

static int __init gate_init(void)
{
	void __iomem *cmu = ioremap(CMU_PERIC1, 0x4000), *uart;
	int i;

	if (!cmu)
		return -ENOMEM;
	for (i = 0; i < ARRAY_SIZE(gates); i++)
		pr_info("zrn_btgate: gate +%#x = %#010x\n", gates[i], readl(cmu + gates[i]));
	pr_info("zrn_btgate: div +0x1814 = %#010x, qch +0x3028 = %#010x\n",
		readl(cmu + 0x1814), readl(cmu + 0x3028));
	for (i = 0; i < ARRAY_SIZE(gates); i++) {
		writel(readl(cmu + gates[i]) | 0x300000, cmu + gates[i]);
		pr_info("zrn_btgate: gate +%#x now %#010x\n", gates[i], readl(cmu + gates[i]));
	}
	iounmap(cmu);
	uart = ioremap(UART_BT, 0x100);
	if (uart) {
		pr_info("zrn_btgate: UART ULCON %#x UCON %#x UFCON %#x UFSTAT %#x UBRDIV %#x\n",
			readl(uart + 0x0), readl(uart + 0x4), readl(uart + 0x8),
			readl(uart + 0x18), readl(uart + 0x28));
		iounmap(uart);
	}
	return -EAGAIN;
}
module_init(gate_init);
MODULE_LICENSE("GPL");
