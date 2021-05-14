/*
 * Copyright (c) 2015-2018, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/swap.h>
#include <linux/sort.h>
#include <linux/oom.h>
#include <linux/sched.h>
#include <linux/rcupdate.h>
#include <linux/notifier.h>
#include <linux/vmpressure.h>
#include <linux/delay.h>
#include <linux/cred.h>

#ifdef DEBUG
#define kill_dbg(tsk)                                                        \
         pr_info("prlmk: comm:%s(%d) acc_rss:%llu killed",                   \
                  (tsk)->comm, (tsk)->pid, (tsk)->acct_timexpd)

#define reclaim_dbg(tsk, reclaimed)                                          \
         pr_info("prlmk: comm:%s(%d) reclaimed:%d",                          \
                  (tsk)->comm, (tsk)->pid, (reclaimed))

#define lowmem_dbg(filepgs, swappgs, lc)                                     \
         pr_info("prlmk: file_pgs: %lu, swap_pgs: %lu, lowmem: %s",          \
                  (filepgs), (swappgs),                                      \
                  ((lc) > 1 ? "critical" : ((lc) == 1 ? "normal" : "none")))
#else
static inline void kill_dbg(struct task_struct *tsk, ...) { }
static inline void reclaim_dbg(struct task_struct *tsk, ...) { }
static inline void lowmem_dbg(unsigned long filepgs, ...) { }
#endif

#define MAX_KTASK 128
#define MAX_ATASK SWAP_CLUSTER_MAX

#define SWAP_EFF_WIN 2
#define SWAP_OPT_EFF 50
#define PER_SWAP_SIZE (SWAP_CLUSTER_MAX * 32)

struct selected_task {
	struct task_struct *p;
	short adj;
	int anonsize;
	kgid_t gid;
	bool ignore;
};

/*
 * Tasks who have adjs that belong to this list do not
 * have their MM_ANONPAGES reclaimed, and they are also
 * given the highest priority when tasks are being killed.
 */
static const short adj_ignore[] = {
	0,   /* Foreground */
	50,  /* Service */
	200, /* Service */
};

/*
 * Low-memory notification levels
 *
 * LOWMEM_NONE: No low-memory scenario detected.
 *
 * LOWMEM_NORMAL: A scenario in which the swap
 * memory levels are below free_swap_limit.
 *
 * LOWMEM_CRITICAL: A scenario in which the LOWMEM_NORMAL
 * condition is satisfied, as well as when the reclaimable
 * active file pages are below free_file_limit.
 */
enum lowmem_levels {
	LOWMEM_NONE,
	LOWMEM_NORMAL,
	LOWMEM_CRITICAL,
};

static void proc_tasks(struct work_struct *work);
DECLARE_WORK(proc_work, proc_tasks);

static struct selected_task selected[MAX_KTASK] ____cacheline_aligned_in_smp;
static atomic_t skip_reclaim = ATOMIC_INIT(0);
static int tcnt, total_sz, m_eff;

static unsigned long pressure_max = 90;
module_param_named(pressure_max, pressure_max, ulong, 0644);

static short min_adj = 300;
module_param_named(min_adj, min_adj, short, 0644);

/*
 * Number of ACTIVE FILE backed pages in MiB below which
 * tasks with adjs lesser than min_adj will
 * be killed in addition to killing the other tasks.
 *
 * Tune this carefully as lags may occur when this value is
 * too low, and task killing can be more aggressive when too
 * high.
 */
static int free_file_limit = 20000;
module_param_named(free_file_limit, free_file_limit, int, 0644);

/*
 * Number of SWAP pages in MiB below which tasks with
 * adjs greater than min_adj should be killed.
 */
static int free_swap_limit = 20;
module_param_named(free_swap_limit, free_swap_limit, int, 0644);

/*
 * This enables arranging tasks based on their unique
 * group ID, and chooses the heaviest task from them to
 * be considered when tasks are getting killed.
 */
static bool kill_heaviest_gid = true;
module_param_named(kill_heaviest_gid, kill_heaviest_gid, bool, 0644);

static inline int atask_limit(void)
{
	return (tcnt > MAX_ATASK ? MAX_ATASK : tcnt);
}

static inline bool should_skip_reclaim(void)
{
	return (atomic_dec_if_positive(&skip_reclaim) >= 0);
}

static inline bool in_adj_ignore(short adj)
{
	int i;
	short adj_ignore_size = ARRAY_SIZE(adj_ignore);

	for (i = 0; i < adj_ignore_size; i++)
		if (adj == adj_ignore[i])
			return true;

	return false;
}

static inline bool test_task(struct task_struct *tsk)
{
	struct signal_struct *s = tsk->signal;

	if (s->flags & (SIGNAL_GROUP_EXIT | SIGNAL_GROUP_COREDUMP) ||
	     (thread_group_empty(tsk) && tsk->flags & PF_EXITING)  ||
		(tsk->flags & PF_KTHREAD))
		return true;

	return false;
}

static bool test_task_tflag(struct task_struct *p, int flag)
{
	struct task_struct *t = p;

	for_each_thread(p, t) {
		task_lock(t);
		if (test_tsk_thread_flag(t, flag)) {
			task_unlock(t);
			rcu_read_unlock();
			return true;
		}
		task_unlock(t);
	}

	return false;
}

static int asz_cmp(const void *a, const void *b)
{
	struct selected_task *x = ((struct selected_task *)a);
	struct selected_task *y = ((struct selected_task *)b);
	int ret;

	ret = x->anonsize < y->anonsize ? 1 : -1;

	return ret;
}

static int txd_cmp(const void *a, const void *b)
{
	struct selected_task *x = ((struct selected_task *)a);
	struct selected_task *y = ((struct selected_task *)b);
	int ret;

	ret = x->p->acct_timexpd < y->p->acct_timexpd ? 1 : -1;

	return ret;
}

static int gid_cmp(const void *a, const void *b)
{
	struct selected_task *x = ((struct selected_task *)a);
	struct selected_task *y = ((struct selected_task *)b);
	int ret;

	ret = gid_lt(x->gid, y->gid) ? 1 : -1;

	return ret;
}

static void cmp_swap(void *a, void *b, int size)
{
	struct selected_task *x = ((struct selected_task *)a);
	struct selected_task *y = ((struct selected_task *)b);

	swap(*x, *y);
}

static inline int is_low_mem(void)
{
	const int lru_base = NR_LRU_BASE - LRU_BASE;
	int ret;

	unsigned long cur_file_mem =
			global_zone_page_state(lru_base + LRU_ACTIVE_FILE);

	unsigned long cur_swap_mem = (get_nr_swap_pages() << (PAGE_SHIFT - 10));
	unsigned long swap_mem = free_swap_limit * 1024;

	bool swap_limit = cur_swap_mem < swap_mem;
	bool file_limit = cur_file_mem < free_file_limit;

	bool lowmem_normal = (swap_mem ? swap_limit : file_limit);
	bool lowmem_critical = (swap_mem ? file_limit : true) &&
				lowmem_normal && !cur_swap_mem;

	if (lowmem_critical)
		ret = LOWMEM_CRITICAL;
	else if (lowmem_normal)
		ret = LOWMEM_NORMAL;
	else
		ret = LOWMEM_NONE;

	lowmem_dbg(cur_file_mem, cur_swap_mem, ret);

	return ret;
}

static void filter_tasks_by_gid(void)
{
	int i, j, lpos, fpos = -1;

	if (!kill_heaviest_gid || is_low_mem() == LOWMEM_CRITICAL)
		return;

	/* Group tasks based on their GIDs, the order doesn't matter */
	sort(selected, tcnt, sizeof(*selected), gid_cmp, cmp_swap);

	/* Split the tasks with the same GIDs into separate sets */
	for (i = 0; i < (tcnt - 1); i++) {
		kgid_t curr_gid = selected[i].gid;
		kgid_t next_gid = selected[i + 1].gid;
		kgid_t fpos_gid;

		if (!gid_eq(curr_gid, next_gid)) {
			if (fpos >= 0 && i != fpos) {
				fpos_gid = selected[fpos].gid;

				if (gid_eq(curr_gid, fpos_gid))
					goto get_heaviest;
			}

			continue;
		}

		if (fpos < 0)
			fpos = i;

		continue;

get_heaviest:
		lpos = i;

		/*
		 * Find the heaviest task within the given
		 * set of tasks.
		 */
		sort(&selected[fpos], lpos - (fpos - 1),
				sizeof(*selected), txd_cmp, cmp_swap);

		/*
		 * Mark the rest of the tasks in the set as ignored,
		 * so that they won't be considered when tasks are
		 * getting killed.
		 */
		for (j = (fpos + 1); j <= lpos; j++)
			selected[j].ignore = true;

		fpos = -1;
	}
}

static void reclaim_tasks(void)
{
	struct reclaim_param rp;
	int alimit = atask_limit();
	int i, eff, nr_to_reclaim, reclaimed = 0, scanned = 0;

	/*
	 * Sort tasks according to their anonsize (number of MM_ANON pages),
	 * in descending order.
	 */
	sort(selected, (alimit - 1), sizeof(*selected), asz_cmp, cmp_swap);

	/* Reclaim tasks with the highest anonsize first */
	for (i = 0; i < alimit; i++) {
		struct task_struct *t = selected[i].p;
		int adj = selected[i].adj;
		int anonsize = selected[i].anonsize;

		if (!anonsize || in_adj_ignore(adj)) {
			put_task_struct(t);
			continue;
		}

		nr_to_reclaim = (anonsize * PER_SWAP_SIZE) / total_sz;

		rp = reclaim_task_anon(t, nr_to_reclaim);
		reclaim_dbg(t, rp.nr_reclaimed);

		scanned += rp.nr_scanned;
		reclaimed += rp.nr_reclaimed;

		put_task_struct(t);
	}

	if (scanned) {
		eff = (reclaimed * 100) / scanned;

		if (eff < SWAP_OPT_EFF && ++m_eff == SWAP_EFF_WIN) {
			atomic_set(&skip_reclaim, SWAP_EFF_WIN);
			m_eff = 0;
		}
	}
}

static void kill_task(struct selected_task select)
{
	struct task_struct *tsk = select.p;

	kill_dbg(tsk);

	task_lock(tsk);
	do_send_sig_info(SIGKILL, SEND_SIG_FORCED, tsk, true);
	task_unlock(tsk);

	msleep_interruptible(20);
}

static void sort_and_kill_tasks(void)
{
	struct selected_task select;
	int sv[MAX_KTASK], fg[MAX_KTASK];
	int killcnt = 0, svcnt = 0, fgcnt = 0;

	/*
	 * Find the heaviest task within a given set of
	 * GIDs and ignore the rest of the tasks within
	 * that set, so that only the heaviest task is
	 * considered when killing tasks.
	 */
	filter_tasks_by_gid();

	/*
	 * Sort tasks based on their accumulated rss usage,
	 * in descending order.
	 */
	sort(selected, tcnt, sizeof(*selected), txd_cmp, cmp_swap);

	/* Kill tasks with the lowest accumulated rss usage first */
	while (tcnt--) {
		int lm_status = is_low_mem();
		bool lm_crit = lm_status == LOWMEM_CRITICAL;

		select = selected[tcnt];

		if (lm_status == LOWMEM_NONE)
			return;

		if (select.ignore)
			continue;

		if (select.adj < min_adj && !lm_crit)
			continue;

		/*
		 * Protect the tasks whose adjs are in adj_ignore
		 * from being killed under high memory pressure.
		 */
		if (in_adj_ignore(select.adj)) {
			if (!select.adj)
				fg[fgcnt++] = tcnt;
			else
				sv[svcnt++] = tcnt;

			continue;
		}

		/* Kill the selected task */
		kill_task(select);
		killcnt++;
	}

	/*
	 * If no tasks were killed under high memory pressure,
	 * kill the protected tasks as well.
	 *
	 * Kill the services first, and then the foreground tasks.
	 */
	if (!killcnt && is_low_mem() == LOWMEM_CRITICAL) {
		while (svcnt || fgcnt) {
			if (is_low_mem() < LOWMEM_CRITICAL)
				return;

			if (!svcnt) {
				select = selected[fg[--fgcnt]];
				kill_task(select);
				continue;
			}

			select = selected[sv[--svcnt]];
			kill_task(select);
		}
	}
}

static void proc_tasks(struct work_struct *work)
{
	struct task_struct *tsk;

	int i, anonsize;

	tcnt = total_sz = 0;

	rcu_read_lock();
	for_each_process(tsk) {
		struct task_struct *p;
		short adj;
		struct group_info *group;
		kgid_t gid = KGIDT_INIT(0);

		if (test_task(tsk))
			continue;

		if (test_task_tflag(tsk, TIF_MEMDIE))
			continue;

		p = find_lock_task_mm(tsk);
		if (!p)
			continue;

		adj = p->signal->oom_score_adj;
		if (adj < 0) {
			task_unlock(p);
			continue;
		}

		anonsize = get_mm_counter(p->mm, MM_ANONPAGES);
		task_unlock(p);

		if (kill_heaviest_gid) {
			group = __task_cred(p)->group_info;
			gid = group->gid[(group->ngroups - 1)];

			if (!__kgid_val(gid))
				continue;
		}

		selected[tcnt].p = p;
		selected[tcnt].adj = adj;
		selected[tcnt].gid = gid;
		selected[tcnt].ignore = false;
		selected[tcnt].anonsize = anonsize;
		total_sz += anonsize;

		if (++tcnt == MAX_KTASK)
			break;
	}

	if (!tcnt || !total_sz) {
		rcu_read_unlock();
		return;
	}

	if (is_low_mem() == LOWMEM_NONE) {
		if (!should_skip_reclaim()) {
			for (i = 0; i < atask_limit(); i++)
				get_task_struct(selected[i].p);
			rcu_read_unlock();
			reclaim_tasks();
			return;
		}
		rcu_read_unlock();
		return;
	}
	rcu_read_unlock();

	sort_and_kill_tasks();
}

static int vmpressure_notifier(struct notifier_block *nb,
			unsigned long action, void *data)
{
	unsigned long pressure = action;

	if (!current_is_kswapd())
		return 0;

	if (pressure >= pressure_max)
		if (!work_pending(&proc_work))
			queue_work(system_highpri_wq, &proc_work);
	return 0;
}

static struct notifier_block vmpr_nb = {
	.notifier_call = vmpressure_notifier,
};

/* Needed to prevent Android from thinking there's no LMK and thus rebooting. */
static int prlmk_init(const char *val, const struct kernel_param *kp)
{
	static atomic_t init_done = ATOMIC_INIT(0);

	if (!atomic_cmpxchg(&init_done, 0, 1))
		BUG_ON(vmpressure_notifier_register(&vmpr_nb));

	return 0;
}

static const struct kernel_param_ops prlmk_ops = {
	.set = prlmk_init
};

#undef MODULE_PARAM_PREFIX
#define MODULE_PARAM_PREFIX "lowmemorykiller."
module_param_cb(minfree, &prlmk_ops, NULL, 0200);
