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
#define DEBUG 1

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

#include "bwlock.h"

/**************************************************************************
 * Public Definitions
 **************************************************************************/
#define MAX_NPROC 64

#define BUF_SIZE 32
#define my_printf(fd, fmt, args...) \
{\
	char buf[BUF_SIZE]; int len;	                            \
	sprintf(buf, fmt, ##args);				    \
	len = write(fd, buf, BUF_SIZE);				    \
	if (len < BUF_SIZE) {					    \
		fprintf(stderr, "ERR: buf=%s, len=%d\n", buf, len); \
		return -1;					    \
	}							    \
	return 0;						    \
}

/**************************************************************************
 * Global Variables
 **************************************************************************/
static int fd_limit;
static int fd_control;
static long r_min_mb = 1200; 
static long q_min_mb = 100;
static int n_proc, core_id, node_id;

/**************************************************************************
 * Local Implementation
 **************************************************************************/
int bw_lock_init()
{
	int ret; 
	assert(fd_control == 0);
	fd_control = open("/sys/kernel/debug/memguard/control", O_RDWR);
	assert(fd_control);
	fd_limit = open("/sys/kernel/debug/memguard/limit", O_RDWR);
	assert(fd_limit);
	n_proc = (int)sysconf(_SC_NPROCESSORS_ONLN);
	core_id = sched_getcpu();
	assert(core_id >= 0); 

	fprintf(stdout, "N_PROC: %d, CoreId: %d\n",
		n_proc, core_id);
	assert(node_id == 0);
}

int bw_lock(int reserve_mb, bw_attr_t attr)
{
	my_printf(fd_limit, "bw_lock %d %d\n", core_id, reserve_mb);
}

int bw_unlock(bw_attr_t *attr)
{
	my_printf(fd_limit, "bw_unlock %d\n", core_id);
}

int set_attr(bw_attr_t attr)
{
	my_printf(fd_control, "set_attr %d %d\n", core_id, attr);
}
