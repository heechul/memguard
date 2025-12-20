#!/bin/bash
. functions

#test_2core_twobench "470.lbm" "000.cpuhog" # (1) solor reference

#benchb=$allspec2006sorted #subject

outputfile=reclaimshare.txt

init_system
rmmod memguard
set_cpus "1 1 1 1"
disable_prefetcher >> /dev/null

echo "enable prefetcher" >> $outputfile
echo "llc:$llc_miss_evt arch:${archbit}bit" >> $outputfile
echo "RMIN: $RMIN"

set_cpu_interval 2
set_cpus "1 0 1 0"

SHARES="5 1"

test_isolation()
{
    rmmod memguard

    foreground=$allspec2006sorted
    background=470.lbm

    log_echo "w/o memguard, solo"
    for f in $foreground; do
    	do_exp_ncore $f
    done

    log_echo "w/o memguard, co-run"
    for f in $foreground; do
    	do_exp_ncore $f $background
    done

    log_echo "memguard-RO(reserve only), co-run"
    for f in $foreground; do
        do_init_share "$SHARES" 0 $RMIN 0  >& /dev/null
    	do_exp_ncore $f $background
    done

    log_echo "memguard-BR(reclaim), co-run"
    for f in $foreground; do
        do_init_share "$SHARES" 1 $RMIN 0  >& /dev/null
    	do_exp_ncore $f $background
    done

    log_echo "memguard-BR+SS(rtas13), co-run"
    for f in $foreground; do
        do_init_share "$SHARES" 1 $RMIN 2  >& /dev/null
	do_exp_ncore $f $background
    done

    log_echo "memguard-BR+PS(journal), co-run"
    for f in $foreground; do
        do_init_share "$SHARES" 1 $RMIN 5  >& /dev/null
	do_exp_ncore $f $background
    done
}

test_isolation
finish
