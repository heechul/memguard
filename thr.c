/**
 *
 * Copyright (C) 2012  Heechul Yun <heechul@illinois.edu>
 *               2012  Zheng <zpwu@uwaterloo.ca>
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <inttypes.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/time.h>
#include <sys/resource.h>

/**************************************************************************
 * Public Definitions
 **************************************************************************/
#define ADDR_2ND_RANK 0x40000000   /* the address of the 2nd Rank(1GB) of 
				      memory of 1st Mem Controller */
#define CACHE_LINE_SIZE 64	   /* cache Line size is 64 byte */

/**************************************************************************
 * Public Types
 **************************************************************************/
enum access_type { READ, WRITE, RDWR, WRST};

/**************************************************************************
 * Global Variables
 **************************************************************************/
int g_mem_size = 8192 * 1024;	   /* memory size */
int g_indx = 0;			   /* global index used for accessing the array */
int g_next = (CACHE_LINE_SIZE/4);  /* incrementing the next element in the array 
				      (Sequtial Default) */
int *g_mem_ptr = 0;		   /* pointer to allocated memory region */

FILE *g_fd = NULL;			
char *g_label = NULL;

uint64_t g_nread = 0;	           /* number of bytes read */
unsigned int g_start;		   /* starting time */

/**************************************************************************
 * Public Functions
 **************************************************************************/
unsigned int get_usecs()
{
	static struct timeval  base;
	struct timeval         time;
	gettimeofday(&time, NULL);
	if (!base.tv_usec) {
		base = time;
	}
	if (base.tv_usec > time.tv_usec)
		return ((time.tv_sec - 1 - base.tv_sec) * 1000000 +
			(1000000 + time.tv_usec - base.tv_usec));
	else
		return ((time.tv_sec - base.tv_sec) * 1000000 +
			(time.tv_usec - base.tv_usec));
}

void quit(int param)
{
	float dur_in_sec;
	float bw;
	float dur = get_usecs() - g_start;
	dur_in_sec = (float)dur / 1000000;
	printf("g_nread(bytes read) = %lld\n", g_nread);
	printf("elapsed = %.2f sec ( %.0f usec )\n", dur_in_sec, dur);
	bw = (float)g_nread / dur_in_sec / 1024 / 1024;
	printf("B/W = %.2f MB/s | ", bw);
	printf("average = %.2f ns\n", (dur*1000)/(g_nread/CACHE_LINE_SIZE));

	if (g_fd) {
		fprintf(g_fd, "%s %d\n", g_label, (int)bw);
		fclose(g_fd);
	}
	exit(0);
}

int bench_read()
{
	int i;
	int sum = 0;    

	for ( i = g_indx; i < g_mem_size/4; i+=g_next ) {
		sum += g_mem_ptr[i];
		g_nread += CACHE_LINE_SIZE ;
	}
	return sum;
}

int bench_write()
{
	int i;	
	for ( i = g_indx; i < g_mem_size/4; i+=g_next ) {
		g_nread += CACHE_LINE_SIZE ;
		g_mem_ptr[i] = i;
	}
	return 1;
}

int bench_rdwr()
{
	int i;
	int sum = 0;    

	for ( i = g_indx; i < g_mem_size/4; i+=g_next ) {
		g_mem_ptr[i] = i;
		sum += g_mem_ptr[i];
		g_nread += CACHE_LINE_SIZE;
	}
	return sum;
}

void usage(int argc, char *argv[])
{
	printf("Usage: $ %s [<option>]*\n\n", argv[0]);
	printf("-m: memory size in KB. deafult=8192\n");
	printf("-a: access type - read, write, rdwr. default=read\n");
	printf("-n: addressing pattern - Seq, Row, Bank. default=Seq\n");
	printf("-t: time to run in sec. 0 means indefinite. default=5. \n");
	printf("-c: CPU to run.\n");
	printf("-i: iterations. 0 means intefinite. default=0\n");
	printf("-p: priority\n");
	printf("-l: log label. use together with -f\n");
	printf("-f: log file name\n");
	printf("-x: memory map to /dev/mem. !!! DANGEROUS !!!\n");
	printf("-h: help\n");
	printf("\nExamples: \n$ bandwidth -m 8192 -a read -t 1 -c 2\n  <- 8MB read for 1 second on CPU 2\n");
	exit(1);
}

int main(int argc, char *argv[])
{
	uint64_t sum = 0;
	unsigned finish = 5;
	int cpuid = 0;
	int prio = 0;        
	int num_processors;
	int acc_type = READ;
	int opt;
	cpu_set_t cmask;
	int use_mmap = 0;
	int iterations = 0;
	struct sched_param param;
	int i;

	/*
	 * get command line options 
	 */
	while ((opt = getopt(argc, argv, "m:a:n:t:c:i:p:o:f:l:xh")) != -1) {
		switch (opt) {
		case 'm': /* set memory size */
			g_mem_size = 1024 * strtol(optarg, NULL, 0);
			break;
		case 'a': /* set access type */
			if (!strcmp(optarg, "read"))
				acc_type = READ;
			else if (!strcmp(optarg, "write"))
				acc_type = WRITE;
			else if (!strcmp(optarg, "rdwr"))
				acc_type = RDWR;
			else
				exit(1);
			break;
			
		case 'n': /* set access pattern */
			/* sequential */
			if( strcmp(optarg,"Seq") == 0 ) {
				g_indx = 0;
				g_next = (CACHE_LINE_SIZE/4);				
			}
			/* same bank */
			else if( strcmp(optarg,"Row") == 0 ) {
				g_indx = 0;
				g_next = (CACHE_LINE_SIZE/4) * 1024;		
			}
			/* diff bank */
			else if( strcmp(optarg,"Bank") == 0 ) {
				g_indx = cpuid*128;
				g_next = (CACHE_LINE_SIZE/4) * 1024;
			}
			else
				exit(1);
			break;
		case 't': /* set time in secs to run */
			finish = strtol(optarg, NULL, 0);
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
		case 'o': /* SCHED_BATCH */
			if (!strcmp(optarg, "batch")) {
				param.sched_priority = 0; 
				if(sched_setscheduler(0, SCHED_BATCH, &param) == -1) {
					perror("sched_setscheduler failed");
					exit(1);
				}
			} else if (!strcmp(optarg, "fifo")) {
				param.sched_priority = 1; 
				if(sched_setscheduler(0, SCHED_FIFO, &param) == -1) {
					perror("sched_setscheduler failed");
					exit(1);
				}
			}
			break;
		case 'p': /* set priority */
			prio = strtol(optarg, NULL, 0);
			if (setpriority(PRIO_PROCESS, 0, prio) < 0)
				perror("error");
			else
				fprintf(stderr, "assigned priority %d\n", prio);
			break;
		case 'i': /* iterations */
			iterations = strtol(optarg, NULL, 0);
			break;
		case 'l': /* set label */
			g_label = strdup(optarg);
			break;
			
		case 'f': /* set file descriptor */
			g_fd = fopen(optarg, "a+");
			if (g_fd == NULL) 
				perror("error");
			break;
		case 'x': /* mapping to /dev/mem. !! DANGEROUS !! */
			use_mmap = 1;
			break;
		case 'h': 
			usage(argc, argv);
			break;
		}
	}

	/*
	 * allocate contiguous region of memory 
	 */ 
	if (use_mmap) {
		/* open /dev/mem for accessing memory in physical addr. */
		int fd = -1;
		unsigned long offset;

		printf("Use mmap\n");
		fd = open("/dev/mem", O_RDWR | O_SYNC);
		if(fd == -1) {
			fprintf(stderr, "ERROR Opening /dev/mem\n");	
			exit(1);
		}
		/* offset variable is used to allocate each cpu to a different offset 
		   from each other */
		offset = ADDR_2ND_RANK + cpuid*g_mem_size;
		
		/* use mmap to allocate each cpu to the specific address in memory */
		g_mem_ptr = (int *)mmap(NULL, g_mem_size, PROT_READ|PROT_WRITE, 
					MAP_SHARED, fd, offset);
		if(g_mem_ptr == NULL) {
			fprintf(stderr, "could not allocate memarea");
			exit(1);
		}
	} else {
		printf("Use standard malloc\n");
		g_mem_ptr = (int *)malloc(g_mem_size);
	}
	memset(g_mem_ptr, 1, g_mem_size);

	/* print experiment info before starting */
	printf("memsize=%d KB, type=%s, cpuid=%d\n",
	       g_mem_size/1024,
	       ((acc_type==READ) ?"read":
		(acc_type==WRITE)? "write" :
		(acc_type==RDWR) ? "rdwr" : "worst"),
		cpuid);
	printf("stop at %d\n", finish);

	/* set signals to terminate once time has been reached */
	signal(SIGINT, &quit);
	if (finish > 0) {
		signal(SIGALRM, &quit);
		alarm(finish);
	}

	/*
	 * actual memory access
	 */
	g_start = get_usecs();
	for (i=0;; i++) {
		switch (acc_type) {
		case READ:
			sum = bench_read();
			break;
		case WRITE:
			sum = bench_write();
			break;
		case RDWR:
			sum = bench_rdwr();
			break;
		}

		if (iterations > 0 && i >= iterations)
			break;
	}
	quit(0);
}

