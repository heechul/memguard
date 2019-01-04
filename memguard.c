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

#define USE_RCFS   0
#define USE_BWLOCK 0

#define DEBUG(x)
#define DEBUG_RECLAIM(x)
#define DEBUG_USER(x)
#define DEBUG_BWLOCK(x) 
#define DEBUG_PROFILE(x) x
#define DEBUG_RCFS(x) 

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
#if LINUX_VERSION_CODE > KERNEL_VERSION(4, 13, 0)
#  include <linux/sched/types.h>
#endif

#if LINUX_VERSION_CODE > KERNEL_VERSION(3, 8, 0)
#  include <linux/sched/rt.h>
#endif
#include <linux/sched.h>

/**************************************************************************
 * Public Definitions
 **************************************************************************/
#define MAX_NCPUS 64
#define CACHE_LINE_SIZE 64
#define BUF_SIZE 256
#define PREDICTOR 1  /* 0 - used, 1 - ewma(a=1/2), 2 - ewma(a=1/4) */

#if LINUX_VERSION_CODE > KERNEL_VERSION(4, 10, 0) // somewhere between 4.4-4.10
#  define TM_NS(x) (x)
#else
#  define TM_NS(x) (x).tv64
#endif

/**************************************************************************
 * Public Types
 **************************************************************************/
struct memstat{
	u64 used_read_budget;         /* used read budget */
	u64 used_write_budget;	 /* used write budget */
	u64 assigned_read_budget; /* assigned read budget */
	u64 assigned_write_budget; /* assigned write budget */
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
	int read_budget;              /* assigned read budget */
	int write_budget;        /* assigned write budged */
	int read_limit;               /* read limit mode (exclusive to weight)*/
	int write_limit;		 /* write limit mode (exclusive to weight) */

	/* for control logic */
	int cur_read_budget;          /* currently available read budget */
	int cur_write_budget;		  /* currently available write budget */

	struct task_struct * throttled_task;
	
	ktime_t throttled_time;  /* absolute time when throttled */

	u64 old_read_val;             /* hold previous read counter value */
	u64 old_write_val;		 /* hold previous write counter value */
	int prev_read_throttle_error; /* check whether there was throttle error in 
				    the previous period for the read counter */
    	int prev_write_throttle_error; /* check whether there was throttle error in 
				    the previous period for the write counter */

	int exclusive_mode;      /* 1 - if in exclusive mode */
	ktime_t exclusive_time;  /* time when exclusive mode begins */

	struct irq_work	read_pending;  /* delayed work for NMIs */
	struct perf_event *read_event; /* PMC: LLC misses */
    
    	struct irq_work write_pending;   /* delayed work for NMIs */
	struct perf_event *write_event;  /* PMC: LLC writebacks */                                                
#if USE_RCFS
	struct perf_event *cycle_event; /* PMC: cycles */
	struct perf_event *instr_event; /* PMC: retired instructions */
#endif
	struct task_struct *throttle_thread;  /* forced throttle idle thread */
	wait_queue_head_t throttle_evt; /* throttle wait queue */

	/* statistics */
	struct memstat overall;  /* stat for overall periods. reset by user */
	int read_used[3];        /* EWMA memory load */
	int write_used[3];		 /* EWMA memory load */
	long period_cnt;         /* active periods count */
};

/* global info */
struct memguard_info {
	int master;
	ktime_t period_in_ktime;
	ktime_t cur_period_start;
	int start_tick;
	long period_cnt;
	cpumask_var_t throttle_mask;
	cpumask_var_t active_mask;
	atomic_t wsum;
#if USE_BWLOCK
	int bwlocked_cores;
#endif
	struct hrtimer hr_timer;
};


/**************************************************************************
 * Global Variables
 **************************************************************************/
static struct memguard_info memguard_info;
static struct core_info __percpu *core_info;

static char *g_hw_type = "";
static int g_period_us = 1000;
static int g_use_bwlock = 1;
static int g_use_exclusive = 0;
static int g_read_budget_mb[MAX_NCPUS];
static int g_write_budget_mb[MAX_NCPUS];

static struct dentry *memguard_dir;

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

static const u32 prio_to_wmult[40] = {
 /* -20 */     48388,     59856,     76040,     92818,    118348,
 /* -15 */    147320,    184698,    229616,    287308,    360437,
 /* -10 */    449829,    563644,    704093,    875809,   1099582,
 /*  -5 */   1376151,   1717300,   2157191,   2708050,   3363326,
 /*   0 */   4194304,   5237765,   6557202,   8165337,  10153587,
 /*   5 */  12820798,  15790321,  19976592,  24970740,  31350126,
 /*  10 */  39045157,  49367440,  61356676,  76695844,  95443717,
 /*  15 */ 119304647, 148102320, 186737708, 238609294, 286331153,
};

#if USE_BWLOCK
/* should be defined in the scheduler code */
static int sysctl_maxperf_bw_mb = 100000;
static int sysctl_throttle_bw_mb = 100;
#endif

/**************************************************************************
 * External Function Prototypes
 **************************************************************************/
#if USE_BWLOCK
extern int nr_bwlocked_cores(void);
#endif
extern void register_get_cpi(int (*func_ptr)(void));

/**************************************************************************
 * Local Function Prototypes
 **************************************************************************/
static void period_timer_callback_slave(void *info);
enum hrtimer_restart period_timer_callback_master(struct hrtimer *timer);
static void memguard_read_process_overflow(struct irq_work *entry);
static void memguard_write_process_overflow(struct irq_work *entry);    
static int throttle_thread(void *arg);
static void memguard_on_each_cpu_mask(const struct cpumask *mask,
				      smp_call_func_t func,
				      void *info, bool wait);

/**************************************************************************
 * Module parameters
 **************************************************************************/

module_param(g_hw_type, charp,  S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
MODULE_PARM_DESC(g_hw_type, "hardware type");

module_param(g_use_bwlock, int, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
MODULE_PARM_DESC(g_use_bwlock, "enable/disable reclaim");

module_param(g_period_us, int, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
MODULE_PARM_DESC(g_period_us, "throttling period in usec");

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
static inline u64 memguard_read_event_used(struct core_info *cinfo)
{
	return perf_event_count(cinfo->read_event) - cinfo->old_read_val;
}

/** return used event in the current period */
static inline u64 memguard_write_event_used(struct core_info *cinfo)
{
	return perf_event_count(cinfo->write_event) - cinfo->old_write_val;
}

static void print_core_info(int cpu, struct core_info *cinfo)
{
	pr_info("CPU%d: budget: %d, cur_budget: %d, period: %ld\n", 
	       cpu, cinfo->read_budget, cinfo->cur_read_budget, cinfo->period_cnt);
}

#if USE_RCFS
/* return cpi value of the calling core */
static int get_cpi(void)
{
	struct core_info *cinfo = this_cpu_ptr(core_info);
	u64 old_val, new_val;
	u64 delta_cycles, delta_instrs;
	int wcpi;

	BUG_ON(!cinfo->cycle_event);
	BUG_ON(!cinfo->instr_event);

	old_val = local64_read(&cinfo->cycle_event->count);
	cinfo->cycle_event->pmu->stop(cinfo->cycle_event, PERF_EF_UPDATE);
	new_val = local64_read(&cinfo->cycle_event->count);
	delta_cycles = new_val - old_val;

	old_val = local64_read(&cinfo->instr_event->count);
	cinfo->instr_event->pmu->stop(cinfo->instr_event, PERF_EF_UPDATE);
	new_val = local64_read(&cinfo->instr_event->count);
	delta_instrs = new_val - old_val;
	wcpi = (int)div64_u64(delta_cycles * 1024, delta_instrs + 1);

	cinfo->cycle_event->pmu->start(cinfo->cycle_event, 0);
	cinfo->cycle_event->pmu->start(cinfo->instr_event, 0);
	
	DEBUG_RCFS(trace_printk("d_cycle: %lld d_instr: %lld wcpi: %d\n",
				delta_cycles, delta_instrs, wcpi));

	return wcpi;
}
#endif

#if USE_BWLOCK
static int mg_nr_bwlocked_cores(void)
{
	struct memguard_info *global = &memguard_info;
	int i;
	int nr = nr_bwlocked_cores(); /* check running tasks */
	for_each_cpu(i, global->throttle_mask) {
		struct task_struct *t = (struct task_struct *)
			per_cpu_ptr(core_info, i)->throttled_task;
		if (t && t->bwlock_val > 0)
			nr += t->bwlock_val;
	}
	return nr;
}
#endif

/**
 * update per-core usage statistics
 */
void update_statistics(struct core_info *cinfo)
{
	/* counter must be stopped by now. */
	s64 read_new, write_new;
	int read_used, write_used;

	read_new = perf_event_count(cinfo->read_event);
	read_used = (int)(read_new - cinfo->old_read_val); 

	cinfo->old_read_val = read_new;
	cinfo->overall.used_read_budget += read_used;
	cinfo->overall.assigned_read_budget += cinfo->read_budget;
	
	write_new = perf_event_count(cinfo->write_event);
	write_used = (int)(write_new - cinfo->old_write_val); 

	cinfo->old_write_val = write_new;
	cinfo->overall.used_write_budget += write_used;
	cinfo->overall.assigned_write_budget += cinfo->write_budget;

	/* EWMA filtered per-core usage statistics */
	cinfo->read_used[0] = read_used;
	cinfo->read_used[1] = (cinfo->read_used[1] * (2-1) + read_used) >> 1; 
	/* used[1]_k = 1/2 used[1]_k-1 + 1/2 used */
	cinfo->read_used[2] = (cinfo->read_used[2] * (4-1) + read_used) >> 2; 
	/* used[2]_k = 3/4 used[2]_k-1 + 1/4 used */
	
	/* EWMA filtered per-core usage statistics */
	cinfo->write_used[0] = write_used;
	cinfo->write_used[1] = (cinfo->write_used[1] * (2-1) + write_used) >> 1; 
	/* used[1]_k = 1/2 used[1]_k-1 + 1/2 used */
	cinfo->write_used[2] = (cinfo->write_used[2] * (4-1) + write_used) >> 2;                                          
	/* used[2]_k = 3/4 used[2]_k-1 + 1/4 used */

	/* core is currently throttled. */
	if (cinfo->throttled_task) {
		cinfo->overall.throttled_time_ns +=
			(TM_NS(ktime_get()) - TM_NS(cinfo->throttled_time));
		cinfo->overall.throttled++;
	}

	/* throttling error condition:
	   I was too aggressive in giving up "unsed" budget */
	if (cinfo->prev_read_throttle_error && read_used < cinfo->read_budget) {
		int diff = cinfo->read_budget - read_used;
		int idx;
		cinfo->overall.throttled_error ++; // += diff;
		BUG_ON(cinfo->read_budget == 0);
		idx = (int)(diff * 10 / cinfo->read_budget);
		cinfo->overall.throttled_error_dist[idx]++;
		trace_printk("ERR: throttled_error: %d < %d\n", read_used, cinfo->read_budget);
		/* compensation for error to catch-up*/
		cinfo->read_used[PREDICTOR] = cinfo->read_budget + diff;
	}
	cinfo->prev_read_throttle_error = 0;
    
    	if (cinfo->prev_write_throttle_error && write_used < cinfo->write_budget) {
		int diff = cinfo->write_budget - write_used;
		int idx;
		cinfo->overall.throttled_error ++; // += diff;
		BUG_ON(cinfo->write_budget == 0);
		idx = (int)(diff * 10 / cinfo->write_budget);
		cinfo->overall.throttled_error_dist[idx]++;
		trace_printk("ERR: throttled_error: %d < %d\n", read_used, cinfo->read_budget);
		/* compensation for error to catch-up*/
		cinfo->write_used[PREDICTOR] = cinfo->write_budget + diff;
	}
	cinfo->prev_write_throttle_error = 0;           

	/* I was the lucky guy who used the DRAM exclusively */
	if (cinfo->exclusive_mode) {
		u64 exclusive_ns, exclusive_bw;

		/* used time */
		exclusive_ns = (TM_NS(ktime_get()) -
				TM_NS(cinfo->exclusive_time));
		
		/* used bw */
		exclusive_bw = (cinfo->read_used[0] - cinfo->read_budget);

		cinfo->overall.exclusive_ns += exclusive_ns;
		cinfo->overall.exclusive_bw += exclusive_bw;
		cinfo->exclusive_mode = 0;
		cinfo->overall.exclusive++;
	}
	DEBUG_PROFILE(trace_printk("%lld %d %p CPU%d org: %d cur: %d period: %ld\n",
			   read_new, read_used, cinfo->throttled_task,
			   smp_processor_id(), 
			   cinfo->read_budget,
			   cinfo->cur_read_budget,
			   cinfo->period_cnt));
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
	irq_work_queue(&cinfo->read_pending);
}

static void event_write_overflow_callback(struct perf_event *event,
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 2, 0)
				    int nmi,
#endif
				    struct perf_sample_data *data,
				    struct pt_regs *regs)
{
	struct core_info *cinfo = this_cpu_ptr(core_info);
	BUG_ON(!cinfo);
	irq_work_queue(&cinfo->write_pending);
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
		DEBUG_RECLAIM(trace_printk("exclusive mode begin\n"));
	}
}

static void __newperiod(void *info)
{
	long period = (long)info;
	ktime_t start = ktime_get();
	struct memguard_info *global = &memguard_info;
	
	if (period == global->period_cnt) {
		ktime_t new_expire = ktime_add(start, global->period_in_ktime);
		long new_period = ++global->period_cnt;

		/* arrived before timer interrupt is called */
		hrtimer_start_range_ns(&global->hr_timer, new_expire,
				       0, HRTIMER_MODE_ABS_PINNED);
		DEBUG(trace_printk("begin new period\n"));

		on_each_cpu(period_timer_callback_slave, (void *)new_period, 0);
	} 
}

/**
 * memory overflow handler.
 * must not be executed in NMI context. but in hard irq context
 */
static void memguard_read_process_overflow(struct irq_work *entry)
{
	struct core_info *cinfo = this_cpu_ptr(core_info);
	struct memguard_info *global = &memguard_info;
	ktime_t start;
	s64 read_budget_used;//, write_budget_used;

	start = ktime_get();

	BUG_ON(in_nmi() || !in_irq());

	read_budget_used = memguard_read_event_used(cinfo);

	/* erroneous overflow, that could have happend before period timer
	   stop the pmu */
	if (read_budget_used < cinfo->cur_read_budget) {
		trace_printk("ERR: overflow in timer. used %lld < cur_budget %d. ignore\n",
			     read_budget_used, cinfo->cur_read_budget);
		return;
	}

	/* no more overflow interrupt */
	local64_set(&cinfo->read_event->hw.period_left, 0xfffffff);
    //local64_set(&cinfo->write_event->hw.period_left, 0xfffffff);

	/* check if we donated too much */
	if (read_budget_used < cinfo->read_budget) {
		trace_printk("ERR: throttling error\n");
		cinfo->prev_read_throttle_error = 1;
    }

	if (!cpumask_test_cpu(smp_processor_id(), global->active_mask)) {
		trace_printk("ERR: not active\n");
		return;
	} else if (global->period_cnt != cinfo->period_cnt) {
		trace_printk("ERR: global(%ld) != local(%ld) period mismatch\n",
			     global->period_cnt, cinfo->period_cnt);
		return;
	}

	/* we are going to be throttled */
	cpumask_set_cpu(smp_processor_id(), global->throttle_mask);
	smp_mb(); // w -> r ordering of the local cpu.
	if (cpumask_equal(global->throttle_mask, global->active_mask)) {
		/* all other cores are alreay throttled */
		if (g_use_exclusive == 1) {
			/* algorithm 1: last one get all remaining time */
			cinfo->exclusive_mode = 1;
			cinfo->exclusive_time = ktime_get();
			DEBUG_RECLAIM(trace_printk("exclusive mode begin\n"));
			return;
		} else if (g_use_exclusive == 2) {
			/* algorithm 2: wakeup all (i.e., non regulation) */
			smp_call_function_many(global->throttle_mask, __unthrottle_core, NULL, 0);
			cinfo->exclusive_mode = 1;
			cinfo->exclusive_time = ktime_get();
			cinfo->throttled_task = NULL;
			DEBUG_RECLAIM(trace_printk("exclusive mode begin\n"));
			return;
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
	}

	if (cinfo->prev_read_throttle_error)
		return;
	/*
	 * fail to reclaim. now throttle this core
	 */
	DEBUG_RECLAIM(trace_printk("fail to reclaim after %lld nsec.\n",
				   TM_NS(ktime_get()) - TM_NS(start)));

	/* wake-up throttle task */
	cinfo->throttled_task = current;
	cinfo->throttled_time = start;

	WARN_ON_ONCE(!strncmp(current->comm, "swapper", 7));
	wake_up_interruptible(&cinfo->throttle_evt);
}


static void memguard_write_process_overflow(struct irq_work *entry)
{
	struct core_info *cinfo = this_cpu_ptr(core_info);
	struct memguard_info *global = &memguard_info;
	ktime_t start;
	s64 write_budget_used;

	start = ktime_get();

	BUG_ON(in_nmi() || !in_irq());
    
    write_budget_used = memguard_write_event_used(cinfo);
    
    if (write_budget_used < cinfo->cur_write_budget)
    {
        trace_printk("ERR: overflow in write timer. used %lld < cur_budget %d. ignore\n",
			     write_budget_used, cinfo->cur_write_budget);
		return;
    }

    //local64_set(&cinfo->read_event->hw.period_left, 0xfffffff);
    local64_set(&cinfo->write_event->hw.period_left, 0xfffffff);

	if (write_budget_used < cinfo->write_budget) {
		trace_printk("ERR: throttling error\n");
		cinfo->prev_write_throttle_error = 1;
	}

	if (!cpumask_test_cpu(smp_processor_id(), global->active_mask)) {
		trace_printk("ERR: not active\n");
		return;
	} else if (global->period_cnt != cinfo->period_cnt) {
		trace_printk("ERR: global(%ld) != local(%ld) period mismatch\n",
			     global->period_cnt, cinfo->period_cnt);
		return;
	}

	cpumask_set_cpu(smp_processor_id(), global->throttle_mask);
	smp_mb(); // w -> r ordering of the local cpu.
	if (cpumask_equal(global->throttle_mask, global->active_mask)) {
		if (g_use_exclusive == 1) {
			cinfo->exclusive_mode = 1;
			cinfo->exclusive_time = ktime_get();
			DEBUG_RECLAIM(trace_printk("exclusive mode begin\n"));
			return;
		} else if (g_use_exclusive == 2) {
			smp_call_function_many(global->throttle_mask, __unthrottle_core, NULL, 0);
			cinfo->exclusive_mode = 1;
			cinfo->exclusive_time = ktime_get();
			cinfo->throttled_task = NULL;
			DEBUG_RECLAIM(trace_printk("exclusive mode begin\n"));
			return;
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
	}

	if (cinfo->prev_write_throttle_error)
		return;
	
	trace_printk("WHY\n");
    
	DEBUG_RECLAIM(trace_printk("WHY fail to reclaim after %lld nsec.\n",
				   TM_NS(ktime_get()) - TM_NS(start)));

	cinfo->throttled_task = current;
	cinfo->throttled_time = start;

	WARN_ON_ONCE(!strncmp(current->comm, "swapper", 7));
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
	cinfo->read_event->pmu->stop(cinfo->read_event, PERF_EF_UPDATE);
	cinfo->write_event->pmu->stop(cinfo->write_event, PERF_EF_UPDATE);

	/* I'm actively participating */
	cpumask_clear_cpu(cpu, global->throttle_mask);
	smp_mb();
	cpumask_set_cpu(cpu, global->active_mask);

	/* unthrottle tasks (if any) */
	if (cinfo->throttled_task)
		target = (struct task_struct *)cinfo->throttled_task;
	else
		target = current;
	cinfo->throttled_task = NULL;

#if USE_BWLOCK
	/* bwlock check */
	if (g_use_bwlock) {
		if (global->bwlocked_cores > 0) {
			if (target->bwlock_val > 0)
				cinfo->read_limit = convert_mb_to_events(sysctl_maxperf_bw_mb);
			else
				cinfo->read_limit = convert_mb_to_events(sysctl_throttle_bw_mb);
		} else {
			cinfo->read_limit = convert_mb_to_events(sysctl_maxperf_bw_mb);
		}
	}
	DEBUG_BWLOCK(trace_printk("%s|bwlock_val %d|g->bwlocked_cores %d\n", 
				  (current)?current->comm:"null", 
				  current->bwlock_val, global->bwlocked_cores));
#endif

	DEBUG(trace_printk("%p|New period %ld. global->budget=%d\n",
			   cinfo->throttled_task,
			   cinfo->period_cnt, cinfo->read_budget));
	
	/* update statistics. */
	update_statistics(cinfo);

	/* new budget assignment from user */
	if (cinfo->read_limit > 0) {
		/* limit mode */
		cinfo->read_budget = cinfo->read_limit;
	} 

	/* budget can't be zero? */
	cinfo->read_budget = max(cinfo->read_budget, 1);
	
	/* new budget assignment from user */
	if (cinfo->write_limit > 0) {
		/* limit mode */
		cinfo->write_budget = cinfo->write_limit;
	} 

	/* budget can't be zero? */
	cinfo->write_budget = max(cinfo->write_budget, 1);

	if (cinfo->read_event->hw.sample_period != cinfo->read_budget) {
		/* new budget is assigned */
		trace_printk("MSG: new budget %d is assigned\n", 
				   cinfo->read_budget);
		cinfo->read_event->hw.sample_period = cinfo->read_budget;
	}
	
	if (cinfo->write_event->hw.sample_period != cinfo->write_budget) {
		/* new budget is assigned */
		trace_printk("MSG: new write budget %d is assigned\n", 
				   cinfo->write_budget);
		cinfo->write_event->hw.sample_period = cinfo->write_budget;
	}

	/* per-task donation policy */
	cinfo->cur_read_budget = cinfo->read_budget;
	cinfo->cur_write_budget = cinfo->write_budget;
	
	/* setup an interrupt */
	cinfo->cur_read_budget = max(1, cinfo->cur_read_budget);
	local64_set(&cinfo->read_event->hw.period_left, cinfo->cur_read_budget);
	
	cinfo->cur_write_budget = max(1, cinfo->cur_write_budget);
	local64_set(&cinfo->write_event->hw.period_left, cinfo->cur_write_budget);

	/* enable performance counter */
	cinfo->read_event->pmu->start(cinfo->read_event, PERF_EF_RELOAD);
	cinfo->write_event->pmu->start(cinfo->write_event, PERF_EF_RELOAD);
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

	now = timer->base->get_time();
	global->cur_period_start = now;
        DEBUG(trace_printk("master begin\n"));
	BUG_ON(smp_processor_id() != global->master);

	orun = hrtimer_forward(timer, now, global->period_in_ktime);
	if (orun == 0)
		return HRTIMER_RESTART;

	global->period_cnt += orun;
	new_period = global->period_cnt;

	if (orun > 1)
		trace_printk("ERR: timer overrun %d at period %ld\n",
			    orun, new_period);
#if USE_BWLOCK
	global->bwlocked_cores = mg_nr_bwlocked_cores();
#endif
	memguard_on_each_cpu_mask(global->active_mask,
		period_timer_callback_slave, (void *)new_period, 0);

	DEBUG(trace_printk("master end\n"));
	return HRTIMER_RESTART;
}

#if USE_RCFS
static struct perf_event *init_counting_counter(int cpu, int id)
{
	struct perf_event *event = NULL;
	struct perf_event_attr sched_perf_hw_attr = {
		/* use generalized hardware abstraction */
		.type           = PERF_TYPE_HARDWARE,
		.config         = id,
		.size		= sizeof(struct perf_event_attr),
		.pinned		= 1,
		.disabled	= 1,
		.exclude_kernel = 1,   /* TODO: 1 mean, no kernel mode counting */
	};

	/* Try to register using hardware perf events */
	event = perf_event_create_kernel_counter(
		&sched_perf_hw_attr,
		cpu, NULL,
		NULL
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

	/* This is needed since 4.1? */
	perf_event_enable(event);
	
	/* success path */
	pr_info("cpu%d enabled counter type %d.\n", cpu, (int)id);

	return event;
}
#endif /* RCFS */

static struct perf_event *init_counter(int cpu, int budget, int write)
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
	};

	if (!strcmp(g_hw_type, "core2")) {
		sched_perf_hw_attr.type           = PERF_TYPE_RAW;
		sched_perf_hw_attr.config         = 0x7024; /* 7024 - incl. prefetch 
							       5024 - only prefetch
							       4024 - excl. prefetch */
	} else if (!strcmp(g_hw_type, "snb")) {
		sched_perf_hw_attr.type           = PERF_TYPE_RAW;
		sched_perf_hw_attr.config         = 0x08b0; /* 08b0 - incl. prefetch */
	} else if (!strcmp(g_hw_type, "armv7")) {
		sched_perf_hw_attr.type           = PERF_TYPE_RAW;
		sched_perf_hw_attr.config         = 0x17; /* Level 2 data cache refill */
        if(write == 1)
        {
            sched_perf_hw_attr.config = 0x18;
        }        
	} else if (!strcmp(g_hw_type, "soft")) {
		sched_perf_hw_attr.type           = PERF_TYPE_SOFTWARE;
		sched_perf_hw_attr.config         = PERF_COUNT_SW_CPU_CLOCK;
	} 

	/* select based on requested event type */
	sched_perf_hw_attr.sample_period = budget;

	if(write == 0)
    {
        /* Try to register using hardware perf events */
        event = perf_event_create_kernel_counter(
            &sched_perf_hw_attr,
            cpu, NULL,
            event_overflow_callback
    #if LINUX_VERSION_CODE > KERNEL_VERSION(3, 2, 0)
            ,NULL
    #endif
            );
    }
    else if(write == 1)
    {
        /* Try to register using hardware perf events */
        event = perf_event_create_kernel_counter(
            &sched_perf_hw_attr,
            cpu, NULL,
            event_write_overflow_callback
    #if LINUX_VERSION_CODE > KERNEL_VERSION(3, 2, 0)
            ,NULL
    #endif
            );
    } 
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

	/* This is needed since 4.1? */
	perf_event_enable(event);

	/* success path */
	pr_info("cpu%d enabled counter.\n", cpu);

	return event;
}

static void __disable_counter(void *info)
{
	struct core_info *cinfo = this_cpu_ptr(core_info);
	BUG_ON(!cinfo->read_event);
    BUG_ON(!cinfo->write_event);

	/* stop the counter */
	cinfo->read_event->pmu->stop(cinfo->read_event, PERF_EF_UPDATE);
	cinfo->read_event->pmu->del(cinfo->read_event, 0);
    
    cinfo->write_event->pmu->stop(cinfo->write_event, PERF_EF_UPDATE);
    cinfo->write_event->pmu->del(cinfo->write_event, 0);

#if USE_RCFS
	cinfo->cycle_event->pmu->stop(cinfo->cycle_event, PERF_EF_UPDATE);
	cinfo->cycle_event->pmu->del(cinfo->cycle_event, 0);

	cinfo->instr_event->pmu->stop(cinfo->instr_event, PERF_EF_UPDATE);
	cinfo->instr_event->pmu->del(cinfo->instr_event, 0);
#endif

	pr_info("LLC bandwidth throttling disabled\n");
}

static void disable_counters(void)
{
	on_each_cpu(__disable_counter, NULL, 0);
}


static void __start_counter(void* info)
{
	struct core_info *cinfo = this_cpu_ptr(core_info);
	cinfo->read_event->pmu->add(cinfo->read_event, PERF_EF_START);
	cinfo->write_event->pmu->add(cinfo->write_event, PERF_EF_START);
#if USE_RCFS
	cinfo->cycle_event->pmu->add(cinfo->cycle_event, PERF_EF_START);
	cinfo->instr_event->pmu->add(cinfo->instr_event, PERF_EF_START);
#endif
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
	if (copy_from_user(&buf, ubuf, (cnt > BUF_SIZE) ? BUF_SIZE: cnt) != 0)
		return 0;

	if (!strncmp(p, "exclusive ", 10))
		sscanf(p+10, "%d", &g_use_exclusive);
	else
		pr_info("ERROR: %s\n", p);
	return cnt;
}

static int memguard_control_show(struct seq_file *m, void *v)
{
	struct memguard_info *global = &memguard_info;
	char buf[BUF_SIZE];
	seq_printf(m, "exclusive: %d\n", g_use_exclusive);
	cpumap_print_to_pagebuf(1, buf, global->active_mask);
	seq_printf(m, "active: %s\n", buf);
	cpumap_print_to_pagebuf(1, buf, global->throttle_mask);	
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
	cinfo->read_limit = (unsigned long)info;
	DEBUG_USER(trace_printk("MSG: New budget of Core%d is %d\n",
				smp_processor_id(), cinfo->read_budget));

}

static void __update_write_budget(void *info)
{
	struct core_info *cinfo = this_cpu_ptr(core_info);

	if ((unsigned long)info == 0) {
		pr_info("ERR: Requested budget is zero\n");
		return;
	}
	cinfo->write_limit = (unsigned long)info;
	DEBUG_USER(trace_printk("MSG: New write budget of Core%d is %d\n",
				smp_processor_id(), cinfo->write_budget));

}
static ssize_t memguard_read_limit_write(struct file *filp,
				    const char __user *ubuf,
				    size_t cnt, loff_t *ppos)
{
	char buf[BUF_SIZE];
	char *p = buf;
	int i;
	int use_mb = 0;

	if (copy_from_user(&buf, ubuf, (cnt > BUF_SIZE) ? BUF_SIZE: cnt) != 0) 
		return 0;

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
		if (use_mb)
			events = (unsigned long)convert_mb_to_events(input);
		else
			events = input;

		pr_info("CPU%d: New budget=%ld (%d %s)\n", i, 
			events, input, (use_mb)?"MB/s": "events");
		smp_call_function_single(i, __update_budget,
					 (void *)events, 0);
		
		p = strchr(p, ' ');
		if (!p) break;
		p++;
	}
	return cnt;
}

static int memguard_read_limit_show(struct seq_file *m, void *v)
{
	int i, cpu;
	cpu = get_cpu();

	seq_printf(m, "cpu  |budget (MB/s,pct,weight)\n");
	seq_printf(m, "-------------------------------\n");

	for_each_online_cpu(i) {
		struct core_info *cinfo = per_cpu_ptr(core_info, i);
		int budget = 0;
		if (cinfo->read_limit > 0)
			budget = cinfo->read_limit;

		WARN_ON_ONCE(budget == 0);
		seq_printf(m, "CPU%d: %d (%dMB/s)\n", 
				   i, budget,
				   convert_events_to_mb(budget));
	}
#if USE_BWLOCK
	struct memguard_info *global = &memguard_info;
	seq_printf(m, "bwlocked_core: %d\n", global->bwlocked_cores);
#endif

	put_cpu();
	return 0;
}

static int memguard_read_limit_open(struct inode *inode, struct file *filp)
{
	return single_open(filp, memguard_read_limit_show, NULL);
}

static const struct file_operations memguard_read_limit_fops = {
	.open		= memguard_read_limit_open,
	.write          = memguard_read_limit_write,
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

	/* current utilization */
	for (j = 0; j < 3; j++) {
		for_each_online_cpu(i) {
			struct core_info *cinfo = per_cpu_ptr(core_info, i);
			u64 budget, used, util;

			budget = cinfo->read_budget;
			used = cinfo->read_used[j];
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

		total_budget = cinfo->overall.assigned_read_budget;
		total_used   = cinfo->overall.used_read_budget;
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

static ssize_t memguard_write_limit_write(struct file *filp,
				    const char __user *ubuf,
				    size_t cnt, loff_t *ppos)
{
	char buf[BUF_SIZE];
	char *p = buf;
	int i;
	int use_mb = 0;

	if (copy_from_user(&buf, ubuf, (cnt > BUF_SIZE) ? BUF_SIZE: cnt) != 0) 
		return 0;

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
		if (use_mb)
			events = (unsigned long)convert_mb_to_events(input);
		else
			events = input;

		pr_info("CPU%d: New budget=%ld (%d %s)\n", i, 
			events, input, (use_mb)?"MB/s": "events");
		smp_call_function_single(i, __update_write_budget,
					 (void *)events, 0);
		
		p = strchr(p, ' ');
		if (!p) break;
		p++;
	}
	return cnt;
}

static int memguard_write_limit_show(struct seq_file *m, void *v)
{
	int i, cpu;
	cpu = get_cpu();

	seq_printf(m, "cpu  |budget (MB/s,pct,weight)\n");
	seq_printf(m, "-------------------------------\n");

	for_each_online_cpu(i) {
		struct core_info *cinfo = per_cpu_ptr(core_info, i);
		int budget = 0;
		if (cinfo->write_limit > 0)
			budget = cinfo->write_limit;

		WARN_ON_ONCE(budget == 0);
		seq_printf(m, "CPU%d: %d (%dMB/s)\n", 
				   i, budget,
				   convert_events_to_mb(budget));
	}
#if USE_BWLOCK
	struct memguard_info *global = &memguard_info;
	seq_printf(m, "bwlocked_core: %d\n", global->bwlocked_cores);
#endif

	put_cpu();
	return 0;
}

static int memguard_write_limit_open(struct inode *inode, struct file *filp)
{
	return single_open(filp, memguard_write_limit_show, NULL);
}

static const struct file_operations memguard_write_limit_fops = {
	.open		= memguard_write_limit_open,
	.write          = memguard_write_limit_write,
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

	debugfs_create_file("read_limit", 0444, memguard_dir, NULL,
			    &memguard_read_limit_fops);
				
	debugfs_create_file("write_limit", 0444, memguard_dir, NULL,
			    &memguard_write_limit_fops);

	debugfs_create_file("usage", 0666, memguard_dir, NULL,
			    &memguard_usage_fops);
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

		while (cinfo->throttled_task && !kthread_should_stop())
		{
			smp_mb();
			cpu_relax();
			/* TODO: mwait */
		}
	}

	DEBUG(trace_printk("exit\n"));
	return 0;
}

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

	global->start_tick = jiffies;
	global->period_in_ktime = ktime_set(0, g_period_us * 1000);

	/* initialize all online cpus to be active */
	cpumask_copy(global->active_mask, cpu_online_mask);

	pr_info("ARCH: %s\n", g_hw_type);
	pr_info("HZ=%d, g_period_us=%d\n", HZ, g_period_us);

	pr_info("Initilizing perf counter\n");
	core_info = alloc_percpu(struct core_info);

	get_online_cpus();
	for_each_online_cpu(i) {
		struct core_info *cinfo = per_cpu_ptr(core_info, i);
		int read_budget, write_budget;

		if (i >= MAX_NCPUS) {
			printk(KERN_INFO "too many cores. up to %d is supported\n", MAX_NCPUS);
			return -ENODEV;
		}
		/* initialize counter h/w & event structure */
		if (g_read_budget_mb[i] == 0)
			g_read_budget_mb[i] = 500;
		
		if (g_write_budget_mb[i] == 0)
			g_write_budget_mb[i] = 100;
		
		read_budget = convert_mb_to_events(g_read_budget_mb[i]);
		write_budget = convert_mb_to_events(g_write_budget_mb[i]);
		pr_info("budget[%d] = %d (%d MB)\n", i, read_budget, g_read_budget_mb[i]);

		/* initialize per-core data structure */
		memset(cinfo, 0, sizeof(struct core_info));

		/* create performance counter */
		cinfo->read_event = init_counter(i, read_budget, 0);
		cinfo->write_event = init_counter(i, write_budget, 1);
		if (!cinfo->read_event || !cinfo->write_event)
			break;

		/* initialize budget */
		cinfo->read_budget = cinfo->read_limit = cinfo->read_event->hw.sample_period;
		cinfo->write_budget = cinfo->write_limit = cinfo->write_event->hw.sample_period;


#if USE_RCFS
		/* create cpi events */
		cinfo->cycle_event = init_counting_counter(i, PERF_COUNT_HW_CPU_CYCLES);
		if (!cinfo->cycle_event)
			break;

		cinfo->instr_event = init_counting_counter(i, PERF_COUNT_HW_INSTRUCTIONS);
		if (!cinfo->instr_event)
			break;
#endif

		/* throttled task pointer */
		cinfo->throttled_task = NULL;

		init_waitqueue_head(&cinfo->throttle_evt);

		/* initialize statistics */
		/* update local period information */
		cinfo->period_cnt = 0;
		cinfo->read_used[0] = cinfo->read_used[1] = cinfo->read_used[2] =
			cinfo->read_budget; /* initial condition */
		cinfo->cur_read_budget = cinfo->read_budget;
		cinfo->overall.used_read_budget = 0;
		cinfo->overall.assigned_read_budget = 0;
        
        	cinfo->cur_write_budget = cinfo->write_budget;
        	cinfo->overall.used_write_budget = 0;
        	cinfo->overall.assigned_write_budget = 0;
        
		cinfo->overall.throttled_time_ns = 0;
		cinfo->overall.throttled = 0;
		cinfo->overall.throttled_error = 0;

		memset(cinfo->overall.throttled_error_dist, 0, sizeof(int)*10);
		cinfo->throttled_time = ktime_set(0,0);

		print_core_info(smp_processor_id(), cinfo);

		/* initialize nmi irq_work_queue */
		init_irq_work(&cinfo->read_pending, memguard_read_process_overflow);
        	init_irq_work(&cinfo->write_pending, memguard_write_process_overflow);

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

	memguard_init_debugfs();

	pr_info("Start event counters\n");
	start_counters();

	pr_info("Start period timer (period=%lld us)\n",
		div64_u64(TM_NS(global->period_in_ktime), 1000));

	get_cpu();
	global->master = smp_processor_id();

	hrtimer_init(&global->hr_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL_PINNED );
	global->hr_timer.function = &period_timer_callback_master;
	hrtimer_start(&global->hr_timer, global->period_in_ktime, 
		      HRTIMER_MODE_REL_PINNED);
	put_cpu();

#if USE_RCFS
	/* register cpi function */
	register_get_cpi(&get_cpi);
#endif

	return 0;
}

void cleanup_module( void )
{
	int i;

	struct memguard_info *global = &memguard_info;

#if USE_RCFS
	/* unregister cpi function */
	register_get_cpi(NULL);
#endif
	get_online_cpus();

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

		perf_event_disable(cinfo->read_event);
		perf_event_release_kernel(cinfo->read_event); 
		cinfo->read_event = NULL; 
        
	        perf_event_disable(cinfo->write_event);
       		perf_event_release_kernel(cinfo->write_event);
        	cinfo->write_event = NULL;
#if USE_RCFS
		perf_event_release_kernel(cinfo->cycle_event);
		perf_event_release_kernel(cinfo->instr_event);
		cinfo->cycle_event = cinfo->instr_event = NULL;
#endif
	}

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
