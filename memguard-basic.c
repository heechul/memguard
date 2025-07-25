/**
 * Memory bandwidth controller for multi-core systems
 *
 * Copyright (C) 2013  Heechul Yun <heechul@illinois.edu>
 *           (C) 2022  Heechul Yun <heechul.yun@ku.edu>
 *
 * This file is distributed under GPL v2 License. 
 * See LICENSE.TXT for details.
 *
 */

/**************************************************************************
 * Conditional Compilation Options
 **************************************************************************/
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#define DEBUG(x)
#define DEBUG_USER(x)

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

#if LINUX_VERSION_CODE > KERNEL_VERSION(5, 0, 0)
#  include <uapi/linux/sched/types.h>
#elif LINUX_VERSION_CODE > KERNEL_VERSION(4, 13, 0)
#  include <linux/sched/types.h>
#elif LINUX_VERSION_CODE > KERNEL_VERSION(3, 8, 0)
#  include <linux/sched/rt.h>
#endif
#include <linux/sched.h>

/**************************************************************************
 * Public Definitions
 **************************************************************************/
#define CACHE_LINE_SIZE 64
#define BUF_SIZE 256

#define DEFAULT_PERIOD_US 1000 // 1ms
#define DEFAULT_RD_BUDGET_MB 2000 // 2GB/s

#if defined(__aarch64__) || defined(__arm__)
#  define PMU_LLC_MISS_COUNTER_ID 0x17   // LINE_REFILL
#  define PMU_LLC_WB_COUNTER_ID   0x18   // LINE_WB
#elif defined(__x86_64__) || defined(__i386__)
#  define PMU_LLC_MISS_COUNTER_ID 0x08b0 // OFFCORE_REQUESTS.ALL_DATA_RD
#  define PMU_LLC_WB_COUNTER_ID   0x40b0 // OFFCORE_REQUESTS.WB
#elif defined(__riscv)
// Note: These performance counters are specific to the T-Head C910.
// 		 They may not function correctly on other RISC-V designs.
#  define PMU_LLC_MISS_COUNTER_ID 0x11   // LL_CACHE_READ_MISS
#  define PMU_LLC_WB_COUNTER_ID   0x13   // LL_CACHE_WRITE_MISS
#endif

#if LINUX_VERSION_CODE > KERNEL_VERSION(4, 10, 0) // somewhere between 4.4-4.10
#  define TM_NS(x) (x)
#else
#  define TM_NS(x) (x).tv64
#endif

/**************************************************************************
 * Public Types
 **************************************************************************/

/* percpu info */
struct core_info {
	/* user configurations */
	int read_limit;          /* read limit mode */

	/* for control logic */
	int read_budget;         /* assigned read budget */

	int cur_read_budget;     /* currently available read budget */

	struct task_struct * throttled_task; /* throttled task */

	struct irq_work	read_pending;  /* delayed work for NMIs */
	struct perf_event *read_event; /* PMC: LLC misses */
    
	struct task_struct *throttle_thread;  /* forced throttle idle thread */
	wait_queue_head_t throttle_evt; /* throttle wait queue */

	int64_t period_cnt;      /* active periods count */

	int rtcore;              /* never throttle an rt core */
	
	struct hrtimer hr_timer;	 /* per-core period timer */
};

/* global info */
struct memguard_info {
	ktime_t period_in_ktime;
	cpumask_var_t throttle_mask;
	cpumask_var_t active_mask;
};


/**************************************************************************
 * Global Variables
 **************************************************************************/
static struct memguard_info memguard_info;
static struct core_info __percpu *core_info;

static int g_period_us = DEFAULT_PERIOD_US;
static int g_read_budget_mb = DEFAULT_RD_BUDGET_MB;
static int g_read_counter_id = PMU_LLC_MISS_COUNTER_ID;

static struct dentry *memguard_dir;

/**************************************************************************
 * External Function Prototypes
 **************************************************************************/

/**************************************************************************
 * Local Function Prototypes
 **************************************************************************/
static void period_timer_callback_slave(struct core_info *cinfo);
enum hrtimer_restart period_timer_callback_master(struct hrtimer *timer);
static void memguard_read_process_overflow(struct irq_work *entry);
static int throttle_thread(void *arg);

/**************************************************************************
 * Module parameters
 **************************************************************************/

#if LINUX_VERSION_CODE > KERNEL_VERSION(5, 10, 0)
module_param(g_read_counter_id, hexint,  S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
#else
module_param(g_read_counter_id, int,  S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
#endif

module_param(g_period_us, int, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
MODULE_PARM_DESC(g_period_us, "throttling period in usec");

module_param(g_read_budget_mb, int, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
MODULE_PARM_DESC(g_read_budget_mb, "default read budget in MB/s");
/**************************************************************************
 * Module main code
 **************************************************************************/

/** convert MB/s to #of events (i.e., LLC miss counts) per 1ms */
static inline u64 convert_mb_to_events(int mb)
{
	return div64_u64((u64)mb*1024*1024,
			 CACHE_LINE_SIZE * (1000000/g_period_us));
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

static void print_core_info(int cpu, struct core_info *cinfo)
{
	pr_info("CPU%d: budget: %d, cur_budget: %d, period: %ld\n", 
		cpu, cinfo->read_budget, cinfo->cur_read_budget, (long)cinfo->period_cnt);
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

/**
 * memory overflow handler.
 * must not be executed in NMI context. but in hard irq context
 */
static void memguard_read_process_overflow(struct irq_work *entry)
{
	struct core_info *cinfo = this_cpu_ptr(core_info);
	struct memguard_info *global = &memguard_info;
	ktime_t start;

	start = ktime_get();

	BUG_ON(in_nmi() || !in_irq());

    /* no more overflow interrupt */
	local64_set(&cinfo->read_event->hw.period_left, 0xfffffff);

	if (!cpumask_test_cpu(smp_processor_id(), global->active_mask)) {
		trace_printk("ERR: not active\n");
		cinfo->throttled_task = NULL;
		return;
	}
	/* we are going to be throttled */
	cpumask_set_cpu(smp_processor_id(), global->throttle_mask);
	smp_mb(); // w -> r ordering of the local cpu.
	if (cpumask_equal(global->throttle_mask, global->active_mask)) {
		/* all other cores are alreay throttled */
		DEBUG(trace_printk("all cores are throttled. "
					   "skip reclaiming\n"));
	}

	/* set the throttled task */
	cinfo->throttled_task = current;

	/* throttle the core */
	wake_up_interruptible(&cinfo->throttle_evt);
}

/**
 * per-core period timer callback
 *
 *   called while cpu_base->lock is held by hrtimer_interrupt()
 *
 */
enum hrtimer_restart period_timer_callback_master(struct hrtimer *timer)
{
	struct core_info *cinfo = this_cpu_ptr(core_info);
	struct memguard_info *global = &memguard_info;
	int orun;

	/* must be irq disabled. hard irq */
	BUG_ON(!irqs_disabled());
	// WARN_ON_ONCE(!in_interrupt());
	/* stop counter */
	cinfo->read_event->pmu->stop(cinfo->read_event, PERF_EF_UPDATE);
	/* forward timer */
	orun = hrtimer_forward_now(timer, global->period_in_ktime);
	BUG_ON(orun == 0);
	if (orun > 1)
		trace_printk("ERR: timer overrun %d at period %ld\n",
			     orun, (long)cinfo->period_cnt);

	/* assign local period */
	cinfo->period_cnt += orun;
	period_timer_callback_slave(cinfo);

	/* re-enable counter */
	cinfo->read_event->pmu->start(cinfo->read_event, PERF_EF_RELOAD);
	return HRTIMER_RESTART;
}

/**
 * period_timer algorithm:
 *	excess = 0;
 *	if predict < budget:
 *	   excess = budget - predict;
 *	   global += excess
 *	set interrupt at (budget - excess)
 *
 */
static void period_timer_callback_slave(struct core_info *cinfo)
{
	struct memguard_info *global = &memguard_info;
	int cpu = smp_processor_id();

	/* I'm actively participating */
	cpumask_clear_cpu(cpu, global->throttle_mask);
	// smp_mb();
	cpumask_set_cpu(cpu, global->active_mask);

	/* update statistics. */
	// update_statistics(cinfo);

	/* new budget assignment from user */
	if (cinfo->read_limit > 0)
		cinfo->read_budget = max(cinfo->read_limit, 1);

	if (cinfo->read_event->hw.sample_period != cinfo->read_budget) {
		/* new budget is assigned */
		trace_printk("MSG: new budget %d is assigned\n", 
				   cinfo->read_budget);
		cinfo->read_event->hw.sample_period = cinfo->read_budget;
	}

	/* per-task donation policy */
	cinfo->cur_read_budget = cinfo->read_budget;

	/* unthrottle tasks (if any) */
	cinfo->throttled_task = NULL;

    /* setup overflow interrupts except RT cores */
	if (!cinfo->rtcore) {
		/* setup an interrupt */
		local64_set(&cinfo->read_event->hw.period_left, cinfo->cur_read_budget);
	}
}

static struct perf_event *init_counter(int cpu, int budget, int counter_id, void *callback)
{
	struct perf_event *event = NULL;
	struct perf_event_attr sched_perf_hw_attr = {
		.type		= PERF_TYPE_RAW,
		.size		= sizeof(struct perf_event_attr),
		.pinned		= 1,
		.disabled	= 1,
		.config         = counter_id,
		.sample_period  = budget, 
		.exclude_kernel = 1,   /* TODO: 1 mean, no kernel mode counting */
	};

	/* Try to register using hardware perf events */
	event = perf_event_create_kernel_counter(
		&sched_perf_hw_attr,
		cpu, NULL,
		callback
#if LINUX_VERSION_CODE > KERNEL_VERSION(3, 2, 0)
		, NULL
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
	pr_info("cpu%d enabled counter 0x%x\n", cpu, counter_id);

	return event;
}

static void __start_counter(void *info)
{
	struct memguard_info *global = &memguard_info;
	struct core_info *cinfo = this_cpu_ptr(core_info);
	BUG_ON(!cinfo->read_event);

	/* initialize hr timer */
    hrtimer_init(&cinfo->hr_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL_PINNED);
    cinfo->hr_timer.function = &period_timer_callback_master;

	/* start timer */
    hrtimer_start(&cinfo->hr_timer, global->period_in_ktime,
                      HRTIMER_MODE_REL_PINNED);

	/* initialize */
	cinfo->throttled_task = NULL;
	cinfo->period_cnt = 0;

	/* start performance counter */
	/* cinfo->event->pmu->start(cinfo->event, PERF_EF_RELOAD); */
}

static void __stop_counter(void *info)
{
	struct core_info *cinfo = this_cpu_ptr(core_info);
	BUG_ON(!cinfo->read_event);
	
	/* stop the kthrottle/i */
	cinfo->throttled_task = NULL;
	cinfo->period_cnt = -1; // done 

	/* stop the counter */
	cinfo->read_event->pmu->stop(cinfo->read_event, PERF_EF_UPDATE);

	/* stop timer */
	hrtimer_cancel(&cinfo->hr_timer);
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
	if (!strncmp(p, "rt ", 3)) {
		int i, val;
		sscanf(p+3, "%d %d", &i, &val);
		if (i >=0 && i < num_online_cpus())
			per_cpu_ptr(core_info, i)->rtcore = val;
	} else
		pr_info("ERROR: %s\n", p);
	return cnt;
}

static int memguard_control_show(struct seq_file *m, void *v)
{
	struct memguard_info *global = &memguard_info;
	char buf[BUF_SIZE];
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
	.write      = memguard_control_write,
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
	DEBUG_USER(trace_printk("MSG: New read budget of Core%d is %d\n",
				smp_processor_id(), cinfo->read_budget));

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

	seq_printf(m, "cpu  |budget (MB/s)\t RT?\n");
	seq_printf(m, "-------------------------------\n");

	for_each_online_cpu(i) {
		struct core_info *cinfo = per_cpu_ptr(core_info, i);
		int budget = 0;
		if (cinfo->read_limit > 0)
			budget = cinfo->read_limit;

		WARN_ON_ONCE(budget == 0);
		seq_printf(m, "CPU%d: %d (%dMB/s)\t %s\n", 
			   i, budget, convert_events_to_mb(budget),
			   cinfo->rtcore?"Yes":"No");
	}

	put_cpu();
	return 0;
}

static int memguard_read_limit_open(struct inode *inode, struct file *filp)
{
	return single_open(filp, memguard_read_limit_show, NULL);
}

static const struct file_operations memguard_read_limit_fops = {
	.open		= memguard_read_limit_open,
	.write      = memguard_read_limit_write,
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

	return 0;
}

static int throttle_thread(void *arg)
{
	int cpunr = (unsigned long)arg;
	struct core_info *cinfo = per_cpu_ptr(core_info, cpunr);

#if LINUX_VERSION_CODE > KERNEL_VERSION(5, 9, 0)
	sched_set_fifo(current);
#else
	static const struct sched_param param = {
		.sched_priority = MAX_USER_RT_PRIO/2,
	};

	sched_setscheduler(current, SCHED_FIFO, &param);
#endif

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

	global->period_in_ktime = ktime_set(0, g_period_us * 1000);

	/* initialize all online cpus to be active */
	cpumask_copy(global->active_mask, cpu_online_mask);

	pr_info("NR_CPUS: %d, online: %d\n", NR_CPUS, num_online_cpus());

#if defined(__arm__) || defined(__aarch64__)
	/* check if we are running on ARM */
	u32 cpu_id = (u32)read_cpuid_id();
	u32 cpu_part = (cpu_id >> 4) & 0xFFF;
	if (cpu_part == 0xD0B || cpu_part == 0xD42) { // Cortex-A76/A78
		pr_info("Cortex-A76/A78 detected\n");
		g_read_counter_id = 0x002A;
	}
#endif
	pr_info("RAW HW READ COUNTER ID: 0x%x\n", g_read_counter_id);
	pr_info("HZ=%d, g_period_us=%d\n", HZ, g_period_us);
	pr_info("g_read_budget_mb=%d\n", g_read_budget_mb);

	pr_info("Initilizing perf counter\n");
	core_info = alloc_percpu(struct core_info);

	for_each_online_cpu(i) {
		struct core_info *cinfo = per_cpu_ptr(core_info, i);
		int read_budget;

		/* initialize counter h/w & event structure */
		read_budget = convert_mb_to_events(g_read_budget_mb);

		/* initialize per-core data structure */
		memset(cinfo, 0, sizeof(struct core_info));

		/* create performance counter */
		cinfo->read_event = init_counter(i, read_budget, g_read_counter_id,
						 event_overflow_callback);
		if (!cinfo->read_event)
			break;

		/* initialize budget */
		cinfo->read_budget = cinfo->read_limit = cinfo->read_event->hw.sample_period;
		cinfo->cur_read_budget = cinfo->read_budget;

		/* throttled task pointer */
		cinfo->throttled_task = NULL;
		init_waitqueue_head(&cinfo->throttle_evt);

		/* update local period information */
		cinfo->period_cnt = 0;

		print_core_info(smp_processor_id(), cinfo);

		/* initialize nmi irq_work_queue */
		init_irq_work(&cinfo->read_pending, memguard_read_process_overflow);

		/* create and wake-up throttle threads */
		cinfo->throttle_thread =
			kthread_create_on_node(throttle_thread,
					       (void *)((unsigned long)i),
					       cpu_to_node(i),
					       "kthrottle/%d", i);
		perf_event_enable(cinfo->read_event);
		BUG_ON(IS_ERR(cinfo->throttle_thread));
		kthread_bind(cinfo->throttle_thread, i);
		wake_up_process(cinfo->throttle_thread);
	}

	memguard_init_debugfs();

	/* start timer and perf counters */
	pr_info("Start period timer (period=%lld us)\n",
		div64_u64(TM_NS(global->period_in_ktime), 1000));
	on_each_cpu(__start_counter, NULL, 0);

	return 0;
}

void cleanup_module( void )
{
	int i;

	struct memguard_info *global = &memguard_info;

	/* stop perf_event counters and timers */
	on_each_cpu(__stop_counter, NULL, 0);
	pr_info("bandwidth throttling disabled\n");

	/* destroy perf objects */
	for_each_online_cpu(i) {
		struct core_info *cinfo = per_cpu_ptr(core_info, i);
		pr_info("Stopping kthrottle/%d\n", i);
		kthread_stop(cinfo->throttle_thread);
		perf_event_disable(cinfo->read_event);
		perf_event_release_kernel(cinfo->read_event); 
		cinfo->read_event = NULL; 
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
