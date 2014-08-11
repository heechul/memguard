/**
 * BwLock: memory bandwidth lock.
 *
 * Copyright (C) 2014  Heechul Yun <heechul.yun@ku.edu> 
 *
 * This file is distributed under the GPLv2 License. 
 */ 

#ifndef BWLOCK_H
#define BWLOCK_H

typedef enum {HARD, SOFT} bw_attr_t;

int bw_lock_init();
  
int bw_lock(int reserve_mb, bw_attr_t attr);

int bw_unlock(bw_attr_t *attr);

int set_attr(bw_attr_t attr);

#endif /* BWLOCK_H */
