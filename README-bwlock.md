memguard: add BwLock support

BwLock is a user level bandwidth control mechanism to protect memory
performance critical section

APIs
====

    int bw_lock(int reserve_mb, int attr);
        reserve 'reserve_mb' MB/s for the core that call this function.
        The other cores's bandwidth will be limited to 100MB/s

    int bw_unlock(int *attr);
        remove b/w reservation of all cores.

Usage
=====

Modify source code as follows.

    #include "bwlock.h"

    bw_lock(10000, SOFT);

    ... memory performance critical section ...

    bw_unlock(NULL);

Compile your code with 'bwlock.c'. In the following, we will use the
included test program 'bwlocktest.c'

    $ make bwlocktest
    cc -std=gnu99 -O2 -g bwlock.c bwlocktest.c -o bwlocktest -lrt

Run the test program.

    $ ./bwlocktest
    USE_BWLOCK=0            // this mean BWLOCK is not being used
    iter=100000 74.69 ns

At this point, we are not using BwLock. To use the BwLock, you need
to set the environment variable USE_BWLOCK=1

    $ export USE_BWLOCK=1
    $ ./bwlocktest
    USE_BWLOCK=1
    N_PROC: 4, CoreId: 3
    memguard is not loaded
    N_PROC: 4, CoreId: 3
    bwlocktest: bwlock.c:109: bw_lock_init: Assertion `fd_control == 0' failed.
    Aborted (core dumped)

This is because you did not load memguard kernel module. BwLock requires the
module is loaded ahead of the time. Also, you need to be a root.

    $ sudo bash
    # insmod ./memguard.ko
    # USE_BWLOCK=1 ./bwlocktest
    USE_BWLOCK=1
    N_PROC: 4, CoreId: 3
    iter=100000 2313.68 ns

Now it is working correctly.


Additionally, you can print the legnth of each bwlock region with USE_TIMING 
environment variable. See the following.

    # USE_TIMING=1 USE_BWLOCK=1 ./bwlocktest
    USE_BWLOCK=1
    N_PROC: 4, CoreId: 1
    65
    1
    1
    ...
    1
    1
    iter=100000 9161.52 ns
