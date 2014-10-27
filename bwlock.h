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
#include <sys/time.h>
#include <stdio.h>

#define SYS_bwlock 323

#define USE_BWLOCK 1

#if USE_BWLOCK
#  define bw_lock()    syscall(SYS_bwlock, 0, 1)
#  define bw_unlock()  syscall(SYS_bwlock, 0, 0)
#else
#  define bw_lock()    
#  define bw_unlock()  
#endif

#endif /* BWLOCK_H */
