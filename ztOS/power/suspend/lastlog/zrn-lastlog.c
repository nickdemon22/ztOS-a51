// SPDX-License-Identifier: GPL-2.0
/*
 * zrn-lastlog: the kernel log of the boot before, whatever booted between.
 *
 * pstore's ramoops region (0xfed10000) is shared with TWRP, which writes its
 * own records over it, and after a hang the S9 may boot TWRP first. This
 * module is a console that copies every line, as it is printed, into 64 KiB
 * at 0xff580000: inside Samsung's debug-snapshot area, which TWRP reserves
 * but leaves unused (its last log ends at 0xff518000), and which our device
 * tree keeps out of Linux's memory (snapshot-after-pstore, no-map). Writes go
 * through an uncached mapping, so a line is in DRAM before the next one.
 *
 * Loading it again hands over what the previous load wrote, in
 * /proc/zrn-lastlog-previous, then starts over.
 */
#include <linux/console.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#define MAGIC	0x5a4c4c47	/* "ZLLG" */

static unsigned long phys = 0xff580000;
module_param(phys, ulong, 0444);
static unsigned int size = 0x10000;
module_param(size, uint, 0444);
static unsigned int previous_bytes;
module_param(previous_bytes, uint, 0444);

struct header {
	u32 magic;
	u32 head;	/* where the next byte goes, in the ring */
	u32 wrapped;
	u32 sum;	/* magic ^ head ^ wrapped: a torn header is not trusted */
};

static void __iomem *base;
static char *ring_previous;
static unsigned int ring;
static DEFINE_RAW_SPINLOCK(lock);

static void put_header(u32 head, u32 wrapped)
{
	writel(head, base + offsetof(struct header, head));
	writel(wrapped, base + offsetof(struct header, wrapped));
	writel(MAGIC ^ head ^ wrapped, base + offsetof(struct header, sum));
	writel(MAGIC, base + offsetof(struct header, magic));
}

static void lastlog_write(struct console *con, const char *s, unsigned int n)
{
	unsigned long flags;
	u32 head, wrapped;

	raw_spin_lock_irqsave(&lock, flags);
	head = readl(base + offsetof(struct header, head));
	wrapped = readl(base + offsetof(struct header, wrapped));
	while (n) {
		unsigned int part = min(n, ring - head);

		memcpy_toio(base + sizeof(struct header) + head, s, part);
		s += part;
		n -= part;
		head += part;
		if (head == ring) {
			head = 0;
			wrapped = 1;
		}
	}
	put_header(head, wrapped);
	mb();
	raw_spin_unlock_irqrestore(&lock, flags);
}

static struct console lastlog_console = {
	.name = "zrnlast",
	.write = lastlog_write,
	.flags = CON_ENABLED | CON_ANYTIME,
	.index = -1,
};

static int previous_show(struct seq_file *m, void *v)
{
	seq_write(m, ring_previous, previous_bytes);
	return 0;
}

static void take_previous(void)
{
	u32 magic = readl(base), head = readl(base + 4), wrapped = readl(base + 8);
	u32 sum = readl(base + 12);

	if (magic != MAGIC || sum != (magic ^ head ^ wrapped) || head >= ring || wrapped > 1)
		return;
	ring_previous = kmalloc(ring, GFP_KERNEL);
	if (!ring_previous)
		return;
	if (wrapped) {
		memcpy_fromio(ring_previous, base + sizeof(struct header) + head, ring - head);
		memcpy_fromio(ring_previous + ring - head, base + sizeof(struct header), head);
		previous_bytes = ring;
	} else {
		memcpy_fromio(ring_previous, base + sizeof(struct header), head);
		previous_bytes = head;
	}
}

static int __init lastlog_init(void)
{
	if (size < 0x1000 || size & 3)
		return -EINVAL;
	base = ioremap(phys, size);
	if (!base)
		return -ENOMEM;
	ring = size - sizeof(struct header);
	take_previous();
	if (ring_previous)
		proc_create_single("zrn-lastlog-previous", 0400, NULL, previous_show);
	put_header(0, 0);
	register_console(&lastlog_console);
	pr_info("zrn-lastlog: logging to %#lx (%u bytes); previous log %u bytes\n",
		phys, size, previous_bytes);
	return 0;
}

static void __exit lastlog_exit(void)
{
	unregister_console(&lastlog_console);
	remove_proc_entry("zrn-lastlog-previous", NULL);
	kfree(ring_previous);
	iounmap(base);
}

module_init(lastlog_init);
module_exit(lastlog_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("The S9's kernel log of the boot before, kept where TWRP does not write");
