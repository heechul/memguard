#define _GNU_SOURCE         /* See feature_test_macros(7) */
#include <unistd.h>
#include <sys/syscall.h>   /* For SYS_xxx definitions */
#include <stdio.h>
#include <stdint.h>
#include <time.h>

#define SYS_bwlock 323

#define ITER 1000000

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
	long int i;
	uint64_t tmpdiff;
	struct timespec start, end;
	
	clock_gettime(CLOCK_REALTIME, &start);	
	for (long i = 0; i < ITER; i++) {
		// if (i % 10000 == 0) fprintf(stderr, "%ld\n", i);
		syscall(SYS_bwlock, 0, 1);
		syscall(SYS_bwlock, 0, 0);
	}
	clock_gettime(CLOCK_REALTIME, &end);
	tmpdiff = get_elapsed(&start, &end);
	printf("iter=%d %.2f ns\n", ITER, (double) tmpdiff/ITER);


	printf("enter memory critical section\n");
	syscall(SYS_bwlock, 0, 1);

	clock_gettime(CLOCK_REALTIME, &start);	
repeat:

	clock_gettime(CLOCK_REALTIME, &end);
	tmpdiff = get_elapsed(&start, &end);
	if (tmpdiff < 5000000000) goto repeat;

	syscall(SYS_bwlock, getpid(), 0);
	printf("exit memory critical section\n");
}
