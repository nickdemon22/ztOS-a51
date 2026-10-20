// SPDX-License-Identifier: GPL-2.0
/*
 * snd-zrn-abox: sound for the Galaxy S9 (Exynos 9810) on a mainline kernel.
 *
 * One ALSA card with one playback device, 48 kHz, 16 bit, stereo, out of the
 * phone's two speakers.
 *
 * Every sound on this SoC passes through ABOX, an audio block built round a
 * Cortex-A7 that runs Samsung's "calliope" firmware. This driver starts that
 * firmware (calliope_sram.bin, calliope_dram.bin, calliope_iva.bin from the
 * phone's own vendor partition, in /lib/firmware), gives it one playback
 * stream (RDMA0) whose buffer is the ALSA buffer, routes that stream to the
 * UAIF1 port, and sets up the two MAX98512 amplifiers that listen there.
 *
 * It is a port by hand of the parts of Samsung's GPL driver that this needs
 * (sound/soc/samsung/abox/ and sound/soc/codecs/max98512.c in the
 * exynos9810 vendor kernel), not of its structure: no device tree node, no
 * clock or IOMMU framework. Addresses come from the stock device tree. The
 * audio power domain and PLL are as the bootloader leaves them (on, and at
 * 1179.648 MHz).
 *
 * The amplifiers' I2C controller is not in the mainline device tree either,
 * so the driver registers a bit-banged bus (i2c-gpio) on its two pins.
 *
 * NOT here: Maxim's speaker protection (DSM), which on the stock system runs
 * in the DSP against the current and voltage the amplifiers report. Until it
 * is, the volume control stops at -4 dB, the analogue gain is 6 dB under
 * Samsung's and the boost converter's output is set about half way to theirs.
 */
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/firmware.h>
#include <linux/gpio/machine.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/irqdomain.h>
#include <linux/kthread.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/platform_data/i2c-gpio.h>
#include <linux/platform_device.h>
#include <linux/sizes.h>
#include <sound/control.h>
#include <sound/core.h>
#include <sound/initval.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/tlv.h>

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


#define  PCM_PLTDAI_HW_FREE	15
#define  PCM_PLTDAI_POINTER	18
#define RDMA_STATUS		0x30
#define ABOX_SPI		76	/* the block's interrupt to us, on the main GIC */
#define AMP_BUS			98
#define AMP_L			0x39
#define AMP_R			0x3a
#define AMP_VOL_MAX		0x30	/* -4 dB; 0x40 is 0 dB. See the note on DSM above. */
/* the DSP's position register wraps at 64K: no ring larger than that */
#define BUFFER_BYTES		SZ_64K
#define  ABOX_CHANGE_GEAR	4
#define  ABOX_CHANGED_GEAR	9
#define CMU_DIV_PLL		0x1820
#define AUD_PLL_HZ		1179648000UL

static struct platform_device *pdev, *i2c_pdev;
static void __iomem *pmu, *sfr, *sysreg, *sram, *gicd, *gicc, *mmu, *cmu, *pins;
static void *dram, *iva, *vss, *vparam, *lv1, *lv2, *pcm;
static dma_addr_t dram_pa, iva_pa, vss_pa, vparam_pa, lv1_pa, lv2_pa, pcm_pa;
static int lv2_used = 1;	/* table 0 stays empty: what unmapped space points at */
static bool started, booted;
static u32 fw_version;
static DEFINE_SPINLOCK(ipc_lock);
static int abox_irq;
static struct task_struct *rx_task;	/* only when the interrupt could not be had */
static struct snd_card *card;
static struct snd_pcm_substream *active;	/* the open stream, for period interrupts */
static struct i2c_adapter *amp_bus;
static int amp_volume = 0x20;	/* -8 dB */
static bool amp_on;
static struct work_struct gear_work;
static int debug;
module_param(debug, int, 0644);
MODULE_PARM_DESC(debug, "log this many of the DSP's next messages");
static DEFINE_MUTEX(amp_lock);

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
 * interrupt on its controller whose number is the message's kind. Called with
 * interrupts off from the stream's trigger, so it only ever spins briefly. */
static int ipc_send(const u32 *words, int n)
{
	u32 msg[IPC_MSG_BYTES / 4] = { 0 };
	unsigned long flags;
	int i, ret = 0;

	if (!booted)
		return -ENODEV;
	memcpy(msg, words, n * 4);
	spin_lock_irqsave(&ipc_lock, flags);
	memcpy_toio(sram + IPC_TX, msg, sizeof(msg));
	writel(1, sram + IPC_TX_ACK);
	writel(BIT(16) | (words[0] & 0xf), gicd + 0xf00);
	for (i = 0; i < 2000 && readl(sram + IPC_TX_ACK); i++)
		udelay(5);
	if (readl(sram + IPC_TX_ACK)) {
		writel(0, sram + IPC_TX_ACK);
		ret = -EIO;
	}
	spin_unlock_irqrestore(&ipc_lock, flags);
	if (ret)
		pr_err("snd-zrn-abox: the DSP did not take message %u/%u\n", words[0], words[2]);
	return ret;
}

static int pcm_msg(u32 type, u32 a, u32 b, u32 c)
{
	u32 m[7] = { IPC_PCMPLAYBACK, 0, type, 0, a, b, c };

	return ipc_send(m, 7);
}

/*
 * The firmware keeps time by its own core's clock, and has to be told how
 * fast that is: it asks for a "gear" (a divider of the audio PLL) and waits to
 * hear what it got. Untold, it assumes the slowest gear, PLL / 12, while the
 * core here runs at whatever the bootloader left, PLL / 4, and so it played
 * everything three times too fast. The clock is left alone and the firmware
 * told the truth, at boot and every time it asks.
 */
static void gear_tell(struct work_struct *work)
{
	u64 now = ktime_get_ns();
	u32 hz = AUD_PLL_HZ / ((readl(cmu + CMU_DIV_PLL) & 0xf) + 1);
	u32 m[6] = { IPC_SYSTEM, 0, ABOX_CHANGED_GEAR, hz, 0, 0 };

	m[5] = do_div(now, NSEC_PER_SEC);
	m[4] = (u32)now;
	ipc_send(m, 6);
}

/* Whatever the DSP has put in its mailbox for us: a period has been played,
 * or housekeeping of its own that needs no answer here. */
static void rx_one(void)
{
	u32 id, type;

	if (!readl(sram + IPC_RX_ACK))
		return;
	id = readl(sram + IPC_RX);
	type = readl(sram + IPC_RX + 8);
	if (debug > 0) {
		debug--;
		pr_info("snd-zrn-abox: from DSP: id %u task %d: %u %08x %08x %08x\n", id, readl(sram + IPC_RX + 4),
			type, readl(sram + IPC_RX + 12), readl(sram + IPC_RX + 16), readl(sram + IPC_RX + 20));
	}
	writel(0, sram + IPC_RX_ACK);
	if (id == IPC_SYSTEM && type == 3) {
		fw_version = readl(sram + IPC_RX + 20);
		booted = true;
	} else if (id == IPC_SYSTEM && type == ABOX_CHANGE_GEAR) {
		schedule_work(&gear_work);
	} else if (id == IPC_PCMPLAYBACK && type == PCM_PLTDAI_POINTER) {
		struct snd_pcm_substream *s = READ_ONCE(active);

		if (s)
			snd_pcm_period_elapsed(s);
	}
}

/* The DSP's interrupts arrive through its own controller: acknowledge them
 * there, as Samsung's driver does. Their numbers say what happened: 11 is
 * "stream 0 has played a period" and carries no message; the rest come with
 * something in the mailbox. */
#define RDMA0_BUF_EMPTY		0xb
static irqreturn_t abox_isr(int irq, void *unused)
{
	irqreturn_t ret = IRQ_NONE;
	bool period = false;
	int i;

	for (i = 0; i < 32; i++) {
		u32 stat = readl(gicc + 0x00c), nr = stat & 0x3ff;

		if (nr >= 1020)
			break;
		writel(stat, gicc + 0x010);
		if (nr < 16)
			writel(stat, gicc + 0x1000);
		if (nr == RDMA0_BUF_EMPTY)
			period = true;
		if (debug > 0) {
			debug--;
			pr_info("snd-zrn-abox: DSP interrupt %u (%08x)\n", nr, stat);
		}
		ret = IRQ_HANDLED;
	}
	if (period) {
		struct snd_pcm_substream *s = READ_ONCE(active);

		if (s)
			snd_pcm_period_elapsed(s);
	}
	rx_one();
	return ret;
}

static int rx_fn(void *unused)
{
	while (!kthread_should_stop()) {
		rx_one();
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

/* ---- the amplifiers ------------------------------------------------------ */

static void amp_write(u8 addr, u16 reg, u8 val)
{
	u8 buf[3] = { reg >> 8, reg & 0xff, val };
	struct i2c_msg msg = { .addr = addr, .len = 3, .buf = buf };

	if (!amp_bus)
		amp_bus = i2c_get_adapter(AMP_BUS);
	if (amp_bus && i2c_transfer(amp_bus, &msg, 1) != 1)
		pr_warn_ratelimited("snd-zrn-abox: amplifier %02x did not take %04x\n", addr, reg);
}

static void amp_both(u16 reg, u8 val)
{
	amp_write(AMP_L, reg, val);
	amp_write(AMP_R, reg, val);
}

/* Samsung's set-up, quietened (see the top of the file). */
static void amp_set(bool on)
{
	static const struct { u16 reg; u8 val; } init[] = {
		{ 0x003c, 0x00 },	/* no current/voltage measurement: nothing reads it */
		/* The measurement ADC (battery, boost output, temperature), set up
		 * as Samsung does. It has to run: both times sound was heard it
		 * had been switched on by hand, and without it there was none. */
		{ 0x0041, 0x07 }, { 0x0042, 0x00 }, { 0x0043, 0x0b },
		{ 0x0044, 0x02 }, { 0x0045, 0x02 }, { 0x0046, 0x02 },
		{ 0x0047, 0x0c }, { 0x0048, 0x0c }, { 0x0049, 0x0c },
		{ 0x001c, 0xff }, { 0x001d, 0xff },	/* their own data output: high impedance */
		{ 0x0020, 0x40 },	/* I2S, 16 bits a channel */
		{ 0x0021, 0x00 },	/* clocks come from the SoC */
		{ 0x0022, 0x02 },	/* 32 bit clocks a frame */
		{ 0x0023, 0x08 }, { 0x0024, 0x88 },	/* 48 kHz */
		{ 0x003a, 0x03 },	/* analogue gain: +9 dB (Samsung: +15; +3 is its earpiece setting) */
		{ 0x0036, 0x01 },	/* DC blocker */
		/*
		 * The boost converter that supplies the output stage. Left in its
		 * reset state (bypass) on this revision of the chip the stage has
		 * no supply at all: the chip's own ADC reads 0 for it, everything
		 * else reports fine, and nothing is heard. With bypass off it
		 * reads 0x44 at output code 0, 0x94 at 0x10 and 0xcf at 0x1c
		 * (Samsung's). Envelope tracking, which Samsung enables to lower it
		 * when the signal is small, is left off: here it took the supply
		 * straight down to the level at which nothing is heard.
		 */
		{ 0x0086, 0x05 }, { 0x0087, 0x02 }, { 0x0088, 0x01 },	/* bypass off */
		{ 0x003e, 0x10 },	/* boost output: about half way to Samsung's 0x1c */
		{ 0x0040, 0x3e }, { 0x003f, 0x01 },
		{ 0x0084, 0x00 },	/* no envelope tracking */
		{ 0x004e, 0x10 },	/* squelch */
		/* Brown-out limiting off. Enabled with Samsung's thresholds but
		 * without the per-level current limits and gains that go with
		 * them, it sat engaged (status 5) and nothing came out. */
		{ 0x0050, 0x00 },
		{ 0x0011, 0x00 },
	};
	int i;

	mutex_lock(&amp_lock);
	if (on && !amp_on) {
		amp_both(0x0401, 0x01);		/* software reset */
		usleep_range(20000, 30000);
		for (i = 0; i < ARRAY_SIZE(init); i++)
			amp_both(init[i].reg, init[i].val);
		amp_write(AMP_L, 0x0018, 0x01);	/* left takes channel 0 */
		amp_write(AMP_L, 0x0025, 0x00);
		amp_write(AMP_L, 0x0026, 0x00);
		amp_write(AMP_R, 0x0018, 0x02);	/* right takes channel 1 */
		amp_write(AMP_R, 0x0025, 0x41);
		amp_write(AMP_R, 0x0026, 0x01);
		amp_both(0x0035, amp_volume);
		amp_both(0x0038, 0x81);		/* amplifier on */
		amp_both(0x0400, 0x01);		/* out of shutdown; it waits for the clocks */
	} else if (!on && amp_on) {
		amp_both(0x0400, 0x00);
		amp_both(0x0038, 0x80);
	}
	amp_on = on;
	mutex_unlock(&amp_lock);
}

static struct gpiod_lookup_table amp_gpios = {
	.dev_id = "i2c-gpio.98",
	.table = {
		/* data on gpp4-5, clock on gpp4-4: found by trying */
		GPIO_LOOKUP_IDX("gpp4", 5, NULL, 0, GPIO_ACTIVE_HIGH | GPIO_OPEN_DRAIN),
		GPIO_LOOKUP_IDX("gpp4", 4, NULL, 1, GPIO_ACTIVE_HIGH | GPIO_OPEN_DRAIN),
		{ }
	},
};
static struct i2c_gpio_platform_data amp_i2c = { .udelay = 5 };	/* about 100 kHz */

/* ---- ALSA ----------------------------------------------------------------- */

static const struct snd_pcm_hardware abox_hw = {
	.info = SNDRV_PCM_INFO_INTERLEAVED | SNDRV_PCM_INFO_BLOCK_TRANSFER |
		SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_MMAP_VALID,
	.formats = SNDRV_PCM_FMTBIT_S16_LE,
	.rates = SNDRV_PCM_RATE_48000,
	.rate_min = RATE, .rate_max = RATE,
	.channels_min = 2, .channels_max = 2,
	.buffer_bytes_max = BUFFER_BYTES,
	.period_bytes_min = SZ_1K, .period_bytes_max = BUFFER_BYTES / 2,
	.periods_min = 2, .periods_max = 64,
};

static int abox_pcm_open(struct snd_pcm_substream *s)
{
	s->runtime->hw = abox_hw;
	/* the DSP takes the period size in bytes and its ring in whole periods */
	snd_pcm_hw_constraint_integer(s->runtime, SNDRV_PCM_HW_PARAM_PERIODS);
	snd_pcm_hw_constraint_step(s->runtime, 0, SNDRV_PCM_HW_PARAM_PERIOD_BYTES, 16);
	return pcm_msg(PCM_PLTDAI_OPEN, 0, 0, 0);
}

static int abox_pcm_close(struct snd_pcm_substream *s)
{
	WRITE_ONCE(active, NULL);
	amp_set(false);
	pcm_msg(PCM_PLTDAI_CLOSE, 0, 0, 0);
	return 0;
}

static int abox_pcm_hw_params(struct snd_pcm_substream *s, struct snd_pcm_hw_params *p)
{
	struct snd_pcm_runtime *rt = s->runtime;
	int ret;

	/* always the same memory: the DSP sees it at IOVA_RDMA0 */
	rt->dma_area = pcm;
	rt->dma_addr = pcm_pa;
	rt->dma_bytes = params_buffer_bytes(p);
	ret = pcm_msg(PCM_SET_BUFFER, IOVA_RDMA0, params_period_bytes(p), params_periods(p));
	if (!ret)
		ret = pcm_msg(PCM_PLTDAI_HW_PARAMS, params_rate(p), 16, params_channels(p));
	return ret;
}

static int abox_pcm_hw_free(struct snd_pcm_substream *s)
{
	WRITE_ONCE(active, NULL);
	pcm_msg(PCM_PLTDAI_HW_FREE, 0, 0, 0);
	s->runtime->dma_area = NULL;
	s->runtime->dma_addr = 0;
	s->runtime->dma_bytes = 0;
	return 0;
}

static int abox_pcm_prepare(struct snd_pcm_substream *s)
{
	memset(pcm, 0, BUFFER_BYTES);
	WRITE_ONCE(active, s);
	amp_set(true);
	return pcm_msg(PCM_PLTDAI_PREPARE, 0, 0, 0);
}

static int abox_pcm_trigger(struct snd_pcm_substream *s, int cmd)
{
	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		writel(readl(sfr + ABOX_UAIF1_CTRL0) | BIT(0), sfr + ABOX_UAIF1_CTRL0);
		return pcm_msg(PCM_PLTDAI_TRIGGER, 1, 0, 0);
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		return pcm_msg(PCM_PLTDAI_TRIGGER, 0, 0, 0);
	}
	return -EINVAL;
}

/* Where the DSP is reading. Its status register gives the start of the period
 * it is in and how far into that period it has got, both in units of 16
 * bytes. (Samsung's abox_rdma_pointer() takes the second as bytes, which
 * leaves the position crawling for a period and then jumping; PipeWire, which
 * schedules by the position rather than by period interrupts, then misses
 * every deadline. Measured here: the second field never exceeds 0x100 with
 * 4096-byte periods.) */
static snd_pcm_uframes_t abox_pcm_pointer(struct snd_pcm_substream *s)
{
	u32 status = readl(sfr + ABOX_RDMA0 + RDMA_STATUS);
	size_t buffer = snd_pcm_lib_buffer_bytes(s);
	size_t pos;

	if (!(status & BIT(31)) || !buffer)
		return 0;
	pos = ((size_t)((status >> 16) & 0x1fff) << 4) + ((size_t)(status & 0x1fff) << 4);
	return bytes_to_frames(s->runtime, pos % buffer);
}

static int abox_pcm_mmap(struct snd_pcm_substream *s, struct vm_area_struct *vma)
{
	return dma_mmap_coherent(&pdev->dev, vma, pcm, pcm_pa, BUFFER_BYTES);
}

static const struct snd_pcm_ops abox_pcm_ops = {
	.open = abox_pcm_open,
	.close = abox_pcm_close,
	.hw_params = abox_pcm_hw_params,
	.hw_free = abox_pcm_hw_free,
	.prepare = abox_pcm_prepare,
	.trigger = abox_pcm_trigger,
	.pointer = abox_pcm_pointer,
	.mmap = abox_pcm_mmap,
};

static int vol_info(struct snd_kcontrol *k, struct snd_ctl_elem_info *info)
{
	info->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	info->count = 1;
	info->value.integer.min = 0;
	info->value.integer.max = AMP_VOL_MAX;
	return 0;
}

static int vol_get(struct snd_kcontrol *k, struct snd_ctl_elem_value *v)
{
	v->value.integer.value[0] = amp_volume;
	return 0;
}

static int vol_put(struct snd_kcontrol *k, struct snd_ctl_elem_value *v)
{
	int val = clamp_t(int, v->value.integer.value[0], 0, AMP_VOL_MAX);

	if (val == amp_volume)
		return 0;
	mutex_lock(&amp_lock);
	amp_volume = val;
	if (amp_on)
		amp_both(0x0035, val);
	mutex_unlock(&amp_lock);
	return 1;
}

/* the amplifier's digital volume: 0.25 dB a step, 0x40 being 0 dB */
static const DECLARE_TLV_DB_SCALE(vol_tlv, -1600, 25, 0);

static const struct snd_kcontrol_new vol_control = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "Speaker Playback Volume",
	.access = SNDRV_CTL_ELEM_ACCESS_READWRITE | SNDRV_CTL_ELEM_ACCESS_TLV_READ,
	.info = vol_info, .get = vol_get, .put = vol_put,
	.tlv.p = vol_tlv,
};

/* ---- starting and stopping ------------------------------------------------ */

static int map_irq(void)
{
	struct device_node *gic = of_find_compatible_node(NULL, NULL, "arm,gic-400");
	struct irq_fwspec spec = { .param_count = 3, .param = { 0, ABOX_SPI, IRQ_TYPE_LEVEL_HIGH } };
	int irq;

	if (!gic)
		return -ENODEV;
	spec.fwnode = of_fwnode_handle(gic);
	irq = irq_create_fwspec_mapping(&spec);
	of_node_put(gic);
	return irq > 0 ? irq : -EINVAL;
}

static void teardown(void)
{
	if (card)
		snd_card_free(card);
	cancel_work_sync(&gear_work);
	if (abox_irq > 0)
		free_irq(abox_irq, &abox_irq);
	if (rx_task)
		kthread_stop(rx_task);
	if (amp_bus) {
		amp_set(false);
		i2c_put_adapter(amp_bus);
	}
	if (!IS_ERR_OR_NULL(i2c_pdev)) {
		platform_device_unregister(i2c_pdev);
		gpiod_remove_lookup_table(&amp_gpios);
	}
	if (started) {
		/* the core off first, then what it was looking at */
		core_off();
		writel(0x0, sfr + ABOX_SYSPOWER_CTRL);
		writel(0x0, mmu + MMU_CTRL);
		writel(0x0, mmu + MMU_CFG);
	}
	if (dram)
		dma_free_coherent(&pdev->dev, DRAM_SIZE, dram, dram_pa);
	if (iva)
		dma_free_coherent(&pdev->dev, IVA_SIZE, iva, iva_pa);
	if (vss)
		dma_free_coherent(&pdev->dev, VSS_SIZE, vss, vss_pa);
	if (vparam)
		dma_free_coherent(&pdev->dev, VPARAM_SIZE, vparam, vparam_pa);
	if (pcm)
		dma_free_coherent(&pdev->dev, BUFFER_BYTES, pcm, pcm_pa);
	if (lv1)
		dma_free_coherent(&pdev->dev, SZ_16K, lv1, lv1_pa);
	if (lv2)
		dma_free_coherent(&pdev->dev, LV2_TABLES * SZ_1K, lv2, lv2_pa);
	platform_device_unregister(pdev);
}

static int __init abox_init(void)
{
	const struct firmware *fw_sram = NULL, *fw_dram = NULL, *fw_iva = NULL;
	struct snd_pcm *snd_pcm;
	struct device *dev;
	u32 m[6] = { 0 };
	u32 *l1;
	char v[4];
	int i, ret;

	INIT_WORK(&gear_work, gear_tell);
	pdev = platform_device_register_simple("snd-zrn-abox", -1, NULL, 0);
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
		pr_err("snd-zrn-abox: the calliope firmware is not in /lib/firmware (%d)\n", ret);
		ret = ret ? ret : -EINVAL;
		goto err;
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
		goto err;

	ret = -ENODEV;
	if ((readl(pmu + PMU_AUD_STATUS) & 0xf) != 0xf || readl(sfr + ABOX_IP_ID) != 0x41424f58) {
		pr_err("snd-zrn-abox: the audio block is not powered\n");
		goto err;
	}

	/* uncached, contiguous: what the DSP reads is what was written */
	ret = -ENOMEM;
	dram = dma_alloc_coherent(dev, DRAM_SIZE, &dram_pa, GFP_KERNEL);
	iva = dma_alloc_coherent(dev, IVA_SIZE, &iva_pa, GFP_KERNEL);
	vss = dma_alloc_coherent(dev, VSS_SIZE, &vss_pa, GFP_KERNEL);
	vparam = dma_alloc_coherent(dev, VPARAM_SIZE, &vparam_pa, GFP_KERNEL);
	pcm = dma_alloc_coherent(dev, BUFFER_BYTES, &pcm_pa, GFP_KERNEL);
	lv1 = dma_alloc_coherent(dev, SZ_16K, &lv1_pa, GFP_KERNEL);
	lv2 = dma_alloc_coherent(dev, LV2_TABLES * SZ_1K, &lv2_pa, GFP_KERNEL);
	if (!dram || !iva || !vss || !vparam || !pcm || !lv1 || !lv2)
		goto err;

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
		ret = mmu_map(IOVA_RDMA0, pcm_pa, BUFFER_BYTES);
	if (!ret)
		ret = mmu_map(0x10000000, 0x10000000, SZ_4K);	/* the chip ID, as Samsung's driver does */
	if (ret)
		goto err;

	core_off();	/* a fresh start, whatever ran before */
	writel(0x7, mmu + MMU_CTRL);			/* block */
	writel(readl(mmu + MMU_INT_STATUS), mmu + MMU_INT_CLEAR);
	writel((15 << 7) | BIT(20) | BIT(24) | BIT(2), mmu + MMU_CFG);
	writel((u32)(lv1_pa >> 12), mmu + MMU_PT_BASE);
	writel(1, mmu + MMU_FLUSH_ALL);
	writel(0x5, mmu + MMU_CTRL);			/* enable */

	/* the DSP's own interrupt controller */
	writel(0xff, gicc + 0x004);
	writel(0x3, gicd + 0x000);
	for (i = 0; i < 40; i++)
		writel(0x10101010, gicd + 0x400 + i * 4);
	writel(0x3, gicc + 0x000);
	writel(0x1, sysreg + SYSREG_MISC_CON);

	memcpy_toio(sram, fw_sram->data, fw_sram->size);
	memset_io(sram + fw_sram->size, 0, ABOX_SRAM_SIZE - fw_sram->size);
	memset(dram, 0, DRAM_SIZE);
	memcpy(dram, fw_dram->data, fw_dram->size);
	memset(iva, 0, IVA_SIZE);
	memcpy(iva, fw_iva->data, fw_iva->size);
	memset(vss, 0, VSS_SIZE);
	memset(vparam, 0, VPARAM_SIZE);
	memset(pcm, 0, BUFFER_BYTES);

	/* its interrupt to us; without it, the mailbox is looked at every millisecond */
	abox_irq = map_irq();
	if (abox_irq > 0 && request_irq(abox_irq, abox_isr, 0, "snd-zrn-abox", &abox_irq))
		abox_irq = -1;
	if (abox_irq <= 0) {
		pr_warn("snd-zrn-abox: no interrupt (%d): polling\n", abox_irq);
		rx_task = kthread_run(rx_fn, NULL, "snd-zrn-abox");
		if (IS_ERR(rx_task)) {
			ret = PTR_ERR(rx_task);
			rx_task = NULL;
			goto err;
		}
	}

	writel(0x1, sfr + ABOX_SYSPOWER_CTRL);		/* it may use its DRAM */
	pmu_update(PMU_ABOX_CPU_CFG, 0x1, 0x1);
	pmu_update(PMU_ABOX_CPU_OPTION, ABOX_CPU_ENABLE, ABOX_CPU_ENABLE);
	pmu_update(PMU_PAD_RET_ABOX, 0x10000000, 0x10000000);
	pmu_update(PMU_GPIO_MODE_ABOX, 0x1, 0x1);
	started = true;

	for (i = 0; i < 400 && !booted; i++) {
		msleep(10);
		if (abox_irq > 0 && i > 200)
			rx_one();	/* the interrupt is not arriving: look anyway */
	}
	ret = -ETIMEDOUT;
	if (!booted) {
		pr_err("snd-zrn-abox: the DSP did not boot (pc %08x, iommu fault %08x at %08x)\n",
			readl(sfr + ABOX_CPU_PC), readl(mmu + MMU_INT_STATUS), readl(mmu + MMU_FAULT_VA));
		goto err;
	}
	memcpy(v, &fw_version, 4);
	pr_info("snd-zrn-abox: DSP firmware %c%c%c%c running, %s\n", v[3], v[2], v[1], v[0],
		abox_irq > 0 ? "interrupt driven" : "polled");

	/* resume, then the mixer everything is summed in: 48 kHz, 16 bit, stereo */
	m[0] = IPC_SYSTEM; m[2] = ABOX_RESUME;
	ipc_send(m, 6);
	msleep(20);
	gear_tell(NULL);
	pr_info("snd-zrn-abox: DSP core clock %lu Hz\n", AUD_PLL_HZ / ((readl(cmu + CMU_DIV_PLL) & 0xf) + 1));
	m[0] = IPC_ABOX_CONFIG; m[2] = SET_MIXER_SAMPLE_RATE; m[3] = RATE;
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
	/* gpb1-0..3 to the I2S function */
	writel((readl(pins + GPB1_CON) & ~0xffffU) | 0x2222, pins + GPB1_CON);
	/* I2S, 16 bits in 16-bit slots, two of them; the SoC drives the clocks */
	writel((9 << 24) | BIT(23) | (1 << 18) | (15 << 12), sfr + ABOX_UAIF1_CTRL1);
	writel(readl(sfr + ABOX_UAIF1_CTRL0) | BIT(2), sfr + ABOX_UAIF1_CTRL0);

	/* the amplifiers' bus */
	gpiod_add_lookup_table(&amp_gpios);
	i2c_pdev = platform_device_register_data(NULL, "i2c-gpio", AMP_BUS, &amp_i2c, sizeof(amp_i2c));
	if (IS_ERR(i2c_pdev)) {
		gpiod_remove_lookup_table(&amp_gpios);
		ret = PTR_ERR(i2c_pdev);
		goto err;
	}
	for (i = 0; i < 20 && !amp_bus; i++) {
		amp_bus = i2c_get_adapter(AMP_BUS);
		if (!amp_bus)
			msleep(10);
	}
	if (!amp_bus)
		pr_warn("snd-zrn-abox: no amplifier bus (is i2c-gpio loaded?): will retry on playback\n");

	ret = snd_card_new(dev, SNDRV_DEFAULT_IDX1, "ZrnAbox", THIS_MODULE, 0, &card);
	if (ret)
		goto err;
	strscpy(card->driver, "snd-zrn-abox", sizeof(card->driver));
	strscpy(card->shortname, "Galaxy S9 speakers", sizeof(card->shortname));
	snprintf(card->longname, sizeof(card->longname), "Galaxy S9 speakers (ABOX %c%c%c%c, MAX98512)",
		v[3], v[2], v[1], v[0]);
	ret = snd_pcm_new(card, "Speakers", 0, 1, 0, &snd_pcm);
	if (!ret) {
		strscpy(snd_pcm->name, "Speakers", sizeof(snd_pcm->name));
		snd_pcm_set_ops(snd_pcm, SNDRV_PCM_STREAM_PLAYBACK, &abox_pcm_ops);
		ret = snd_ctl_add(card, snd_ctl_new1(&vol_control, NULL));
	}
	if (!ret)
		ret = snd_card_register(card);
	if (ret)
		goto err;
	release_firmware(fw_sram);
	release_firmware(fw_dram);
	release_firmware(fw_iva);
	return 0;

err:
	release_firmware(fw_sram);
	release_firmware(fw_dram);
	release_firmware(fw_iva);
	teardown();
	return ret;
}

static void __exit abox_exit(void)
{
	teardown();
}

module_init(abox_init);
module_exit(abox_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Galaxy S9 (Exynos 9810) speakers: ABOX DSP and MAX98512 amplifiers");
MODULE_FIRMWARE("calliope_sram.bin");
MODULE_FIRMWARE("calliope_dram.bin");
MODULE_FIRMWARE("calliope_iva.bin");
MODULE_SOFTDEP("pre: i2c-gpio snd-pcm");
