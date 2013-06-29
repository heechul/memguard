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

long long int count;

void quit(int param)
{
	printf("count = %lld\n", count);
	exit(1);
}

int main(int argc, char *argv[])
{
	int cpuid = 0;
	int prio = 0;        
	int num_processors;
	cpu_set_t cmask;
	int opt;
	struct sched_param param;

	signal(SIGINT, &quit);

	/*
	 * get command line options 
	 */
	while ((opt = getopt(argc, argv, "a:n:t:c:i:p:o:f:l:xh")) != -1) {
		switch (opt) {
		case 'c': /* set CPU affinity */
			cpuid = strtol(optarg, NULL, 0);
			num_processors = sysconf(_SC_NPROCESSORS_CONF);
			CPU_ZERO(&cmask);
			CPU_SET(cpuid % num_processors, &cmask);
			if (sched_setaffinity(0, num_processors, &cmask) < 0) {
				perror("error");
				exit(1);
			}
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
		}
	}

	count = 0; 
	while(1)
		count ++;

}
