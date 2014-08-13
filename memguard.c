/**
 * Memory bandwidth controller for multi-core systems
 *
 * Copyright (C) 2013  Heechul Yun <heechul@illinois.edu>
 *
 * This file is distributed under the University of Illinois Open Source
 * License. See LICENSE.TXT for details.
 *
 */

/**************************************************************************
 * Conditional Compilation Options
 **************************************************************************/
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#define USE_DEBUG  1 

/**************************************************************************
 * Included Files
 **************************************************************************/
#include <linux/version.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <linux/smp.h> /* IPI calls */
#include <linux/irq_work.h>
#include <linux/hardirq.h>
#include <linux/perf_event.h>
#include <linux/delay.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <asm/atomic.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/uaccess.h>
#include <linux/notifier.h>
#include <linux/kthread.h>
#include <linux/printk.h>
#include <linux/interrupt.h>
#if LINUX_VERSION_CODE > KERNEL_VERSION(3, 8, 0)
#  include <linux/sched/rt.h>
#endif
#include <linux/cpu.h>
#include <asm/idle.h>

/**************************************************************************
 * Public Definitions
 **************************************************************************/
#define MAX_NCPUS 32
#define CACHE_LINE_SIZE 64

#if USE_DEBUG
#  define DEBUG(x) x 
#  define DEBUG_RECLAIM(x) x
#  define DEBUG_USER(x) x
#else
#  define DEBUG(x) 
#  define DEBUG_RECLAIM(x)
#  define DEBUG_USER(x)
#endif

#define BUF_SIZE 256
#define PREDICTOR 1  /* 0 - used, 1 - ewma(a=1/2), 2 - ewma(a=1/4) */

/**************************************************************************
 * Public Types
 **************************************************************************/
struct memstat{
	u64 used_budget;         /* used budget*/
	u64 assigned_budget;
	u64 throttled_time_ns;   
	int throttled;           /* throttled period count */
	u64 throttled_error;     /* throttled & error */
	int throttled_error_dist[10]; /* pct distribution */
	int exclusive;           /* exclusive period count */
	u64 exclusive_ns;        /* exclusive mode real-time duration */
	u64 exclusive_bw;        /* exclusive mode used bandwidth */
};

/* percpu info */
struct core_info {
	/* user configurations */
	int budget;              /* assigned budget */

	int limit;               /* limit mode (exclusive to weight)*/
	int weight;              /* weight mode (exclusive to limit)*/
	int wsum;                /* local copy of global->wsum */

	/* for control logic */
	int cur_budget;          /* currently available budget */

	volatile struct task_struct * throttled_task;
	ktime_t throttled_time;  /* absolute time when throttled */

	u64 old_val;             /* hold previous counter value */
	int prev_throttle_error; /* check whether there was throttle error in 
				    the previous period */

	u64 exclusive_vtime;     /* exclusive mode vtime for scheduling */

	int exclusive_mode;      /* 1 - if in exclusive mode */
	ktime_t exclusive_time;  /* time when exclusive mode begins */

	struct irq_work	pending; /* delayed work for NMIs */
	struct perf_event *event;/* performance counter i/f */

	struct task_struct *throttle_thread;  /* forced throttle idle thread */
	wait_queue_head_t throttle_evt; /* throttle wait queue */

	/* statistics */
	struct memstat overall;  /* stat for overall periods. reset by user */
	int used[3];             /* EWMA memory load */
	long period_cnt;         /* active periods count */
};

/* global info */
struct memguard_info {
	int master;
	ktime_t period_in_ktime;
	int start_tick;
	int budget;              /* reclaimed budget */
	long period_cnt;
	spinlock_t lock;
	int max_budget;          /* \sum(cinfo->budget) */

	cpumask_var_t throttle_mask;
	cpumask_var_t active_mask;
	atomic_t wsum;
	long bw_locked_core;
	struct hrtimer hr_timer;
};


/**************************************************************************
 * Global Variables
 **************************************************************************/
static struct memguard_info memguard_info;
static struct core_info __percpu *core_info;

static char *g_hw_type = "";
static int g_period_us = 1000;
static int g_use_reclaim = 0; /* minimum remaining time to reclaim */
static int g_use_exclusive = 0;
static int g_use_task_priority = 0;
static int g_budget_pct[MAX_NCPUS];
static int g_budget_cnt = 4;
static int g_budget_min_value = 1000;
static int g_budget_max_bw = 1200; /* MB/s. best=6000 MB/s, worst=1200 MB/s */ 

static struct dentry *memguard_dir;

static int g_test = 0;

/* copied from kernel/sched/sched.h */
static const int prio_to_weight[40] = {
 /* -20 */     88761,     71755,     56483,     46273,     36291,
 /* -15 */     29154,     23254,     18705,     14949,     11916,
 /* -10 */      9548,      7620,      6100,      4904,      3906,
 /*  -5 */      3121,      2501,      1991,      1586,      1277,
 /*   0 */      1024,       820,       655,       526,       423,
 /*   5 */       335,       272,       215,       172,       137,
 /*  10 */       110,        87,        70,        56,        45,
 /*  15 */        36,        29,        23,        18,        15,
};

/**************************************************************************
 * External Function Prototypes
 **************************************************************************/
extern unsigned long nr_running_cpu(int cpu);
extern int idle_cpu(int cpu);

/**************************************************************************
 * Local Function Prototypes
 **************************************************************************/
static int self_test(void);
static void __reset_stats(void *info);
static void period_timer_callback_slave(void *info);
enum hrtimer_restart period_timer_callback_master(struct hrtimer *timer);
static void memguard_process_overflow(struct irq_work *entry);
static int throttle_thread(void *arg);
static void memguard_on_each_cpu_mask(const struct cpumask *mask,
				      smp_call_func_t func,
				      void *info, bool wait);

/**************************************************************************
 * Module parameters
 **************************************************************************/

module_param(g_test, int, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
MODULE_PARM_DESC(g_test, "number of test iterations");

module_param(g_hw_type, charp,  S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
MODULE_PARM_DESC(g_hw_type, "hardware type");

module_param(g_use_reclaim, int, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
MODULE_PARM_DESC(g_use_reclaim, "enable/disable reclaim");

module_param(g_period_us, int, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
MODULE_PARM_DESC(g_period_us, "throttling period in usec");

module_param_array(g_budget_pct, int, &g_budget_cnt, 0000);
MODULE_PARM_DESC(g_budget_pct, "array of budget per cpu");

module_param(g_budget_max_bw, int, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
MODULE_PARM_DESC(g_budget_max_bw, "maximum memory bandwidth (MB/s)");

/**************************************************************************
 * Module main code
 **************************************************************************/

/* similar to on_each_cpu_mask(), but this must be called with IRQ disabled */
static void memguard_on_each_cpu_mask(const struct cpumask *mask, 
				      smp_call_func_t func,
				      void *info, bool wait)
{
	int cpu = smp_processor_id();
	smp_call_function_many(mask, func, info, wait);
	if (cpumask_test_cpu(cpu, mask)) {
		func(info);
	}
}

static int __cpuinit memguard_cpu_callback(struct notifier_block *nfb,
					 unsigned long action, void *hcpu)
{
	unsigned int cpu = (unsigned long)hcpu;
	
	switch (action) {
	case CPU_ONLINE:
	case CPU_ONLINE_FROZEN:
		trace_printk("CPU%d is online\n", cpu);
		break;
	case CPU_DEAD:
	case CPU_DEAD_FROZEN:
		trace_printk("CPU%d is offline\n", cpu);
		break;
	}
	return NOTIFY_OK;
}

static struct notifier_block __cpuinitdata memguard_cpu_notifier =
{
	.notifier_call = memguard_cpu_callback,
};


/** convert MB/s to #of events (i.e., LLC miss counts) per 1ms */
static inline u64 convert_mb_to_events(int mb)
{
	return div64_u64((u64)mb*1024*1024,
			 CACHE_LINE_SIZE* (1000000/g_period_us));
}
static inline int convert_events_to_mb(u64 events)
{
	int divisor = g_period_us*1024*1024;
	int mb = div64_u64(events*CACHE_LINE_SIZE*1000000 + (divisor-1), divisor);
	return mb;
}

static inline void print_current_context(void)
{
	trace_printk("in_interrupt(%ld)(hard(%ld),softirq(%d)"
		     ",in_nmi(%d)),irqs_disabled(%d)\n",
		     in_interrupt(), in_irq(), (int)in_softirq(),
		     (int)in_nmi(), (int)irqs_disabled());
}

/** read current counter value. */
static inline u64 perf_event_count(struct perf_event *event)
{
	return local64_read(&event->count) + 
		atomic64_read(&event->child_count);
}

/** return used event in the current period */
static inline u64 memguard_event_used(struct core_info *cinfo)
{
	return perf_event_count(cinfo->event) - cinfo->old_val;
}

static void print_core_info(int cpu, struct core_info *cinfo)
{
	pr_info("CPU%d: budget: %d, cur_budget: %d, period: %ld\n", 
	       cpu, cinfo->budget, cinfo->cur_budget, cinfo->period_cnt);
}

/**
 * update per-core usage statistics
 */
void update_statistics(struct core_info *cinfo)
{
	/* counter must be stopped by now. */
	s64 new;
	int used;
	u64 exclusive_vtime = 0;

	new = perf_event_count(cinfo->event);
	used = (int)(new - cinfo->old_val); 

	cinfo->old_val = new;
	cinfo->overall.used_budget += used;
	cinfo->overall.assigned_budget += cinfo->budget;

	/* EWMA filtered per-core usage statistics */
	cinfo->used[0] = used;
	cinfo->used[1] = (cinfo->used[1] * (2-1) + used) >> 1; 
	/* used[1]_k = 1/2 used[1]_k-1 + 1/2 used */
	cinfo->used[2] = (cinfo->used[2] * (4-1) + used) >> 2; 
	/* used[2]_k = 3/4 used[2]_k-1 + 1/4 used */

	/* core is currently throttled. */
	if (cinfo->throttled_task) {
		cinfo->overall.throttled_time_ns +=
			(ktime_get().tv64 - cinfo->throttled_time.tv64);
		cinfo->overall.throttled++;
	}

	/* throttling error condition:
	   I was too aggressive in giving up "unsed" budget */
	if (cinfo->prev_throttle_error && used < cinfo->budget) {
		int diff = cinfo->budget - used;
		int idx;
		cinfo->overall.throttled_error ++; // += diff;
		BUG_ON(cinfo->budget == 0);
		idx = (int)(diff * 10 / cinfo->budget);
		cinfo->overall.throttled_error_dist[idx]++;
		trace_printk("ERR: throttled_error: %d < %d\n", used, cinfo->budget);
		/* compensation for error to catch-up*/
		cinfo->used[PREDICTOR] = cinfo->budget + diff;
	}
	cinfo->prev_throttle_error = 0;

	/* I was the lucky guy who used the DRAM exclusively */
	if (cinfo->exclusive_mode) {
		struct memguard_info *global = &memguard_info;
		int wsum = 0;
		int i;
		u64 exclusive_ns, exclusive_bw;

		/* used time */
		exclusive_ns = (ktime_get().tv64 - cinfo->exclusive_time.tv64);
		
		/* used bw */
		exclusive_bw = (cinfo->used[0] - cinfo->budget);

		if (g_use_exclusive == 3)
			exclusive_vtime = exclusive_ns;
		else if (g_use_exclusive == 4)
			exclusive_vtime = exclusive_bw;
		else
			exclusive_vtime = 0;

		if (cinfo->weight > 0) {
			/* weighted vtime (used by scheduler on throttle) */
			for_each_cpu(i, global->active_mask)
				wsum += per_cpu_ptr(core_info, i)->weight;
			cinfo->exclusive_vtime += 
				div64_u64((u64)exclusive_vtime * wsum, 
					  cinfo->weight);
		} else
			cinfo->exclusive_vtime += exclusive_vtime;

		cinfo->overall.exclusive_ns += exclusive_ns;
		cinfo->overall.exclusive_bw += exclusive_bw;
		cinfo->exclusive_mode = 0;
		cinfo->overall.exclusive++;
	}
	DEBUG(trace_printk("%lld %d %p CPU%d org: %d cur: %d excl(%d): %lld\n",
			   new, used, cinfo->throttled_task,
			   smp_processor_id(), 
			   cinfo->budget,
			   cinfo->cur_budget,
			   g_use_exclusive, exclusive_vtime));
}


/**
 * budget is used up. PMU generate an interrupt
 * this run in hardirq, nmi context with irq disabled
 */
static void event_overflow_callback(struct perf_event *event,
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 2, 0)
				    int nmi,
#endif
				    struct perf_sample_data *data,
				    struct pt_regs *regs)
{
	struct core_info *cinfo = this_cpu_ptr(core_info);
	BUG_ON(!cinfo);
	irq_work_queue(&cinfo->pending);
}

/* must be in hardirq context */
static int donate_budget(long cur_period, int budget)
{
	struct memguard_info *global = &memguard_info;
	spin_lock(&global->lock);
	if (global->period_cnt == cur_period) {
		global->budget += budget;
	}
	spin_unlock(&global->lock);
	return global->budget;
}

/* must be in hardirq context */
static int reclaim_budget(long cur_period, int budget)
{
	struct memguard_info *global = &memguard_info;
	int reclaimed = 0;
	spin_lock(&global->lock);
	if (global->period_cnt == cur_period) {
		reclaimed = min(budget, global->budget);
		global->budget -= reclaimed;
	}
	spin_unlock(&global->lock);
	return reclaimed;
}

/**
 * reclaim local budget from global budget pool
 */
static int request_budget(struct core_info *cinfo)
{
	int amount = 0;
	int budget_used = memguard_event_used(cinfo);

	BUG_ON(!cinfo);

	if (budget_used < cinfo->budget && current->policy == SCHED_NORMAL) {
		/* didn't used up my original budget */
		amount = cinfo->budget - budget_used;
	} else {
		/* I'm requesting more than I originall assigned */
		amount = g_budget_min_value;
	}

	if (amount > 0) {
		/* successfully reclaim my budget */
		amount = reclaim_budget(cinfo->period_cnt, amount);
	}
	return amount;
}

/**
 * called by process_overflow
 */
static void __unthrottle_core(void *info)
{
	struct core_info *cinfo = this_cpu_ptr(core_info);
	if (cinfo->throttled_task) {
		cinfo->exclusive_mode = 1;
		cinfo->exclusive_time = ktime_get();

		cinfo->throttled_task = NULL;
		smp_wmb();
		DEBUG_RECLAIM(trace_printk("exclusive mode begin\n"));
	}
}

static void __newperiod(void *info)
{
	long period = (long)info;
	ktime_t start = ktime_get();
	struct memguard_info *global = &memguard_info;
	
	spin_lock(&global->lock);
	if (period == global->period_cnt) {
		ktime_t new_expire = ktime_add(start, global->period_in_ktime);
		long new_period = ++global->period_cnt;
		global->budget = 0;
		spin_unlock(&global->lock);

		/* arrived before timer interrupt is called */
		hrtimer_start_range_ns(&global->hr_timer, new_expire,
				       0, HRTIMER_MODE_ABS_PINNED);
		DEBUG(trace_printk("begin new period\n"));

		on_each_cpu(period_timer_callback_slave, (void *)new_period, 0);
	} else
		spin_unlock(&global->lock);
}

/**
 * memory overflow handler.
 * must not be executed in NMI context. but in hard irq context
 */
static void memguard_process_overflow(struct irq_work *entry)
{
	struct core_info *cinfo = this_cpu_ptr(core_info);
	struct memguard_info *global = &memguard_info;

	int amount = 0;
	ktime_t start = ktime_get();
	s64 budget_used;
	BUG_ON(in_nmi() || !in_irq());
	WARN_ON_ONCE(cinfo->budget > global->max_budget);

	spin_lock(&global->lock);
	if (!cpumask_test_cpu(smp_processor_id(), global->active_mask)) {
		spin_unlock(&global->lock);
		trace_printk("ERR: not active\n");
		return;
	} else if (global->period_cnt != cinfo->period_cnt) {
		trace_printk("ERR: global(%ld) != local(%ld) period mismatch\n",
			     global->period_cnt, cinfo->period_cnt);
		spin_unlock(&global->lock);
		return;
	}
	spin_unlock(&global->lock);

	budget_used = memguard_event_used(cinfo);

	/* erroneous overflow, that could have happend before period timer
	   stop the pmu */
	if (budget_used < cinfo->cur_budget) {
		trace_printk("ERR: overflow in timer. used %lld < cur_budget %d. ignore\n",
			     budget_used, cinfo->cur_budget);
		return;
	}

	/* try to reclaim budget from the global pool */
	amount = request_budget(cinfo);
	if (amount > 0) {
		cinfo->cur_budget += amount;
		local64_set(&cinfo->event->hw.period_left, amount);
		DEBUG_RECLAIM(trace_printk("successfully reclaimed %d\n", amount));
		return;
	}

	/* no more overflow interrupt */
	local64_set(&cinfo->event->hw.period_left, 0xfffffff);

	/* check if we donated too much */
	if (budget_used < cinfo->budget) {
		trace_printk("ERR: throttling error\n");
		cinfo->prev_throttle_error = 1;
	}

	/* we are going to be throttled */
	spin_lock(&global->lock);
	cpumask_set_cpu(smp_processor_id(), global->throttle_mask);
	if (cpumask_equal(global->throttle_mask, global->active_mask)) {
		/* all other cores are alreay throttled */
		spin_unlock(&global->lock);
		if (g_use_exclusive == 1) {
			/* algorithm 1: last one get all remaining time */
			cinfo->exclusive_mode = 1;
			cinfo->exclusive_time = ktime_get();
			DEBUG_RECLAIM(trace_printk("exclusive mode begin\n"));
			return;
		} else if (g_use_exclusive == 2) {
			/* algorithm 2: wakeup all (i.e., non regulation) */
			smp_call_function(__unthrottle_core, NULL, 0);
			cinfo->exclusive_mode = 1;
			cinfo->exclusive_time = ktime_get();
			DEBUG_RECLAIM(trace_printk("exclusive mode begin\n"));
			return;
		} else if (g_use_exclusive == 3 || g_use_exclusive == 4) {
			/* algorithm 3: CFS based on exclusive_vtime_ns */
			int target_cpu = smp_processor_id(); /* wake up cpu */
			u64 min_vtime = 0;
			int i;
			for_each_cpu(i, global->active_mask) {
				u64 cur_vtime = per_cpu_ptr(core_info, i)
					->exclusive_vtime;
				if (min_vtime == 0 || cur_vtime < min_vtime) {
					min_vtime = cur_vtime;
					target_cpu = i;
				}
			}
			if (target_cpu == smp_processor_id()) {
				cinfo->exclusive_mode = 1;
				cinfo->exclusive_time = ktime_get();
				DEBUG_RECLAIM(trace_printk("exclusive%d mode begin"
							   "vtime: %lld\n", 
							   cinfo->exclusive_mode,
							   cinfo->exclusive_vtime));
				return;
			}
			smp_call_function_single(
				target_cpu, __unthrottle_core, NULL, 0);
			/* i'll be throttled */
		} else if (g_use_exclusive == 5) {
			smp_call_function_single(global->master, __newperiod, 
					  (void *)cinfo->period_cnt, 0);
			return;
		} else if (g_use_exclusive > 5) {
			trace_printk("ERR: Unsupported exclusive mode %d\n", 
				     g_use_exclusive);
			return;
		} else if (g_use_exclusive != 0 &&
			   cpumask_weight(global->active_mask) == 1) {
			trace_printk("ERR: don't throttle one active core\n");
			return;
		}
	} else
		spin_unlock(&global->lock);
	

	if (cinfo->prev_throttle_error)
		return;
	/*
	 * fail to reclaim. now throttle this core
	 */
	DEBUG_RECLAIM(trace_printk("fail to reclaim after %lld nsec.\n",
				   ktime_get().tv64 - start.tv64));

	/* wake-up throttle task */
	cinfo->throttled_task = current;
	cinfo->throttled_time = start;

	WARN_ON_ONCE(!strncmp(current->comm, "swapper", 7));
	smp_mb();
	wake_up_interruptible(&cinfo->throttle_evt);
}

/**
 * per-core period processing
 *
 * called by scheduler tick to replenish budget and unthrottle if needed
 * run in interrupt context (irq disabled)
 */

/*
 * period_timer algorithm:
 *	excess = 0;
 *	if predict < budget:
 *	   excess = budget - predict;
 *	   global += excess
 *	set interrupt at (budget - excess)
 */
static void period_timer_callback_slave(void *info)
{
	struct core_info *cinfo = this_cpu_ptr(core_info);
	struct memguard_info *global = &memguard_info;
	struct task_struct *target;
	long new_period = (long)info;
	int cpu = smp_processor_id();

	/* must be irq disabled. hard irq */
	BUG_ON(!irqs_disabled());
	WARN_ON_ONCE(!in_irq());

	if (new_period <= cinfo->period_cnt) {
		trace_printk("ERR: new_period(%ld) <= cinfo->period_cnt(%ld)\n",
			     new_period, cinfo->period_cnt);
		return;
	}

	/* assign local period */
	cinfo->period_cnt = new_period;

	/* stop counter */
	cinfo->event->pmu->stop(cinfo->event, PERF_EF_UPDATE);

	if (cinfo->exclusive_vtime == 0 && 
	    (g_use_exclusive == 3 || g_use_exclusive == 4))
	{
		/* set the minimum vtime */
		u64 min_vtime = 0;
		int i;
		for_each_cpu(i, global->active_mask) {
			u64 cur_vtime =
				per_cpu_ptr(core_info, i)->exclusive_vtime;
			if (min_vtime == 0 || cur_vtime < min_vtime)
				min_vtime = cur_vtime;
		}
		cinfo->exclusive_vtime = min_vtime;
	}
	

	/* I'm actively participating */
	spin_lock(&global->lock);
	cpumask_clear_cpu(cpu, global->throttle_mask);
	cpumask_set_cpu(cpu, global->active_mask);
	spin_unlock(&global->lock);

	DEBUG(trace_printk("%p|New period %ld. global->budget=%d\n",
			   cinfo->throttled_task,
			   cinfo->period_cnt, global->budget));
	
	/* update statistics. */
	update_statistics(cinfo);

	/* task priority to weight conversion */
	if (g_use_task_priority) {
		int prio = current->static_prio - MAX_RT_PRIO;
		if (prio < 0) 
			prio = 0;
		cinfo->weight = prio_to_weight[prio];
		DEBUG(trace_printk("Task WGT: %d prio:%d\n", cinfo->weight, prio));
	}

	/* new budget assignment from user */
	spin_lock(&global->lock);

	if (cinfo->weight > 0) {
		/* weight mode */
		int wsum = 0; int i;
		smp_mb();
		for_each_cpu(i, global->active_mask)
			wsum += per_cpu_ptr(core_info, i)->weight;
		cinfo->budget = 
			div64_u64((u64)global->max_budget*cinfo->weight, wsum);
		DEBUG(trace_printk("WGT: budget:%d/%d weight:%d/%d\n",
				   cinfo->budget, global->max_budget,
				   cinfo->weight, wsum));
	} else if (cinfo->limit > 0) {
		/* limit mode */
		cinfo->budget = cinfo->limit;
	} else {
		pr_err("both limit and weight = 0");
	}

	if (cinfo->budget > global->max_budget)
		trace_printk("ERR: c->budget(%d) > g->max_budget(%d)\n",
		     cinfo->budget, global->max_budget);

	spin_unlock(&global->lock);

	/* budget can't be zero? */
	cinfo->budget = max(cinfo->budget, 1);

	if (cinfo->event->hw.sample_period != cinfo->budget) {
		/* new budget is assigned */
		trace_printk("MSG: new budget %d is assigned\n", 
			     cinfo->budget);
		cinfo->event->hw.sample_period = cinfo->budget;
	}

	/* unthrottle tasks (if any) */
	if (cinfo->throttled_task)
		target = (struct task_struct *)cinfo->throttled_task;
	else
		target = current;
	cinfo->throttled_task = NULL;

	/* per-task donation policy */
	if (!g_use_reclaim || rt_task(target)) {
		cinfo->cur_budget = cinfo->budget;
		DEBUG(trace_printk("HRT or !g_use_reclaim: don't donate\n"));
	} else if (target->policy == SCHED_BATCH || 
		   target->policy == SCHED_IDLE) {
		/* Non rt task: donate all */
		donate_budget(cinfo->period_cnt, cinfo->budget);
		cinfo->cur_budget = 0;
		DEBUG(trace_printk("NonRT: donate all %d\n", cinfo->budget));
	} else if (target->policy == SCHED_NORMAL) {
		BUG_ON(rt_task(target));
		if (cinfo->used[PREDICTOR] < cinfo->budget) {
			/* donate 'expected surplus' ahead of time. */
			int surplus = max(cinfo->budget - cinfo->used[PREDICTOR], 0);
			WARN_ON_ONCE(surplus > global->max_budget);
			donate_budget(cinfo->period_cnt, surplus);
			cinfo->cur_budget = cinfo->budget - surplus;
			DEBUG(trace_printk("SRT: surplus: %d, budget: %d\n", surplus, 
					   cinfo->budget));
		} else {
			cinfo->cur_budget = cinfo->budget;
			DEBUG(trace_printk("SRT: don't donate\n"));
		}
	}

	/* setup an interrupt */
	cinfo->cur_budget = max(1, cinfo->cur_budget);
	local64_set(&cinfo->event->hw.period_left, cinfo->cur_budget);

	/* enable performance counter */
	cinfo->event->pmu->start(cinfo->event, PERF_EF_RELOAD);
}

static void __init_per_core(void *info)
{
	struct core_info *cinfo = this_cpu_ptr(core_info);
	memset(cinfo, 0, sizeof(struct core_info));

	smp_rmb();

	/* initialize per_event structure */
	cinfo->event = (struct perf_event *)info;

	/* initialize budget */
	cinfo->budget = cinfo->limit = cinfo->event->hw.sample_period;

	/* create idle threads */
	cinfo->throttled_task = NULL;

	init_waitqueue_head(&cinfo->throttle_evt);

	/* initialize statistics */
	__reset_stats(cinfo);

	print_core_info(smp_processor_id(), cinfo);

	smp_wmb();

	/* initialize nmi irq_work_queue */
	init_irq_work(&cinfo->pending, memguard_process_overflow);
}

/**
 *   called while cpu_base->lock is held by hrtimer_interrupt()
 */
enum hrtimer_restart period_timer_callback_master(struct hrtimer *timer)
{
	struct memguard_info *global = &memguard_info;

	ktime_t now;
	int orun;
	long new_period;
	cpumask_var_t active_mask;

	now = timer->base->get_time();

        DEBUG(trace_printk("master begin\n"));
	BUG_ON(smp_processor_id() != global->master);

	orun = hrtimer_forward(timer, now, global->period_in_ktime);
	if (orun == 0)
		return HRTIMER_RESTART;

	spin_lock(&global->lock);
	global->period_cnt += orun;
	global->budget = 0;
	new_period = global->period_cnt;
	cpumask_copy(active_mask, global->active_mask);
	spin_unlock(&global->lock);

	DEBUG(trace_printk("spinlock end\n"));
	if (orun > 1)
		trace_printk("ERR: timer overrun %d at period %ld\n",
			    orun, new_period);

	memguard_on_each_cpu_mask(active_mask,
		period_timer_callback_slave, (void *)new_period, 0);

	DEBUG(trace_printk("master end\n"));
	return HRTIMER_RESTART;
}

static struct perf_event *init_counter(int cpu, int budget)
{
	struct perf_event *event = NULL;
	struct perf_event_attr sched_perf_hw_attr = {
		/* use generalized hardware abstraction */
		.type           = PERF_TYPE_HARDWARE,
		.config         = PERF_COUNT_HW_CACHE_MISSES,
		.size		= sizeof(struct perf_event_attr),
		.pinned		= 1,
		.disabled	= 1,
		.exclude_kernel = 1,   /* TODO: 1 mean, no kernel mode counting */
		.pinned = 1,
	};

	if (!strcmp(g_hw_type, "core2")) {
		sched_perf_hw_attr.type           = PERF_TYPE_RAW;
		sched_perf_hw_attr.config         = 0x7024; /* 7024 - incl. prefetch 
							       5024 - only prefetch
							       4024 - excl. prefetch */
	} else if (!strcmp(g_hw_type, "snb")) {
		sched_perf_hw_attr.type           = PERF_TYPE_RAW;
		sched_perf_hw_attr.config         = 0x08b0; /* 08b0 - incl. prefetch */
	} else if (!strcmp(g_hw_type, "soft")) {
		sched_perf_hw_attr.type           = PERF_TYPE_SOFTWARE;
		sched_perf_hw_attr.config         = PERF_COUNT_SW_CPU_CLOCK;
	}

	/* select based on requested event type */
	sched_perf_hw_attr.sample_period = budget;

	/* Try to register using hardware perf events */
	event = perf_event_create_kernel_counter(
		&sched_perf_hw_attr,
		cpu, NULL,
		event_overflow_callback
#if LINUX_VERSION_CODE > KERNEL_VERSION(3, 2, 0)
		,NULL
#endif
		);

	if (!event)
		return NULL;

	if (IS_ERR(event)) {
		/* vary the KERN level based on the returned errno */
		if (PTR_ERR(event) == -EOPNOTSUPP)
			pr_info("cpu%d. not supported\n", cpu);
		else if (PTR_ERR(event) == -ENOENT)
			pr_info("cpu%d. not h/w event\n", cpu);
		else
			pr_err("cpu%d. unable to create perf event: %ld\n",
			       cpu, PTR_ERR(event));
		return NULL;
	}

	/* success path */
	pr_info("cpu%d enabled counter.\n", cpu);

	smp_wmb();

	return event;
}

static void __kill_throttlethread(void *info)
{
	struct core_info *cinfo = this_cpu_ptr(core_info);
	pr_info("Stopping kthrottle/%d\n", smp_processor_id());
	cinfo->throttled_task = NULL;
	smp_mb();
}

static void __disable_counter(void *info)
{
	struct core_info *cinfo = this_cpu_ptr(core_info);
	BUG_ON(!cinfo->event);

	/* stop the counter */
	cinfo->event->pmu->stop(cinfo->event, PERF_EF_UPDATE);
	cinfo->event->pmu->del(cinfo->event, 0);

	pr_info("LLC bandwidth throttling disabled\n");
}

static void disable_counters(void)
{
	on_each_cpu(__disable_counter, NULL, 0);
}


static void __start_counter(void* info)
{
	struct core_info *cinfo = this_cpu_ptr(core_info);
	cinfo->event->pmu->add(cinfo->event, PERF_EF_START);
}

static void start_counters(void)
{
	on_each_cpu(__start_counter, NULL, 0);
}

/**************************************************************************
 * Local Functions
 **************************************************************************/

static ssize_t memguard_control_write(struct file *filp,
				    const char __user *ubuf,
				    size_t cnt, loff_t *ppos)
{
	char buf[BUF_SIZE];
	char *p = buf;
	struct memguard_info *global = &memguard_info;

	if (copy_from_user(&buf, ubuf, (cnt > BUF_SIZE) ? BUF_SIZE: cnt) != 0)
		return 0;

	if (!strncmp(p, "maxbw ", 6)) {
		sscanf(p+6, "%d", &g_budget_max_bw);
		global->max_budget =
			convert_mb_to_events(g_budget_max_bw);
		WARN_ON(global->max_budget == 0);
	}
	else if (!strncmp(p, "taskprio ", 9))
		sscanf(p+9, "%d", &g_use_task_priority);
	else if (!strncmp(p, "reclaim ", 8))
		sscanf(p+8, "%d", &g_use_reclaim);
	else if (!strncmp(p, "exclusive ", 10))
		sscanf(p+10, "%d", &g_use_exclusive);
	else
		pr_info("ERROR: %s\n", p);
	smp_mb();
	return cnt;
}

static int memguard_control_show(struct seq_file *m, void *v)
{
	char buf[64];
	struct memguard_info *global = &memguard_info;

	seq_printf(m, "maxbw: %d (MB/s)\n", g_budget_max_bw);
	seq_printf(m, "reclaim: %d\n", g_use_reclaim);
	seq_printf(m, "exclusive: %d\n", g_use_exclusive);
	seq_printf(m, "taskprio: %d\n", g_use_task_priority);
	cpulist_scnprintf(buf, 64, global->active_mask);
	seq_printf(m, "active: %s\n", buf);
	cpulist_scnprintf(buf, 64, global->throttle_mask);
	seq_printf(m, "throttle: %s\n", buf);
	return 0;
}

static int memguard_control_open(struct inode *inode, struct file *filp)
{
	return single_open(filp, memguard_control_show, NULL);
}

static const struct file_operations memguard_control_fops = {
	.open		= memguard_control_open,
	.write          = memguard_control_write,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};


static void __update_budget(void *info)
{
	struct core_info *cinfo = this_cpu_ptr(core_info);

	if ((unsigned long)info == 0) {
		pr_info("ERR: Requested budget is zero\n");
		return;
	}
	cinfo->limit = (unsigned long)info;
	cinfo->weight = 0;
	smp_mb();
	DEBUG_USER(trace_printk("MSG: New budget of Core%d is %d\n",
				smp_processor_id(), cinfo->budget));

}

static void __update_weight(void *info)
{
	struct core_info *cinfo = this_cpu_ptr(core_info);

	if ((unsigned long)info == 0) {
		pr_info("ERR: Requested weight is zero\n");
		return;
	}

	cinfo->weight = (unsigned long)info;
	cinfo->limit = 0;
	smp_mb();
	DEBUG_USER(trace_printk("MSG: New weight of Core%d is %d\n",
				smp_processor_id(), cinfo->weight));
}

#define MAXPERF_EVENTS 1638400  /* 100000 MB/s */
#define THROTTLE_EVENTS 1638     /*    100 MB/s */
 
static ssize_t memguard_limit_write(struct file *filp,
				    const char __user *ubuf,
				    size_t cnt, loff_t *ppos)
{
	char buf[BUF_SIZE];
	char *p = buf;
	int i;
	int max_budget = 0;
	int use_mb = 0;
	struct memguard_info *global = &memguard_info;
	struct core_info *cinfo;

	if (copy_from_user(&buf, ubuf, (cnt > BUF_SIZE) ? BUF_SIZE: cnt) != 0) 
		return 0;


	/* bw_lock support */
	if (!strncmp(p, "bw_lock ", 8)) {
		int input, core; 
		unsigned long events;
		p += 8; 
		sscanf(p, "%d %d", &core, &input); /* coreid, bw_mb */

		events = (unsigned long)convert_mb_to_events(input);

		cinfo = per_cpu_ptr(core_info, core);
		cinfo->limit = events;
		cinfo->weight = 0;
		global->bw_locked_core++;
		if (global->bw_locked_core == 1) { 
			/* initiate bw_lock */
			for_each_online_cpu(i) {
				struct core_info *cinfo = per_cpu_ptr(core_info, i);
				if (i != core) {
					/* throttle unlocked cores */
					cinfo->limit = THROTTLE_EVENTS;
					cinfo->weight = 0;
				}
			}
		}
		
		DEBUG(trace_printk("bw_lock: throttle cores except %d\n", core));
		goto out;
		
	} else if (!strncmp(p, "bw_unlock ", 10)) {
		int core; 
		p += 10; 
		sscanf(p, "%d", &core); 

		cinfo = per_cpu_ptr(core_info, core);
		cinfo->limit = MAXPERF_EVENTS;
		cinfo->weight = 0;
		global->bw_locked_core--;
		if (global->bw_locked_core == 0) {
			/* unlock all throttled cores */
			for_each_online_cpu(i) {
				struct core_info *cinfo = per_cpu_ptr(core_info, i);
				if (cinfo->limit == THROTTLE_EVENTS) {
					/* throttle unlocked cores */
					cinfo->limit = MAXPERF_EVENTS;
					cinfo->weight = 0;
				}
			}

		}

		DEBUG(trace_printk("bw_unlock: lock cores\n"));
		goto out;
	}

	if (!strncmp(p, "mb ", 3)) {
		use_mb = 1;
		p+=3;
	}
	for_each_online_cpu(i) {
		int input;
		unsigned long events;
		sscanf(p, "%d", &input);
		if (input == 0) {
			pr_err("ERR: CPU%d: input is zero: %s.\n",i, p);
			continue;
		}
		if (!use_mb)
			input = g_budget_max_bw*100/input;
		events = (unsigned long)convert_mb_to_events(input);
		max_budget += events;
		pr_info("CPU%d: New budget=%ld (%d %s)\n", i, 
			events, input, (use_mb)?"MB/s": "pct");
		smp_call_function_single(i, __update_budget,
					 (void *)events, 0);
		
		p = strchr(p, ' ');
		if (!p) break;
		p++;
	}
	global->max_budget = max_budget;
	g_budget_max_bw = convert_events_to_mb(max_budget);

out:
	smp_mb();
	return cnt;
}

static int memguard_limit_show(struct seq_file *m, void *v)
{
	int i, cpu;
	int wsum = 0;
	struct memguard_info *global = &memguard_info;
	cpu = get_cpu();

	smp_mb();
	seq_printf(m, "cpu  |budget (MB/s,pct,weight)\n");
	seq_printf(m, "-------------------------------\n");

	for_each_online_cpu(i)
		wsum += per_cpu_ptr(core_info, i)->weight;
	for_each_online_cpu(i) {
		struct core_info *cinfo = per_cpu_ptr(core_info, i);
		int budget = 0, pct;
		if (cinfo->limit > 0)
			budget = cinfo->limit;
		else if (cinfo->weight > 0) {
			budget = (int)div64_u64((u64)global->max_budget*
						cinfo->weight, wsum);
		}
		WARN_ON_ONCE(budget == 0);
		pct = div64_u64((u64)budget * 100 + (global->max_budget-1),
				(global->max_budget) ? global->max_budget : 1);
		seq_printf(m, "CPU%d: %d (%dMB/s, %d pct, w%d)\n", 
			   i, budget,
			   convert_events_to_mb(budget),
			   pct, cinfo->weight);
	}
	seq_printf(m, "g_budget_max_bw: %d MB/s, (%d)\n", g_budget_max_bw,
		global->max_budget);
	put_cpu();
	return 0;
}

static int memguard_limit_open(struct inode *inode, struct file *filp)
{
	return single_open(filp, memguard_limit_show, NULL);
}

static const struct file_operations memguard_limit_fops = {
	.open		= memguard_limit_open,
	.write          = memguard_limit_write,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};


static ssize_t memguard_share_write(struct file *filp,
				    const char __user *ubuf,
				    size_t cnt, loff_t *ppos)
{
	char buf[BUF_SIZE];
	char *p = buf;
	int i, cpu;

	if (copy_from_user(&buf, ubuf, (cnt > BUF_SIZE) ? BUF_SIZE: cnt) != 0)
		return 0;
	cpu = get_cpu();
	for_each_online_cpu(i) {
		unsigned long input;
		sscanf(p, "%ld", &input);

		pr_info("CPU%d: input=%ld\n", i, input);
		if (input == 0)
			input = 1024;
		pr_info("CPU%d: New weight=%ld\n", i, input);
		smp_call_function_single(i, __update_weight,
					 (void *)input, 0);
		p = strchr(p, ' ');
		if (!p) break;
		p++;
	}
	put_cpu();
	return cnt;
}


static const struct file_operations memguard_share_fops = {
	.open		= memguard_limit_open,
	.write          = memguard_share_write,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};



/**
 * Display usage statistics
 *
 * TODO: use IPI
 */
static int memguard_usage_show(struct seq_file *m, void *v)
{
	int i, j;

	smp_mb();

	/* current utilization */
	for (j = 0; j < 3; j++) {
		for_each_online_cpu(i) {
			struct core_info *cinfo = per_cpu_ptr(core_info, i);
			u64 budget, used, util;

			budget = cinfo->budget;
			used = cinfo->used[j];
			util = div64_u64(used * 100, (budget) ? budget : 1);
			seq_printf(m, "%llu ", util);
		}
		seq_printf(m, "\n");
	}
	seq_printf(m, "<overall>----\n");

	/* overall utilization
	   WARN: assume budget did not changed */
	for_each_online_cpu(i) {
		struct core_info *cinfo = per_cpu_ptr(core_info, i);
		u64 total_budget, total_used, result;

		total_budget = cinfo->overall.assigned_budget;
		total_used   = cinfo->overall.used_budget;
		result       = div64_u64(total_used * 100, 
					 (total_budget) ? total_budget : 1 );
		seq_printf(m, "%lld ", result);
	}
	return 0;
}

static int memguard_usage_open(struct inode *inode, struct file *filp)
{
	return single_open(filp, memguard_usage_show, NULL);
}

static const struct file_operations memguard_usage_fops = {
	.open		= memguard_usage_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static void __reset_stats(void *info)
{
	struct core_info *cinfo = this_cpu_ptr(core_info);
	trace_printk("CPU%d\n", smp_processor_id());

	/* update local period information */
	cinfo->period_cnt = 0;
	cinfo->used[0] = cinfo->used[1] = cinfo->used[2] =
		cinfo->budget; /* initial condition */
	cinfo->cur_budget = cinfo->budget;
	cinfo->overall.used_budget = 0;
	cinfo->overall.assigned_budget = 0;
	cinfo->overall.throttled_time_ns = 0;
	cinfo->overall.throttled = 0;
	cinfo->overall.throttled_error = 0;
	memset(cinfo->overall.throttled_error_dist, 0, sizeof(int)*10);
	cinfo->throttled_time = ktime_set(0,0);
	smp_mb();

	DEBUG_USER(trace_printk("MSG: Clear statistics of Core%d\n",
				smp_processor_id()));
}


static ssize_t memguard_failcnt_write(struct file *filp,
				    const char __user *ubuf,
				    size_t cnt, loff_t *ppos)
{
	/* reset local statistics */
	struct memguard_info *global = &memguard_info;

	spin_lock(&global->lock);
	global->budget = global->period_cnt = 0;
	global->start_tick = jiffies;
	spin_unlock(&global->lock);

	smp_mb();
	on_each_cpu(__reset_stats, NULL, 0);
	return cnt;
}

static int memguard_failcnt_show(struct seq_file *m, void *v)
{
	int i;
	struct memguard_info *global = &memguard_info;

	smp_mb();
	/* total #of throttled periods */
	seq_printf(m, "throttled: ");
	for_each_online_cpu(i) {
		struct core_info *cinfo = per_cpu_ptr(core_info, i);
		seq_printf(m, "%d ", cinfo->overall.throttled);
	}
	seq_printf(m, "\nthrottle_error: ");
	for_each_online_cpu(i) {
		struct core_info *cinfo = per_cpu_ptr(core_info, i);
		seq_printf(m, "%lld ", cinfo->overall.throttled_error);
	}

	seq_printf(m, "\ncore-pct   10    20    30    40    50    60    70    80    90    100\n");
	seq_printf(m, "--------------------------------------------------------------------");
	for_each_online_cpu(i) {
		int idx;
		struct core_info *cinfo = per_cpu_ptr(core_info, i);
		seq_printf(m, "\n%4d    ", i);
		for (idx = 0; idx < 10; idx++)
			seq_printf(m, "%5d ",
				cinfo->overall.throttled_error_dist[idx]);
	}

	/* total #of exclusive mode periods */
	seq_printf(m, "\nexclusive: ");
	for_each_online_cpu(i) {
		struct core_info *cinfo = per_cpu_ptr(core_info, i);
		seq_printf(m, "%d(%lld ms|%lld MB) ", cinfo->overall.exclusive,
			   cinfo->overall.exclusive_ns >> 20, 
			   (cinfo->overall.exclusive_bw * CACHE_LINE_SIZE) >> 20);
	}

	/* out of total periods */
	seq_printf(m, "\ntotal_periods: ");
	for_each_online_cpu(i)
		seq_printf(m, "%ld ", per_cpu_ptr(core_info, i)->period_cnt);

	seq_printf(m, "\nbw_locked_core: %ld\n", global->bw_locked_core);


	return 0;
}

static int memguard_failcnt_open(struct inode *inode, struct file *filp)
{
	return single_open(filp, memguard_failcnt_show, NULL);
}

static const struct file_operations memguard_failcnt_fops = {
	.open		= memguard_failcnt_open,
	.write          = memguard_failcnt_write,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static int memguard_init_debugfs(void)
{

	memguard_dir = debugfs_create_dir("memguard", NULL);
	BUG_ON(!memguard_dir);
	debugfs_create_file("control", 0444, memguard_dir, NULL,
			    &memguard_control_fops);

	debugfs_create_file("limit", 0444, memguard_dir, NULL,
			    &memguard_limit_fops);

	debugfs_create_file("share", 0444, memguard_dir, NULL,
			    &memguard_share_fops);

	debugfs_create_file("usage", 0666, memguard_dir, NULL,
			    &memguard_usage_fops);

	debugfs_create_file("failcnt", 0644, memguard_dir, NULL,
			    &memguard_failcnt_fops);
	return 0;
}

static int throttle_thread(void *arg)
{
	int cpunr = (unsigned long)arg;
	struct core_info *cinfo = per_cpu_ptr(core_info, cpunr);

	static const struct sched_param param = {
		.sched_priority = MAX_USER_RT_PRIO/2,
	};

	sched_setscheduler(current, SCHED_FIFO, &param);

	while (!kthread_should_stop() && cpu_online(cpunr)) {

		DEBUG(trace_printk("wait an event\n"));
		wait_event_interruptible(cinfo->throttle_evt,
					 cinfo->throttled_task ||
					 kthread_should_stop());

		DEBUG(trace_printk("got an event\n"));

		if (kthread_should_stop())
			break;

		smp_mb();
		while (cinfo->throttled_task && !kthread_should_stop())
		{
			cpu_relax();
			/* TODO: mwait */
			smp_mb();
		}
	}

	DEBUG(trace_printk("exit\n"));
	return 0;
}

/* Idle notifier to look at idle CPUs */
static int memguard_idle_notifier(struct notifier_block *nb, unsigned long val,
				void *data)
{
	struct memguard_info *global = &memguard_info;
	unsigned long flags;

	DEBUG(trace_printk("idle state update: %ld\n", val));

	spin_lock_irqsave(&global->lock, flags);
	if (val == IDLE_START) {
		cpumask_clear_cpu(smp_processor_id(), global->active_mask);
		if (cpumask_equal(global->throttle_mask, global->active_mask))
			trace_printk("DBG: last idle\n");
	} else
		cpumask_set_cpu(smp_processor_id(), global->active_mask);
	spin_unlock_irqrestore(&global->lock, flags);
	return 0;
}

static struct notifier_block memguard_idle_nb = {
	.notifier_call = memguard_idle_notifier,
};


int init_module( void )
{
	int i;

	struct memguard_info *global = &memguard_info;

	/* initialized memguard_info structure */
	memset(global, 0, sizeof(struct memguard_info));
	zalloc_cpumask_var(&global->throttle_mask, GFP_NOWAIT);
	zalloc_cpumask_var(&global->active_mask, GFP_NOWAIT);

	if (g_period_us < 0 || g_period_us > 1000000) {
		printk(KERN_INFO "Must be 0 < period < 1 sec\n");
		return -ENODEV;
	}

	if (g_test) {
		self_test();
		return -ENODEV;
	}

	spin_lock_init(&global->lock);
	global->start_tick = jiffies;
	global->period_in_ktime = ktime_set(0, g_period_us * 1000);
	global->max_budget = convert_mb_to_events(g_budget_max_bw);

	/* initialize all online cpus to be active */
	cpumask_copy(global->active_mask, cpu_online_mask);

	pr_info("ARCH: %s\n", g_hw_type);
	pr_info("HZ=%d, g_period_us=%d\n", HZ, g_period_us);

	/* Memory performance characteristics */
	if (g_budget_max_bw == 0) {
		printk(KERN_INFO "budget_max must be set\n");
		return -ENODEV;
	}

	pr_info("Max. b/w: %d (MB/s)\n", g_budget_max_bw);
	pr_info("Max. events per %d us: %lld\n", g_period_us,
	       convert_mb_to_events(g_budget_max_bw));

	pr_info("Initilizing perf counter\n");
	core_info = alloc_percpu(struct core_info);
	smp_mb();

	get_online_cpus();
	for_each_online_cpu(i) {
		struct perf_event *event;
		struct core_info *cinfo = per_cpu_ptr(core_info, i);

		int budget, mb;
		/* initialize counter h/w & event structure */
		if (g_budget_pct[i] == 0) /* uninitialized. assign max value */
			g_budget_pct[i] = 100 / num_online_cpus();
		mb = div64_u64((u64)g_budget_max_bw * g_budget_pct[i],  100);
		budget = convert_mb_to_events(mb);
		pr_info("budget[%d] = %d (%d pct, %d MB/s)\n", i,
		       budget,g_budget_pct[i], mb);

		/* create performance counter */
		event = init_counter(i, budget);
		if (!event)
			break;

		/* initialize per-core data structure */
		smp_call_function_single(i, __init_per_core, (void *)event, 1);
		smp_mb();


		/* create and wake-up throttle threads */
		cinfo->throttle_thread =
			kthread_create_on_node(throttle_thread,
					       (void *)((unsigned long)i),
					       cpu_to_node(i),
					       "kthrottle/%d", i);

		BUG_ON(IS_ERR(cinfo->throttle_thread));
		kthread_bind(cinfo->throttle_thread, i);
		wake_up_process(cinfo->throttle_thread);
	}

	register_hotcpu_notifier(&memguard_cpu_notifier);

	memguard_init_debugfs();

	pr_info("Start event counters\n");
	start_counters();
	smp_mb();

	pr_info("Start period timer (period=%lld us)\n",
		div64_u64(global->period_in_ktime.tv64, 1000));

	get_cpu();
	global->master = smp_processor_id();

	hrtimer_init(&global->hr_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL_PINNED );
	global->hr_timer.function = &period_timer_callback_master;
	hrtimer_start(&global->hr_timer, global->period_in_ktime, 
		      HRTIMER_MODE_REL_PINNED);
	put_cpu();

	idle_notifier_register(&memguard_idle_nb);
	return 0;
}

void cleanup_module( void )
{
	int i;

	struct memguard_info *global = &memguard_info;

	smp_mb();

	get_online_cpus();

	on_each_cpu(__kill_throttlethread, NULL, 1);

	/* unregister sched-tick callback */
	pr_info("Cancel timer\n");
	hrtimer_cancel(&global->hr_timer);

	/* stop perf_event counters */
	disable_counters();

	/* destroy perf objects */
	for_each_online_cpu(i) {
		struct core_info *cinfo = per_cpu_ptr(core_info, i);
		pr_info("Stopping kthrottle/%d\n", i);
		cinfo->throttled_task = NULL;
		kthread_stop(cinfo->throttle_thread);
		perf_event_release_kernel(cinfo->event);
	}

	/* unregister callbacks */
	idle_notifier_unregister(&memguard_idle_nb);
	unregister_hotcpu_notifier(&memguard_cpu_notifier);

	/* remove debugfs entries */
	debugfs_remove_recursive(memguard_dir);

	/* free allocated data structure */
	free_cpumask_var(global->throttle_mask);
	free_cpumask_var(global->active_mask);
	free_percpu(core_info);

	pr_info("module uninstalled successfully\n");
	return;
}

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Heechul Yun <heechul@illinois.edu>");


static void test_ipi_cb(void *info)
{
	trace_printk("IPI called on %d\n", smp_processor_id());
}

#if 0
static void test_ipi_master(void)
{
	ktime_t start, duration;
	int i;

	/* Measuring IPI overhead */
	trace_printk("self-test begin\n");
	start = ktime_get();
	for (i = 0; i < g_test; i++) {
		on_each_cpu(test_ipi_cb, 0, 0);
	}
	duration = ktime_sub(ktime_get(), start);
	trace_printk("self-test end\n");
	pr_info("#iterations: %d | duration: %lld us | average: %lld ns\n",
		g_test, duration.tv64/1000, duration.tv64/g_test);
}
#endif

enum hrtimer_restart test_timer_cb(struct hrtimer *timer)
{
	ktime_t now;
	now = timer->base->get_time();
	hrtimer_forward(timer, now, ktime_set(0, 1000 * 1000));
	trace_printk("master begin\n");
	on_each_cpu(test_ipi_cb, 0, 0);
	trace_printk("master end\n");
	g_test--;
	smp_mb();
	if (g_test == 0) 
		return HRTIMER_NORESTART;
	else
		return HRTIMER_RESTART;
}

static int self_test(void)
{
	struct hrtimer __test_hr_timer;

	hrtimer_init(&__test_hr_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL_PINNED );
	__test_hr_timer.function = &test_timer_cb;
	hrtimer_start(&__test_hr_timer, ktime_set(0, 1000 * 1000), /* 1ms */
		      HRTIMER_MODE_REL_PINNED);

	while (g_test) {
		smp_mb();
		cpu_relax();
	}

	return 0;
}

