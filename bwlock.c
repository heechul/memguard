/**
 * BwLock: memory bandwidth lock.
 *
 * Copyright (C) 2014  Heechul Yun <heechul.yun@ku.edu> 
 *
 * This file is distributed under the GPLv2 License. 
 */ 

/**************************************************************************
 * Conditional Compilation Options
 **************************************************************************/
#define USE_DEBUG  0
#define USE_TIMING 1
/**************************************************************************
 * Included Files
 **************************************************************************/
#define _GNU_SOURCE
#include <stdio.h>
#include <sched.h>
#include <stdlib.h>
#include <assert.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdarg.h>
#include <sched.h>
#include <sys/time.h>

#include "bwlock.h"

/**************************************************************************
 * Public Definitions
 **************************************************************************/
#define MAX_NPROC 64

#define CALL_INIT_AUTO 1

#define BUF_SIZE 64

#if USE_DEBUG==1
#  define my_printf(fd, fmt, args...) fprintf(stdout, fmt, ##args)
#else
#  define my_printf(fd, fmt, args...) \
{\
	char buf[BUF_SIZE]; int len;	                            \
	sprintf(buf, fmt, ##args);				    \
	len = write(fd, buf, BUF_SIZE);				    \
	if (len < 0 ) {						    \
		perror("error");				    \
		exit(1);					    \
	} if (len < BUF_SIZE) {					    \
		fprintf(stderr, "ERR: buf=%s, len=%d\n", buf, len); \
		return -1;					    \
	}							    \
}
#endif

#if USE_TIMING==1
unsigned long get_usecs()
{
	struct timeval         time;
	gettimeofday(&time, NULL);
	return (time.tv_sec * 1000000 +	time.tv_usec);
}
#endif

/**************************************************************************
 * Global Variables
 **************************************************************************/
static int fd_limit;
static int fd_control;
static int n_proc, core_id, node_id;
static int use_bw_lock = -1; /* -1 - unknown, 0 - no, 1 - yes */

static int sysctl_max_bw_mb = 10000;
/* static long r_min_mb = 1200;  */
/* static long q_min_mb = 100; */

static unsigned long prev_lock_time;

/**************************************************************************
 * Local Implementation
 **************************************************************************/
int bw_lock_init(void)
{
	char *str;
	if (use_bw_lock == -1) {
		str = getenv("USE_BWLOCK");
		if (str && atoi(str) == 1) 
			use_bw_lock = 1; 
		else
			use_bw_lock = 0;

		fprintf(stderr, "USE_BWLOCK=%d\n", use_bw_lock);
	}

	if (use_bw_lock == 0)
		return -1;

	n_proc = (int)sysconf(_SC_NPROCESSORS_ONLN);
	core_id = sched_getcpu();
	assert(core_id >= 0); 

	fprintf(stderr, "N_PROC: %d, CoreId: %d\n",
		n_proc, core_id);
	assert(node_id == 0);

	assert(fd_control == 0);
	fd_control = open("/sys/kernel/debug/memguard/control", O_RDWR);
	if (fd_control < 0) {
		fprintf(stderr, "memguard is not loaded\n");
		return -1;
	}
	fd_limit = open("/sys/kernel/debug/memguard/limit", O_RDWR);
	assert(fd_limit > 0);

	return 0; 
}

int bw_lock(int reserve_mb, int attr)
{
#if USE_TIMING==1
	if (getenv("USE_TIMING"))
		prev_lock_time = get_usecs();
#endif

#if CALL_INIT_AUTO
	if (fd_limit <= 0 && bw_lock_init() < 0)
		return -1;
#else
	if (fd_limit <= 0) return -1;
#endif

	if (reserve_mb == 0)
		reserve_mb = sysctl_max_bw_mb;

	my_printf(fd_limit, "bw_lock %d %d\n", core_id, reserve_mb);
}

int bw_unlock(int *attr)
{
#if USE_TIMING==1
 	if (getenv("USE_TIMING"))
		fprintf(stderr, "%ld\n", get_usecs() - prev_lock_time);
#endif

#if CALL_INIT_AUTO
	if (fd_limit <= 0 && bw_lock_init() < 0)
		return -1;
#else
	if (fd_limit <= 0) 
		return -1;
#endif
	my_printf(fd_limit, "bw_unlock %d\n", core_id);
}

int set_attr(int attr)
{
	/* TBD: */
	/* if (fd_control <= 0) bw_lock_init(); */
	/* if (fd_control <= 0) return -1; */
	/* my_printf(fd_control, "set_attr %d %d\n", core_id, attr); */
	return 0;
}
