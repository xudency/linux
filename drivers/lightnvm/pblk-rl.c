/*
 * Copyright (C) 2016 CNEX Labs
 * Initial release: Javier Gonzalez <jg@lightnvm.io>
 *                  Matias Bjorling <m@bjorling.me>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version
 * 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * pblk-rl.c - pblk's rate limiter for user I/O
 */

#include "pblk.h"

/*
 * Increment 'v', if 'v' is below 'below'. Returns true if we succeeded,
 * false if 'v' + 1 would be bigger than 'below'.
 */
static bool atomic_inc_below(atomic_t *v, int below, int inc)
{
	int cur = atomic_read(v);

	for (;;) {
		int old;

		if (cur + inc > below)
			return false;
		old = atomic_cmpxchg(v, cur, cur + inc);
		if (likely(old == cur))
			break;
		cur = old;
	}

	return true;
}

static inline bool __pblk_rl_rate_user_bw(struct pblk *pblk, int nr_entries)
{
	return atomic_inc_below(&pblk->inflight_user, pblk->write_cur_speed,
								nr_entries);
}

static void pblk_rl_rate_user_bw(struct pblk *pblk, int nr_entries)
{
	DEFINE_WAIT(wait);

	if (__pblk_rl_rate_user_bw(pblk, nr_entries))
		return;

	do {
		prepare_to_wait_exclusive(&pblk->wait, &wait,
						TASK_UNINTERRUPTIBLE);

		if (__pblk_rl_rate_user_bw(pblk, nr_entries))
			break;

		io_schedule();
	} while (1);

	finish_wait(&pblk->wait, &wait);
}

static inline bool __pblk_rl_rate_user_rb(struct pblk *pblk, int nr_entries)
{
	return atomic_inc_below(&pblk->rl.rb_user_cnt, pblk->rl.rb_user_max,
								nr_entries);
}

static void pblk_rl_rate_user_rb(struct pblk *pblk, int nr_entries)
{
	DEFINE_WAIT(wait);

	if (__pblk_rl_rate_user_rb(pblk, nr_entries))
		return;

	do {
		prepare_to_wait_exclusive(&pblk->wait2, &wait,
						TASK_UNINTERRUPTIBLE);

		if (__pblk_rl_rate_user_rb(pblk, nr_entries))
			break;

		io_schedule();
	} while (1);

	finish_wait(&pblk->wait2, &wait);
}

void pblk_rl_rate_user_io(struct pblk *pblk, int nr_entries)
{
	/* pblk_rl_rate_user_bw(pblk, nr_entries); */
	pblk_rl_rate_user_rb(pblk, nr_entries);
}

void pblk_rl_rate_gc_io(struct pblk *pblk, int nr_entries)
{

}

void pblk_rl_update_rates(struct pblk *pblk)
{
	int i;
	unsigned int avail = 0;
	struct pblk_lun *rlun;
	int high, low;

	for (i = 0; i < pblk->nr_luns; i++) {
		rlun = &pblk->luns[i];
		spin_lock(&rlun->lock);
		avail += rlun->mgmt->nr_free_blocks;
		spin_unlock(&rlun->lock);
	}

	high = pblk->total_blocks / PBLK_USER_HIGH_THRS;
	low = pblk->total_blocks / PBLK_USER_LOW_THRS;

	if (avail > high)
		pblk->write_cur_speed = pblk->write_max_speed;
	else if (avail > low && avail < high)
	{
		/* redo to power of two calculations */
		int perc = ((avail * 100)) / (high - low);
		pblk->write_cur_speed = (pblk->write_max_speed / 100) * perc;
	} else {
		pblk->write_cur_speed = 0;
	}
}

int pblk_rl_calc_max_wr_speed(struct pblk *pblk)
{
	struct nvm_dev *dev = pblk->dev;
	unsigned long secs_per_sec = (dev->sec_per_pl * NSEC_PER_SEC) /
						dev->identity.groups[0].tprt;

	return secs_per_sec * pblk_map_get_active_luns(pblk);
}

int pblk_rl_sysfs_rate_show(struct pblk *pblk)
{
	/* return pblk->write_cur_speed; */
	return pblk->rl.rb_user_max;
}

int pblk_rl_sysfs_rate_store(struct pblk *pblk, int value)
{
	printk(KERN_CRIT "set:%d\n", value);
	/* pblk->write_cur_speed = value; */
	pblk->rl.rb_user_max = value;

	return 0;
}

void pblk_rl_init(struct pblk *pblk)
{
	struct pblk_rate_limiter *rl = &pblk->rl;

	/* To start with, all buffer is available to user I/O writers */
	rl->rb_user_max = pblk_rb_nr_entries(&pblk->rwb);
	atomic_set(&rl->rb_user_cnt, 0);
}

