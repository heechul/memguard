/**
 * DRAM access latency measurement program
 *
 * Copyright (C) 2012  Heechul Yun <heechul@illinois.edu>
 *
 * This file is distributed under the University of Illinois Open Source
 * License. See LICENSE.TXT for details.
 *
 */

/**************************************************************************
 * Conditional Compilation Options
 **************************************************************************/

/**************************************************************************
 * Included Files
 **************************************************************************/

#define _GNU_SOURCE             /* See feature_test_macros(7) */
#include <sched.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <sys/time.h>
#include <inttypes.h>
#include <signal.h>
#include <sys/resource.h>
#include <string.h>

/**************************************************************************
 * Public Definitions
 **************************************************************************/
#define CACHE_LINE_SIZE 64
#define CACHE_LINE_BITS 6
#define FRAME_LENGTH (1920*1080)

#define min(x,y) ((x > y) ? y: x)
#define max(x,y) ((x > y) ? x: y)

/**************************************************************************
 * Public Types
 **************************************************************************/
typedef struct {
	int64_t max;
	int64_t min;
	
	int64_t cur;
	int64_t ewma;

	int64_t tot;
	int64_t cnt;
	int miss;
} stat_t;

/**************************************************************************
 * Global Variables
 **************************************************************************/
int *frames[2];
int g_frame_length = FRAME_LENGTH;
stat_t t;

/**************************************************************************
 * Public Function Prototypes
 **************************************************************************/
void init_stat(stat_t *ts)
{
	ts->min = 0x0fffffff;
	ts->max = ts->tot = ts->cnt = ts->miss = ts->cur = ts->ewma = 0;
}

void print_fps()
{
	int64_t avgtime = t.tot / t.cnt;
	float fps = (float)1000000000/avgtime;
	fprintf(stdout, "fps: %.1f %d MB/s avg/min/max: %ld/%ld/%ld(us) miss:%d%%(%d/%ld)\n", fps, (int)(fps*g_frame_length*4/1024/1024),
	       avgtime/1000, t.min/1000, t.max/1000, t.miss*100/(int)t.cnt, t.miss, t.cnt);
	fflush(stdout);
}
void quit()
{
	print_fps();
	exit(0);
}

int compute(int *frame)
{
	int sum = 0;
	int i;
	/* read frame */
	for (i = 0; i < g_frame_length; i+=(CACHE_LINE_SIZE/4))
		sum += frame[i];
	return sum;
}

uint64_t get_elapsed(struct timespec *start, struct timespec *end)
{
	uint64_t dur;
	if (start->tv_nsec > end->tv_nsec)
		dur = (uint64_t)(end->tv_sec - 1 - start->tv_sec) * 1000000000 +
			(1000000000 + end->tv_nsec - start->tv_nsec);
	else
		dur = (uint64_t)(end->tv_sec - start->tv_sec) * 1000000000 +
			(end->tv_nsec - start->tv_nsec);

	return dur;
}


int main(int argc, char* argv[])
{
	int i, j;
	struct timespec start, end;
	uint64_t readsum = 0;
	int iterations = 0;
	int cpuid = 0;
	struct sched_param param;
        cpu_set_t cmask;
	int num_processors;
	int opt;
	int sum[2];
	char *ptr;
	int prio;
	int deadline = 10;
	while ((opt = getopt(argc, argv, "m:d:n:t:c:i:p:f:l:xh")) != -1) {
		switch(opt) {
		case 'm': /* set memory size */
			g_frame_length = strtol(optarg, NULL, 0);
			break;
		case 'c': /* set CPU affinity */
			cpuid = strtol(optarg, NULL, 0);
			num_processors = sysconf(_SC_NPROCESSORS_CONF);
			CPU_ZERO(&cmask);
			CPU_SET(cpuid % num_processors, &cmask);
			if (sched_setaffinity(0, num_processors, &cmask) < 0)
				perror("error");
			else
				fprintf(stderr, "assigned to cpu %d\n", cpuid);
			break;
		case 'd':
			deadline = strtol(optarg, NULL, 0);
			fprintf(stderr, "new deadline: %d ms\n", deadline);
			break;
		case 'i': /* iterations */
			iterations = strtol(optarg, NULL, 0);
			break;
		case 'p': /* set priority (nice value: -20 ~ 19) */
			prio = strtol(optarg, NULL, 0);
			if (setpriority(PRIO_PROCESS, 0, prio) < 0)
				perror("error");
			else
				fprintf(stderr, "assigned priority %d\n", prio);
			break;
		}
	}

	srand(0);

#if 0
        if(sched_setscheduler(0, SCHED_FIFO, &param) == -1) {
		perror("sched_setscheduler failed");
        }
#endif

	/* set signals to terminate once time has been reached */
	signal(SIGINT, &quit);

	/* allocate frames */
	ptr = (char *)malloc(g_frame_length * 4 * 2);
	frames[0] = (int *)ptr;
	frames[1] = (int *)(ptr + (g_frame_length * 4));

	/* the most important thing. 
	   w/o initialization, read doesn't access actual memory */
	memset(ptr, 1, g_frame_length*4*2);

	/* actual access */
	sum[0] = compute(frames[0]);
	sum[1] = compute(frames[1]);

	init_stat(&t);

	fprintf(stdout, "deadline: %d, req. b/w(MB/s): %.1f\n", deadline,
		(float)g_frame_length * 4 * 1000 / deadline / 1024 / 1024);
	while (1) {
		if (iterations > 0 && t.cnt >= iterations)
			break;
		clock_gettime(CLOCK_REALTIME, &start);
		if (sum[j%2] != compute(frames[j%2])) {
			fprintf(stderr, "mismatch !!!\n");
		}
		clock_gettime(CLOCK_REALTIME, &end);
		t.cur = get_elapsed(&start, &end);
		if (t.cur > deadline * 1000000)
			t.miss++;
		t.min = min(t.cur, t.min);
		t.max = max(t.cur, t.max);
		t.tot += t.cur;
		t.cnt ++;
		if (t.tot > 1000000000) {
			print_fps();
			init_stat(&t);
		}
	}
	quit(0);
}

