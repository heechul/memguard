#define _GNU_SOURCE         /* See feature_test_macros(7) */
#include <unistd.h>
#include <sys/syscall.h>   /* For SYS_xxx definitions */
#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <stdlib.h>

#define SYS_bwlock 323

#define ITER 1000000


void usage(char *argv[])
{
	printf("%s <pid> <bwlock_val>\n", argv[0]);
	exit(1);
}

int main(int argc, char *argv[])
{
	int pid, val;

	if (argc != 3)
		usage(argv);

	pid = strtol(argv[1], NULL, 0);
	val = strtol(argv[2], NULL, 0);

	printf("set pid=%d val=%d\n", pid, val);
	syscall(SYS_bwlock, pid, val);
}
