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
/* #define FRAME_LENGTH (1920*1080) - HD */

#define FRAME_WIDTH 3840
#define FRAME_HEIGHT 2160
#define FRAME_LENGTH (FRAME_WIDTH * FRAME_HEIGHT)
#define FILTER_WIDTH 3
#define FILTER_HEIGHT 3

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

typedef struct {
	uint8_t r;
	uint8_t g;
	uint8_t b;
} color_rgb_t;

/**************************************************************************
 * Global Variables
 **************************************************************************/

stat_t t;

int frame_width = 3840;
int frame_height = 2160;
color_rgb_t frames[2][FRAME_WIDTH][FRAME_HEIGHT];
color_rgb_t outputs[2][FRAME_WIDTH][FRAME_HEIGHT];

float filter[FILTER_WIDTH][FILTER_HEIGHT] =  
{ 
  0.0, 0.2, 0.0,
  0.2, 0.2, 0.2,
  0.0, 0.2, 0.0
}; 

float factor = 1.0; 
float bias = 0.0; 

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
	fprintf(stdout, "fps: %.1f %d MB/s avg/min/max: %ld/%ld/%ld(us) miss:%d%%(%d/%ld)\n", fps, (int)(fps*frame_width * frame_height*sizeof(color_rgb_t)/1024/1024),
	       avgtime/1000, t.min/1000, t.max/1000, t.miss*100/(int)t.cnt, t.miss, t.cnt);
	fflush(stdout);
}
void quit(int ret)
{
	print_fps();
	exit(0);
}

int load_frames()
{
    int w = frame_width;
    int h = frame_height;
    for(int fno = 0; fno < 2; fno++) {
	    for(int x = 0; x < w; x++) {
		    for(int y = 0; y < h; y++) 
		    { 
			    frames[fno][x][y].r =
				    frames[fno][x][y].g =
				    frames[fno][x][y].b = 0x11;
		    }
	    }
    }
}

int compute_frame(int fno)
{
    int w = frame_width;
    int h = frame_height;
    int sum = 0;
    for(int x = 0; x < w; x++) {
	for(int y = 0; y < h; y++) 
	{ 
		sum += frames[fno][x][y].r +
			frames[fno][x][y].g +
			frames[fno][x][y].b;
	}
    }
    outputs[0][0][0].r = sum;
    outputs[1][0][0].r = sum;
}

int alpha_frame(int fno, float alpha, float beta)
{
    int w = frame_width;
    int h = frame_height;

    for(int x = 0; x < w; x++) {
	for(int y = 0; y < h; y++) 
	{ 
	    outputs[fno][x][y].r = frames[fno][x][y].r * alpha + beta;
	    outputs[fno][x][y].g = frames[fno][x][y].g * alpha + beta;
	    outputs[fno][x][y].b = frames[fno][x][y].b * alpha + beta;
	}
    }
    return 0;
}

int filter_frame(int fno)
{
    int w = frame_width;
    int h = frame_height;

    for(int x = 0; x < w; x++) {
	for(int y = 0; y < h; y++) 
	{ 
	    float red = 0.0, green = 0.0, blue = 0.0; 

	    //multiply every value of the filter with corresponding image pixel 
	    for(int filterX = 0; filterX < FILTER_WIDTH; filterX++) 
		for(int filterY = 0; filterY < FILTER_HEIGHT; filterY++) 
		{ 
		    int imageX = (x - FILTER_WIDTH / 2 + filterX + w) % w; 
		    int imageY = (y - FILTER_HEIGHT / 2 + filterY + h) % h; 
		    red += frames[fno][imageX][imageY].r * filter[filterX][filterY]; 
		    green += frames[fno][imageX][imageY].g * filter[filterX][filterY]; 
		    blue += frames[fno][imageX][imageY].b * filter[filterX][filterY]; 
		} 
	    //truncate values smaller than zero and larger than 255 
	    outputs[fno][x][y].r = min(max(int(factor * red + bias), 0), 255); 
	    outputs[fno][x][y].g = min(max(int(factor * green + bias), 0), 255); 
	    outputs[fno][x][y].b = min(max(int(factor * blue + bias), 0), 255);
	}
    }
    return 0;
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
	int verbose = 0;
	struct sched_param param;
	int interval_ms = 0;
        cpu_set_t cmask;
	int num_processors;
	int opt;
	int sum[2];
	char *ptr;
	int prio;
	int deadline = 10;
	enum {BLUR, ALPHA, SUM} filtermode;

	while ((opt = getopt(argc, argv, "m:d:n:t:c:i:I:p:f:l:xhv")) != -1) {
		switch(opt) {
		case 'm': /* image quality: sd, hd, uhd */
			if (!strcmp("sd", optarg)) {
				frame_width = 720; frame_height = 480;
			} else if (!strcmp("hd", optarg)) {
				frame_width = 1920; frame_height = 1080;
			} else if (!strcmp("uhd", optarg)) {
				frame_width = 3840; frame_height = 2160;
			}
			break;
		case 'f': /* filter types: blur alpha sum */
			if (!strcmp("blur", optarg)) {
				filtermode = BLUR;
			} else if (!strcmp("alpha", optarg)) {
				filtermode = ALPHA;
			} else if (!strcmp("sum", optarg)) {
				filtermode = SUM;
			}
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
		case 'I': /* interval */
			interval_ms = strtol(optarg, NULL, 0);
			fprintf(stderr, "I(interval)=%d(ms)\n", interval_ms);
			break;
		case 'v': /* verbose */
			fprintf(stderr, "Verbose=on");
			verbose = 1;
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

	/* the most important thing. 
	   w/o initialization, read doesn't access actual memory */
	load_frames();
	
	/* actual access */
	init_stat(&t);

	fprintf(stderr, "deadline: %d, req. b/w(MB/s): %.1f\n", deadline,
		(float)frame_width * frame_height * sizeof(color_rgb_t) 
		* 1000 / deadline / 1024 / 1024);
	j = 0;
	while (1) {
		j++;
		if (iterations > 0 && t.cnt >= iterations)
			break;
		clock_gettime(CLOCK_REALTIME, &start);

		switch (filtermode) {
		case BLUR:
			filter_frame(j%2);
			break;
		case ALPHA:
			alpha_frame(j%2, 2.2, 50);
			break;
		default:
			compute_frame(j%2);
			break;
		}

		clock_gettime(CLOCK_REALTIME, &end);
		t.cur = get_elapsed(&start, &end);
		if (t.cur > deadline * 1000000)
			t.miss++;
		t.min = min(t.cur, t.min);
		t.max = max(t.cur, t.max);
		t.tot += t.cur;
		t.cnt ++;
		if (verbose && j >= 0) {
		    // printf("%4d %lld\n", j, t.cur);
		    fprintf(stdout, "%4d %ld\n", j, t.cur);
		}
		if (!verbose && t.tot > 1000000000) {
			print_fps();
			init_stat(&t);
		}
		if (interval_ms > 0)
		    usleep(interval_ms*1000);
	}
	quit(0);
}

