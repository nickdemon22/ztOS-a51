// SPDX-License-Identifier: GPL-2.0-only
/* Host tests: no phone or physical register access. */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include "sequence.h"
#include "sleep-tables.h"

struct fake { unsigned int reads, writes, delays; zrn_u32 value, last; };
static zrn_u32 fake_read(void *p, zrn_u32 base, zrn_u32 offset)
{
	struct fake *f = p;
	f->reads++;
	return f->value;
}
static void fake_write(void *p, zrn_u32 base, zrn_u32 offset, zrn_u32 value)
{
	struct fake *f = p;
	f->writes++;
	f->last = value;
}
static void fake_delay(void *p, unsigned int usec) { ((struct fake *)p)->delays += usec; }
#define COUNT(a) (sizeof(a) / sizeof((a)[0]))
int main(void)
{
	struct fake f = { .value = ~0U };
	struct zrn_seq_io io = { &f, fake_read, fake_write, fake_delay };
	struct zrn_seq item = { .op = PMUCAL_COND_SAVE_RESTORE, .base = 1,
		.mask = 0xff, .cond_base = 2, .cond_mask = 0xf, .cond_value = 0xf };
	unsigned int failed, i, valid = 0;

	assert(COUNT(save_sleep) == 1103 && COUNT(save_sleep_usbl2) == 1105);
	assert(COUNT(enter_sleep) == 3 && COUNT(exit_sleep) == 40);
	assert(COUNT(earlywkup_sleep) == 4 && COUNT(enter_sleep_usbl2) == 10);
	assert(COUNT(exit_sleep_usbl2) == 46 && COUNT(earlywkup_sleep_usbl2) == 4);
	assert(!zrn_sequence_run(save_sleep, COUNT(save_sleep), ZRN_SAVE, &io, &failed));
	assert(f.writes == 0);
	for (i = 0; i < COUNT(save_sleep); i++)
		valid += save_sleep[i].valid;
	assert(valid == 646); /* 457 debug/status READ entries must never restore. */
	assert(!zrn_sequence_run(save_sleep, COUNT(save_sleep), ZRN_RESTORE, &io, &failed));
	assert(f.writes == valid);
	assert(!zrn_sequence_run(save_sleep, COUNT(save_sleep), ZRN_RESTORE, &io, &failed));
	assert(f.writes == valid); /* no stale second restore */

	f = (struct fake) { .value = 0 };
	assert(!zrn_sequence_run(&item, 1, ZRN_SAVE, &io, &failed));
	assert(f.reads == 1 && !item.valid); /* powered-off target never read */
	f.value = 0xf;
	assert(!zrn_sequence_run(&item, 1, ZRN_SAVE, &io, &failed));
	f.value = 0xab00f00f;
	assert(!zrn_sequence_run(&item, 1, ZRN_RESTORE, &io, &failed));
	assert(f.last == 0xab00f00f); /* preserve every unmasked bit */
	f.value = 0;
	assert(!zrn_sequence_run(&item, 1, ZRN_SAVE, &io, &failed));
	f.value = 0xf;
	i = f.writes;
	assert(!zrn_sequence_run(&item, 1, ZRN_RESTORE, &io, &failed));
	assert(f.writes == i); /* stale powered-on snapshot was invalidated */

	item = (struct zrn_seq) { .op = PMUCAL_WAIT, .mask = 1, .value = 1 };
	f.value = 0;
	assert(!zrn_sequence_run(&item, 1, ZRN_SAVE, &io, &failed));
	assert(zrn_sequence_run(&item, 1, ZRN_RESTORE, &io, &failed) == -ETIMEDOUT);
	assert(f.delays == 1001 && failed == 0);
	f.value = 1;
	assert(!zrn_sequence_run(&item, 1, ZRN_RESTORE, &io, &failed));
	item.op = (enum zrn_seq_op)99;
	assert(zrn_sequence_run(&item, 1, ZRN_HANDLE, &io, &failed) == -EINVAL);
	puts("PASS: exact sleep tables, conditional access, masked restore, stale state, timeout");
	return 0;
}
