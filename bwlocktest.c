#include <stdio.h>
#include <stdint.h>
#include <time.h>

#include "bwlock.h"

#define ITER 100000

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

int main()
{
	int attr;
	uint64_t tmpdiff;
	struct timespec start, end;
	
	//  bw_lock_init();

	clock_gettime(CLOCK_REALTIME, &start);	
	for (long i = 0; i < ITER; i++) {
		// if (i % 10000 == 0) fprintf(stderr, "%ld\n", i);
		bw_lock(1000, SOFT);
		bw_unlock(&attr);
	}
	clock_gettime(CLOCK_REALTIME, &end);
	tmpdiff = get_elapsed(&start, &end);
	printf("iter=%d %.2f ns\n", ITER, (double) tmpdiff/ITER);
}
