// SPDX-License-Identifier: GPL-2.0
/*
 * zrn_abox, stage 1: start the Exynos 9810 audio DSP ("ABOX", a Cortex-A7
 * running Samsung's "calliope" firmware) from a loadable module, and see
 * whether it reports that it has booted.
 *
 * Stage 2 (tone=HZ): with the DSP up, play a quiet sine through one playback
 * stream (RDMA0) routed to the port the speaker amplifiers listen on (UAIF1,
 * I2S, 48 kHz, 16 bit, stereo), the way Samsung's driver and the stock mixer
 * paths set it up: mixer rate and format by message, the route and the port in
 * the block's registers, the bit clock from the audio PLL, the four I2S pins.
 * The amplifiers themselves are set up from user space (audio/amp.sh).
 *
 * Nothing here is ALSA yet. The sequence follows abox_enable() in Samsung's
 * driver (sound/soc/samsung/abox/abox.c, GPL): bring up the block's own
 * interrupt controller, put the firmware in its SRAM and, through the audio
 * IOMMU, at the addresses it expects its DRAM, release the core, and watch
 * the mailbox it writes to. The addresses are from the stock device tree.
 *
 * The audio power domain and its clocks are left running by the bootloader
 * on this phone (PMU 0x4004 reads xF, and the block answers "ABOX").
 */
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/firmware.h>
#include <linux/io.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/sizes.h>
#include <linux/workqueue.h>

#define PMU_BASE		0x14060000
#define PMU_AUD_STATUS		0x4004
#define PMU_ABOX_CPU_CFG	0x415c
#define PMU_ABOX_CPU_STATUS	0x4160
#define PMU_ABOX_CPU_OPTION	0x4164
#define  ABOX_CPU_ENABLE	BIT(28)
#define PMU_GPIO_MODE_ABOX	0x1424
#define PMU_PAD_RET_ABOX	0x4170

#define ABOX_SFR		0x17c50000
#define  ABOX_IP_ID		0x0000
#define  ABOX_VERSION		0x0004
#define  ABOX_SYSPOWER_CTRL	0x0010
#define  ABOX_SYSPOWER_STATUS	0x0014
#define  ABOX_CPU_PC		0x2c3c
#define  ABOX_ROUTE_CTRL0	0x0040
#define  ABOX_SPUS_CTRL0	0x0200
#define  ABOX_SPUS_CTRL1	0x0204
#define  ABOX_UAIF1_CTRL0	0x0510
#define  ABOX_UAIF1_CTRL1	0x0514
#define  ABOX_UAIF1_STATUS	0x051c
#define  ABOX_RDMA0		0x1000
#define CMU_AUD			0x17c00000
#define  CMU_DIV_AUDIF		0x1800
#define  CMU_DIV_UAIF1		0x1828
#define  CMU_MUX_UAIF1		0x1008
#define PINCTRL_AUD		0x17c60000
#define  GPB1_CON		0x0020
#define ABOX_SYSREG		0x17c10000
#define  SYSREG_MISC_CON	0x032c
#define ABOX_SRAM		0x17d00000
#define ABOX_SRAM_SIZE		0x31000
#define  IPC_TX			0x30000
#define  IPC_TX_ACK		0x302fc
#define  IPC_RX			0x30300
#define  IPC_RX_ACK		0x305fc
#define ABOX_GICD		0x17cf1000
#define ABOX_GICC		0x17cf2000

#define SYSMMU_AUD		0x17e00000
#define  MMU_CTRL		0x000
#define  MMU_CFG		0x004
#define  MMU_STATUS		0x008
#define  MMU_PT_BASE		0x00c
#define  MMU_FLUSH_ALL		0x010
#define  MMU_INT_STATUS		0x060
#define  MMU_INT_CLEAR		0x064
#define  MMU_FAULT_VA		0x070
#define  MMU_FAULT_INFO		0x078

#define IOVA_DRAM		0x80000000UL
#define IOVA_IVA		0x90000000UL
#define DRAM_SIZE		(SZ_16M + SZ_2M)
#define IVA_SIZE		SZ_512K
/* Shared with the modem on the stock system: the voice firmware and its
 * parameters. There is no modem driver here, so the DSP gets empty memory
 * where it looks for them. */
#define IOVA_VSS		0xa0000000UL
#define IOVA_VPARAM		0xa1000000UL
#define VSS_SIZE		SZ_16M
#define VPARAM_SIZE		SZ_16M
#define LV2_TABLES		64

/* messages, as in Samsung's abox_ipc.h */
#define IPC_SYSTEM		1
#define IPC_PCMPLAYBACK		2
#define IPC_ABOX_CONFIG		0xa
#define  ABOX_RESUME		2
#define  SET_MIXER_SAMPLE_RATE	1
#define  SET_MIXER_FORMAT	0x10
#define  PCM_PLTDAI_OPEN	11
#define  PCM_PLTDAI_CLOSE	12
#define  PCM_PLTDAI_HW_PARAMS	14
#define  PCM_PLTDAI_PREPARE	16
#define  PCM_PLTDAI_TRIGGER	17
#define  PCM_SET_BUFFER		20
#define IPC_MSG_BYTES		764

#define IOVA_RDMA0		0x91000000UL
#define RATE			48000
#define PERIOD_FRAMES		1024
#define PERIODS			8
#define TONE_BYTES		(PERIOD_FRAMES * PERIODS * 4)

static int tone;
module_param(tone, int, 0444);
MODULE_PARM_DESC(tone, "after boot, play a quiet sine of about this many Hz on UAIF1 (0: do not)");

static struct platform_device *pdev;
static void __iomem *pmu, *sfr, *sysreg, *sram, *gicd, *gicc, *mmu, *cmu, *pins;
static void *dram, *iva, *vss, *vparam, *lv1, *lv2, *pcm;
static dma_addr_t dram_pa, iva_pa, vss_pa, vparam_pa, lv1_pa, lv2_pa, pcm_pa;
static struct task_struct *rx_task;
static struct work_struct play;
static bool playing;
static int lv2_used = 1;	/* table 0 stays empty: what unmapped space points at */
static bool started;
static struct delayed_work watch;
static int watched;

static void pmu_update(u32 reg, u32 mask, u32 val)
{
	writel((readl(pmu + reg) & ~mask) | (val & mask), pmu + reg);
}

/* The core held in reset and unpowered, whatever it was doing. */
static void core_off(void)
{
	int i;

	pmu_update(PMU_ABOX_CPU_OPTION, ABOX_CPU_ENABLE, 0);
	pmu_update(PMU_ABOX_CPU_CFG, 0x1, 0x0);
	for (i = 0; i < 100 && (readl(pmu + PMU_ABOX_CPU_STATUS) & 0x1); i++)
		usleep_range(1000, 2000);
	pr_info("zrn_abox: core off: status %08x after %d ms\n", readl(pmu + PMU_ABOX_CPU_STATUS), i);
}

/* One 4K page of the DSP's address space onto one of ours. */
static int mmu_map_page(unsigned long iova, phys_addr_t pa)
{
	u32 *l1 = lv1, *l2;
	u32 link = l1[iova >> 20];
	phys_addr_t table;

	if (link == (((u32)(lv2_pa >> 4)) | 1)) {
		if (lv2_used == LV2_TABLES)
			return -ENOMEM;
		table = lv2_pa + lv2_used * SZ_1K;
		lv2_used++;
		l1[iova >> 20] = (u32)(table >> 4) | 1;
	} else {
		table = (phys_addr_t)(link & ~0xfU) << 4;
	}
	l2 = lv2 + (table - lv2_pa);
	l2[(iova >> 12) & 0xff] = (u32)(pa >> 4) | (3 << 2) | 2;	/* read and write, small page */
	return 0;
}

static int mmu_map(unsigned long iova, phys_addr_t pa, size_t size)
{
	size_t off;
	int ret;

	for (off = 0; off < size; ) {
		/* a whole megabyte at once where everything lines up */
		if (!((iova + off) & (SZ_1M - 1)) && !((pa + off) & (SZ_1M - 1)) &&
				size - off >= SZ_1M) {
			((u32 *)lv1)[(iova + off) >> 20] = (u32)((pa + off) >> 4) | (3 << 4) | 2;
			off += SZ_1M;
			continue;
		}
		ret = mmu_map_page(iova + off, pa + off);
		if (ret)
			return ret;
		off += SZ_4K;
	}
	return 0;
}

/* Send the DSP one message: into its mailbox, flag it, and raise the software
 * interrupt on its controller whose number is the message's kind. */
static int ipc_send(const u32 *words, int n)
{
	u32 msg[IPC_MSG_BYTES / 4] = { 0 };
	int i;

	memcpy(msg, words, n * 4);
	memcpy_toio(sram + IPC_TX, msg, sizeof(msg));
	writel(1, sram + IPC_TX_ACK);
	writel(BIT(16) | (words[0] & 0xf), gicd + 0xf00);
	for (i = 0; i < 20000 && readl(sram + IPC_TX_ACK); i++)
		udelay(10);
	if (readl(sram + IPC_TX_ACK)) {
		pr_err("zrn_abox: message %u/%u not taken\n", words[0], words[2]);
		writel(0, sram + IPC_TX_ACK);
		return -EIO;
	}
	pr_info("zrn_abox: sent %u/%u (%u, %u, %u), taken after %d0 us\n", words[0], words[2],
		words[3], words[4], words[5], i);
	return 0;
}

/* What the DSP sends back is read and acknowledged here, a thousand times a
 * second: its interrupt line is not wired up in this prototype. */
static int rx_fn(void *unused)
{
	int shown = 0;

	while (!kthread_should_stop()) {
		if (readl(sram + IPC_RX_ACK)) {
			u32 rx[6];
			int i;

			for (i = 0; i < 6; i++)
				rx[i] = readl(sram + IPC_RX + i * 4);
			writel(0, sram + IPC_RX_ACK);
			if (shown++ < 60)
				pr_info("zrn_abox: got id=%u task=%d: %u %08x %08x %08x\n",
					rx[0], rx[1], rx[2], rx[3], rx[4], rx[5]);
		}
		usleep_range(900, 1100);
	}
	return 0;
}

static void cmu_div(u32 reg, u32 ratio)
{
	int i;

	writel((readl(cmu + reg) & ~0x1ffU) | ratio, cmu + reg);
	for (i = 0; i < 1000 && (readl(cmu + reg) & BIT(16)); i++)
		udelay(10);
}

/* A sine that fits the buffer a whole number of times, 30 dB under full scale. */
static void fill_tone(void)
{
	/* a quarter wave of sine, 0..1000, 17 points */
	static const s16 quarter[17] = { 0, 98, 195, 290, 383, 471, 556, 634, 707,
		773, 831, 882, 924, 957, 981, 995, 1000 };
	int frames = PERIOD_FRAMES * PERIODS;
	int cycles = max(1, tone * frames / RATE);
	s16 *out = pcm;
	int i;

	for (i = 0; i < frames; i++) {
		/* phase in 1/64ths of a cycle, with eight bits of fraction */
		u32 ph = (u32)(((u64)i * cycles * 64 * 256) / frames) & (64 * 256 - 1);
		int q = ph >> 12, idx = (ph >> 8) & 15, frac = ph & 255, a, b, v;

		if (q & 1)
			idx = 16 - idx, a = quarter[idx], b = quarter[idx - 1];
		else
			a = quarter[idx], b = quarter[idx + 1];
		v = a + (b - a) * frac / 256;
		if (q & 2)
			v = -v;
		out[i * 2] = out[i * 2 + 1] = (s16)v;
	}
	pr_info("zrn_abox: tone: %d cycles in %d frames, %d Hz\n", cycles, frames, cycles * RATE / frames);
}

static void play_fn(struct work_struct *work)
{
	u32 m[6];

	m[0] = IPC_SYSTEM; m[1] = 0; m[2] = ABOX_RESUME; m[3] = m[4] = m[5] = 0;
	ipc_send(m, 6);
	msleep(20);

	/* the mixer everything is summed in: 48 kHz, 16 bit, two channels */
	m[0] = IPC_ABOX_CONFIG; m[2] = SET_MIXER_SAMPLE_RATE; m[3] = RATE; m[4] = 0;
	ipc_send(m, 6);
	m[2] = SET_MIXER_FORMAT; m[3] = 16; m[4] = 2;
	ipc_send(m, 6);
	writel((readl(sfr + ABOX_SPUS_CTRL1) & ~0x1fU) | 9, sfr + ABOX_SPUS_CTRL1);
	/* every stream: rate converter in, out to SIFS0 (6 to SIFS2, 7 to SIFS1) */
	writel(0x15333333, sfr + ABOX_SPUS_CTRL0);
	/* UAIF1's speaker side listens to SIFS0 */
	writel((readl(sfr + ABOX_ROUTE_CTRL0) & ~0xf0U) | (1 << 4), sfr + ABOX_ROUTE_CTRL0);

	/* bit clock: PLL 1179.648 MHz / 16 = 73.728 MHz, / 48 = 1.536 MHz */
	cmu_div(CMU_DIV_AUDIF, 15);
	cmu_div(CMU_DIV_UAIF1, 47);
	writel(readl(cmu + CMU_MUX_UAIF1) & ~1U, cmu + CMU_MUX_UAIF1);
	pr_info("zrn_abox: clocks: audif %08x uaif1 %08x mux %08x\n", readl(cmu + CMU_DIV_AUDIF),
		readl(cmu + CMU_DIV_UAIF1), readl(cmu + CMU_MUX_UAIF1));
	/* gpb1-0..3 to the I2S function */
	writel((readl(pins + GPB1_CON) & ~0xffffU) | 0x2222, pins + GPB1_CON);
	/* I2S, 16 bits in 16-bit slots, two of them; the SoC drives the clocks */
	writel((9 << 24) | BIT(23) | (1 << 18) | (15 << 12), sfr + ABOX_UAIF1_CTRL1);
	writel(readl(sfr + ABOX_UAIF1_CTRL0) | BIT(2), sfr + ABOX_UAIF1_CTRL0);

	m[0] = IPC_PCMPLAYBACK; m[1] = 0; m[3] = 0; m[4] = m[5] = 0;
	m[2] = PCM_PLTDAI_OPEN;
	ipc_send(m, 6);
	{
		u32 b[7] = { IPC_PCMPLAYBACK, 0, PCM_SET_BUFFER, 0, IOVA_RDMA0, PERIOD_FRAMES * 4, PERIODS };
		u32 h[7] = { IPC_PCMPLAYBACK, 0, PCM_PLTDAI_HW_PARAMS, 0, RATE, 16, 2 };
		u32 t[5] = { IPC_PCMPLAYBACK, 0, PCM_PLTDAI_TRIGGER, 0, 1 };

		ipc_send(b, 7);
		ipc_send(h, 7);
		m[2] = PCM_PLTDAI_PREPARE;
		ipc_send(m, 6);
		writel(readl(sfr + ABOX_UAIF1_CTRL0) | BIT(0), sfr + ABOX_UAIF1_CTRL0);
		ipc_send(t, 5);
	}
	playing = true;
	msleep(300);
	pr_info("zrn_abox: playing? rdma0 ctrl %08x %08x buf %08x..%08x status %08x | uaif1 %08x %08x status %08x | route %08x spus %08x %08x\n",
		readl(sfr + ABOX_RDMA0), readl(sfr + ABOX_RDMA0 + 4), readl(sfr + ABOX_RDMA0 + 8),
		readl(sfr + ABOX_RDMA0 + 0xc), readl(sfr + ABOX_RDMA0 + 0x30),
		readl(sfr + ABOX_UAIF1_CTRL0), readl(sfr + ABOX_UAIF1_CTRL1), readl(sfr + ABOX_UAIF1_STATUS),
		readl(sfr + ABOX_ROUTE_CTRL0), readl(sfr + ABOX_SPUS_CTRL0), readl(sfr + ABOX_SPUS_CTRL1));
	msleep(500);
	pr_info("zrn_abox: later:   rdma0 status %08x, mmu int %08x va %08x, pc %08x\n",
		readl(sfr + ABOX_RDMA0 + 0x30), readl(mmu + MMU_INT_STATUS), readl(mmu + MMU_FAULT_VA),
		readl(sfr + ABOX_CPU_PC));
}

static void watch_fn(struct work_struct *work)
{
	u32 rx[6];
	int i;

	for (i = 0; i < 6; i++)
		rx[i] = readl(sram + IPC_RX + i * 4);
	if (watched % 5 == 0 || rx[0])
	pr_info("zrn_abox: pc=%08x cpu=%08x syspower=%08x mmu int=%08x va=%08x | rx id=%u task=%d type=%u p1=%08x p2=%08x p3=%08x ack=%u\n",
		readl(sfr + ABOX_CPU_PC), readl(pmu + PMU_ABOX_CPU_STATUS),
		readl(sfr + ABOX_SYSPOWER_STATUS), readl(mmu + MMU_INT_STATUS),
		readl(mmu + MMU_FAULT_VA) , rx[0], rx[1], rx[2], rx[3], rx[4], rx[5],
		readl(sram + IPC_RX_ACK));
	if (rx[0] == 1 && rx[2] == 3) {
		char v[4];

		memcpy(v, &rx[5], 4);
		pr_info("zrn_abox: calliope booted, version %c%c%c%c\n", v[3], v[2], v[1], v[0]);
		writel(0, sram + IPC_RX_ACK);
		rx_task = kthread_run(rx_fn, NULL, "zrn_abox_rx");
		if (IS_ERR(rx_task))
			rx_task = NULL;
		if (tone > 0)
			schedule_work(&play);
		return;
	}
	if (++watched < 30)
		schedule_delayed_work(&watch, msecs_to_jiffies(100));
}

static int __init zrn_abox_init(void)
{
	const struct firmware *fw_sram, *fw_dram, *fw_iva;
	struct device *dev;
	u32 *l1;
	int i, ret;

	pdev = platform_device_register_simple("zrn-abox", -1, NULL, 0);
	if (IS_ERR(pdev))
		return PTR_ERR(pdev);
	dev = &pdev->dev;
	dma_coerce_mask_and_coherent(dev, DMA_BIT_MASK(36));

	ret = request_firmware(&fw_sram, "calliope_sram.bin", dev);
	if (!ret)
		ret = request_firmware(&fw_dram, "calliope_dram.bin", dev);
	if (!ret)
		ret = request_firmware(&fw_iva, "calliope_iva.bin", dev);
	if (ret || fw_sram->size > ABOX_SRAM_SIZE || fw_dram->size > DRAM_SIZE ||
			fw_iva->size > IVA_SIZE) {
		pr_err("zrn_abox: firmware: %d\n", ret);
		goto err_dev;
	}

	pmu = ioremap(PMU_BASE, 0x10000);
	sfr = ioremap(ABOX_SFR, 0x10000);
	sysreg = ioremap(ABOX_SYSREG, 0x1000);
	sram = ioremap(ABOX_SRAM, ABOX_SRAM_SIZE);
	gicd = ioremap(ABOX_GICD, 0x1000);
	gicc = ioremap(ABOX_GICC, 0x2000);
	mmu = ioremap(SYSMMU_AUD, 0x9000);
	cmu = ioremap(CMU_AUD, 0x8000);
	pins = ioremap(PINCTRL_AUD, 0x1000);
	ret = -ENOMEM;
	if (!pmu || !sfr || !sysreg || !sram || !gicd || !gicc || !mmu || !cmu || !pins)
		goto err_dev;

	ret = -ENODEV;
	if ((readl(pmu + PMU_AUD_STATUS) & 0xf) != 0xf) {
		pr_err("zrn_abox: the audio power domain is off\n");
		goto err_dev;
	}
	pr_info("zrn_abox: block %08x version %08x, cpu status %08x\n",
		readl(sfr + ABOX_IP_ID), readl(sfr + ABOX_VERSION), readl(pmu + PMU_ABOX_CPU_STATUS));
	if (readl(sfr + ABOX_IP_ID) != 0x41424f58)
		goto err_dev;

	/* uncached, contiguous: what the DSP reads is what was written */
	ret = -ENOMEM;
	dram = dma_alloc_coherent(dev, DRAM_SIZE, &dram_pa, GFP_KERNEL);
	iva = dma_alloc_coherent(dev, IVA_SIZE, &iva_pa, GFP_KERNEL);
	vss = dma_alloc_coherent(dev, VSS_SIZE, &vss_pa, GFP_KERNEL);
	vparam = dma_alloc_coherent(dev, VPARAM_SIZE, &vparam_pa, GFP_KERNEL);
	pcm = dma_alloc_coherent(dev, TONE_BYTES, &pcm_pa, GFP_KERNEL);
	lv1 = dma_alloc_coherent(dev, SZ_16K, &lv1_pa, GFP_KERNEL);
	lv2 = dma_alloc_coherent(dev, LV2_TABLES * SZ_1K, &lv2_pa, GFP_KERNEL);
	if (!dram || !iva || !vss || !vparam || !pcm || !lv1 || !lv2)
		goto err_dev;
	pr_info("zrn_abox: dram %pad, iva %pad, page table %pad\n", &dram_pa, &iva_pa, &lv1_pa);

	memset(lv2, 0, LV2_TABLES * SZ_1K);
	l1 = lv1;
	for (i = 0; i < 4096; i++)
		l1[i] = (u32)(lv2_pa >> 4) | 1;	/* nothing here: the empty table */
	ret = mmu_map(IOVA_DRAM, dram_pa, DRAM_SIZE);
	if (!ret)
		ret = mmu_map(IOVA_IVA, iva_pa, IVA_SIZE);
	if (!ret)
		ret = mmu_map(IOVA_VSS, vss_pa, VSS_SIZE);
	if (!ret)
		ret = mmu_map(IOVA_VPARAM, vparam_pa, VPARAM_SIZE);
	if (!ret)
		ret = mmu_map(IOVA_RDMA0, pcm_pa, TONE_BYTES);
	if (!ret)
		ret = mmu_map(0x10000000, 0x10000000, SZ_4K);	/* the chip ID, as Samsung's driver does */
	if (ret)
		goto err_dev;

	core_off();	/* a fresh start, also after an earlier try */

	pr_info("zrn_abox: iommu on (was: ctrl %08x status %08x int %08x)\n", readl(mmu + MMU_CTRL),
		readl(mmu + MMU_STATUS), readl(mmu + MMU_INT_STATUS));
	writel(0x7, mmu + MMU_CTRL);			/* block */
	writel(readl(mmu + MMU_INT_STATUS), mmu + MMU_INT_CLEAR);
	writel((15 << 7) | BIT(20) | BIT(24) | BIT(2), mmu + MMU_CFG);
	writel((u32)(lv1_pa >> 12), mmu + MMU_PT_BASE);
	writel(1, mmu + MMU_FLUSH_ALL);
	writel(0x5, mmu + MMU_CTRL);			/* enable */

	pr_info("zrn_abox: its interrupt controller\n");
	writel(0xff, gicc + 0x004);			/* priority mask */
	writel(0x3, gicd + 0x000);
	for (i = 0; i < 40; i++)
		writel(0x10101010, gicd + 0x400 + i * 4);
	writel(0x3, gicc + 0x000);

	writel(0x1, sysreg + SYSREG_MISC_CON);

	pr_info("zrn_abox: firmware in (%zu, %zu, %zu bytes)\n", fw_sram->size, fw_dram->size, fw_iva->size);
	memcpy_toio(sram, fw_sram->data, fw_sram->size);
	memset_io(sram + fw_sram->size, 0, ABOX_SRAM_SIZE - fw_sram->size);
	memset(dram, 0, DRAM_SIZE);
	memcpy(dram, fw_dram->data, fw_dram->size);
	memset(iva, 0, IVA_SIZE);
	memset(vss, 0, VSS_SIZE);
	memset(vparam, 0, VPARAM_SIZE);
	fill_tone();
	memcpy(iva, fw_iva->data, fw_iva->size);
	release_firmware(fw_sram);
	release_firmware(fw_dram);
	release_firmware(fw_iva);

	pr_info("zrn_abox: core on\n");
	writel(0x1, sfr + ABOX_SYSPOWER_CTRL);		/* it may use its DRAM */
	pmu_update(PMU_ABOX_CPU_CFG, 0x1, 0x1);
	pmu_update(PMU_ABOX_CPU_OPTION, ABOX_CPU_ENABLE, ABOX_CPU_ENABLE);
	for (i = 0; i < 100 && !(readl(pmu + PMU_ABOX_CPU_STATUS) & 0x1); i++)
		usleep_range(1000, 2000);
	pr_info("zrn_abox: cpu status %08x after %d ms\n", readl(pmu + PMU_ABOX_CPU_STATUS), i);
	pmu_update(PMU_PAD_RET_ABOX, 0x10000000, 0x10000000);
	pmu_update(PMU_GPIO_MODE_ABOX, 0x1, 0x1);
	started = true;

	INIT_WORK(&play, play_fn);
	INIT_DELAYED_WORK(&watch, watch_fn);
	schedule_delayed_work(&watch, msecs_to_jiffies(50));
	return 0;

err_dev:
	pr_err("zrn_abox: not started (%d)\n", ret);
	platform_device_unregister(pdev);
	return ret ? ret : -EINVAL;
}

static void __exit zrn_abox_exit(void)
{
	cancel_delayed_work_sync(&watch);
	cancel_work_sync(&play);
	if (playing) {
		u32 t[5] = { IPC_PCMPLAYBACK, 0, PCM_PLTDAI_TRIGGER, 0, 0 };
		u32 c[5] = { IPC_PCMPLAYBACK, 0, PCM_PLTDAI_CLOSE, 0, 0 };

		ipc_send(t, 5);
		writel(readl(sfr + ABOX_UAIF1_CTRL0) & ~BIT(0), sfr + ABOX_UAIF1_CTRL0);
		ipc_send(c, 5);
	}
	if (rx_task)
		kthread_stop(rx_task);
	if (started) {
		/* the core off first, then what it was looking at */
		core_off();
		writel(0x0, sfr + ABOX_SYSPOWER_CTRL);
		writel(0x0, mmu + MMU_CTRL);
		writel(0x0, mmu + MMU_CFG);
	}
	dma_free_coherent(&pdev->dev, DRAM_SIZE, dram, dram_pa);
	dma_free_coherent(&pdev->dev, IVA_SIZE, iva, iva_pa);
	dma_free_coherent(&pdev->dev, VSS_SIZE, vss, vss_pa);
	dma_free_coherent(&pdev->dev, TONE_BYTES, pcm, pcm_pa);
	dma_free_coherent(&pdev->dev, VPARAM_SIZE, vparam, vparam_pa);
	dma_free_coherent(&pdev->dev, SZ_16K, lv1, lv1_pa);
	dma_free_coherent(&pdev->dev, LV2_TABLES * SZ_1K, lv2, lv2_pa);
	platform_device_unregister(pdev);
	pr_info("zrn_abox: stopped\n");
}

module_init(zrn_abox_init);
module_exit(zrn_abox_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Start the Exynos 9810 audio DSP and watch it boot");
