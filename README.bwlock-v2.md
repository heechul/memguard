Preparation
===========

## kernel 
be a root
```
   # cd /
   # wget http://ittc.ku.edu/~heechul/bwlock-kernel-bin-dist.tar.gz
   # tar zxvf bwlock-kernel-bin-dist.tar.gz
   # update-grub
```
reboot
in grub, select '3.6.0-bwlock-00011-g4a4e093' kernel

## memguard and tools
be a normal user
```
  $ cd <memguard dir>
  $ git remote update
  git checkout rtas15-bwlockv2
     (or $ git checkout -b rtas15-bwlockv2 origin/rtas15-bwlockv2 )
  $ make 
```

Usage
==========

## coarse grain locking 

If you do the following, the <pid> process will be bw locked whenever 
it is scheduled by the scheduler. 

```
  ./bwlockset <pid> <bwlock_value>

Example)
  $ ./bwlockset `pidof X` 1
  set pid=2559 val=1
```

## fine-grain locking

To support fine-grain bw locking, you need to modify the program to use 
bw_lock()/bw_unlock(). 

```
  #include "bwlock.h"
  
  bw_lock()
  ...
  bw_unlock()
```




