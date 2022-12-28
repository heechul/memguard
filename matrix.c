#define _GNU_SOURCE             /* See feature_test_macros(7) */
#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <sched.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/resource.h>

/* change dimension size as needed */
struct timeval tv; 
int dimension = 1024;
double start, end; /* time */

double timestamp()
{
        double t;
        gettimeofday(&tv, NULL);
        t = tv.tv_sec + (tv.tv_usec/1000000.0);
        return t;
}

// a naive matrix multiplication implementation. 
void matmult(double *A, double *B, double *C, int dimension)
{
	for(int i = 0; i < dimension; i++) {
		for(int j = 0; j < dimension; j++) {
			for(int k = 0; k < dimension; k++) {
				C[dimension*i+j] += A[dimension*i+k] * B[dimension*k+j];
			}
		}
	}	
}

int main(int argc, char *argv[])
{
	double *A, *B, *C;
	unsigned finish = 0;
	int i, j, k;

	int opt;
	int cpuid = 0;
	int prio = 0;        
	int num_processors;
	struct sched_param param;

	/*
	 * get command line options 
	 */
	while ((opt = getopt(argc, argv, "m:a:n:t:c:i:p:o:f:l:xh")) != -1) {
		switch (opt) {
		case 'n':
			dimension = strtol(optarg, NULL, 0);
			break;
		}
	}

	printf("dimension: %d\n", dimension);

	A = (double*)malloc(dimension*dimension*sizeof(double));
	B = (double*)malloc(dimension*dimension*sizeof(double));
	C = (double*)malloc(dimension*dimension*sizeof(double));

	srand(292);

	// matrix initialization
	for(i = 0; i < dimension; i++) {
		for(j = 0; j < dimension; j++)
		{   
			A[dimension*i+j] = (rand()/(RAND_MAX + 1.0));
			B[dimension*i+j] = (rand()/(RAND_MAX + 1.0));
			C[dimension*i+j] = 0.0;
		}
	}

	// do matrix multiplication
	start = timestamp();
	matmult(A, B, C, dimension);

	end = timestamp();
	printf("secs:%f\n", end-start);

	free(A);
	free(B);
	free(C);

	return 0;
}
