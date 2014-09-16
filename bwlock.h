/**
 * BwLock: memory bandwidth lock.
 *
 * Copyright (C) 2014  Heechul Yun <heechul.yun@ku.edu> 
 *
 * This file is distributed under the GPLv2 License. 
 */ 

#ifndef BWLOCK_H
#define BWLOCK_H

#define HARD 0
#define SOFT 1 

int bw_lock_init(void);
  
int bw_lock(int reserve_mb, int attr);

int bw_unlock(int *attr);

int set_attr(int attr);

#endif /* BWLOCK_H */
