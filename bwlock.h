/**
 * BwLock: memory bandwidth lock.
 *
 * Copyright (C) 2014  Heechul Yun <heechul.yun@ku.edu> 
 *
 * This file is distributed under the GPLv2 License. 
 */ 

#ifndef BWLOCK_H
#define BWLOCK_H
// #define _GNU_SOURCE         /* See feature_test_macros(7) */
#include <unistd.h>
#include <sys/syscall.h>   /* For SYS_xxx definitions */

#define SYS_bwlock 323

#define bw_lock()   syscall(SYS_bwlock, 0, 1)
#define bw_unlock() syscall(SYS_bwlock, 0, 0)

#endif /* BWLOCK_H */
