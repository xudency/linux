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

static inline bool __pblk_rl_rate_user_rb(struct pblk *pblk, int nr_entries)
{
	struct pblk_prov *rl = &pblk->rl;

	spin_lock(&rl->lock);
	if (rl->rb_user_cnt + nr_entries > rl->rb_user_max) {
		spin_unlock(&rl->lock);
		return false;
	}

	rl->rb_user_cnt += nr_entries;
	spin_unlock(&rl->lock);

	return true;
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

void pblk_rl_user_in(struct pblk *pblk, int nr_entries)
{
	/* pblk_rl_rate_user_bw(pblk, nr_entries); */
	pblk_rl_rate_user_rb(pblk, nr_entries);
}

void pblk_rl_user_out(struct pblk *pblk, int nr_entries)
{
	struct pblk_prov *rl = &pblk->rl;

	spin_lock(&rl->lock);
	rl->rb_user_cnt -= nr_entries;
	WARN_ON(rl->rb_user_cnt < 0);
	spin_unlock(&rl->lock);

	if (waitqueue_active(&pblk->wait2))
		wake_up_all(&pblk->wait2);
}

/*
 * We check for (i) the number of free blocks in the current LUN and (ii) the
 * total number of free blocks in the pblk instance. This is to even out the
 * number of free blocks on each LUN when GC kicks in.
 *
 * Only the total number of free blocks is used to configure the rate limiter.
 */
static void pblk_rl_update_rates(struct pblk *pblk, struct pblk_lun *rlun)
{
	struct pblk_prov *rl = &pblk->rl;
	unsigned long rwb_size = pblk_rb_nr_entries(&pblk->rwb);
	int should_start_gc = 0, should_stop_gc = 0;

#if CONFIG_NVM_DEBUG
	lockdep_assert_held(&rl->lock);
#endif

	if (rlun->mgmt->nr_free_blocks > rl->high_lun)
			should_stop_gc = 1;
	else if (rlun->mgmt->nr_free_blocks < rl->low_lun)
			should_start_gc = 1;

	if (rl->free_blocks >= rl->high) {
		rl->rb_user_max = rwb_size;
		should_stop_gc = 1;
	} else if (rl->free_blocks > rl->low && rl->free_blocks < rl->high) {
		/* TODO: redo to power of two calculations */
		int perc = ((rl->free_blocks * 100) / (rl->high - rl->low));

		rl->rb_user_max = (rwb_size / 100) * perc;
		should_start_gc = 1;
	} else {
		rl->rb_user_max = 0;
		should_start_gc = 1;
	}

	if (should_start_gc)
		pblk_gc_should_start(pblk);
	else if (should_stop_gc)
		pblk_gc_should_stop(pblk);
}

void pblk_rl_free_blks_inc(struct pblk *pblk, struct pblk_lun *rlun)
{
#if CONFIG_NVM_DEBUG
	lockdep_assert_held(&rlun->lock);
#endif

	rlun->mgmt->nr_free_blocks++;

	spin_lock(&pblk->rl.lock);
	pblk->rl.free_blocks++;
	pblk_rl_update_rates(pblk, rlun);
	spin_unlock(&pblk->rl.lock);
}

void pblk_rl_free_blks_dec(struct pblk *pblk, struct pblk_lun *rlun)
{
#if CONFIG_NVM_DEBUG
	lockdep_assert_held(&rlun->lock);
#endif

	rlun->mgmt->nr_free_blocks--;

	spin_lock(&pblk->rl.lock);
	pblk->rl.free_blocks--;
	pblk_rl_update_rates(pblk, rlun);
	spin_unlock(&pblk->rl.lock);
}

int pblk_rl_gc_thrs(struct pblk *pblk)
{
	return pblk->rl.high_lun + 1;
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

/* TODO: Update values correctly on power up recovery */
void pblk_rl_init(struct pblk *pblk)
{
	struct pblk_prov *rl = &pblk->rl;

	rl->free_blocks = pblk_nr_free_blks(pblk);

	rl->high = rl->total_blocks / PBLK_USER_HIGH_THRS;
	rl->high_lun = pblk->dev->blks_per_lun / PBLK_USER_HIGH_THRS;
	rl->low = rl->total_blocks / PBLK_USER_LOW_THRS;
	rl->low_lun = pblk->dev->blks_per_lun / PBLK_USER_LOW_THRS;
	if (rl->low_lun < 3)
		rl->low_lun = 3;

	/* To start with, all buffer is available to user I/O writers */
	rl->rb_user_max = pblk_rb_nr_entries(&pblk->rwb);
	rl->rb_user_cnt = 0;

	spin_lock_init(&rl->lock);
}

