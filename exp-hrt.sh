#!/bin/bash
. ./functions

outputfile=hrt.txt

init_system
rmmod memguard

log_echo "llc:$llc_miss_evt arch:${archbit}bit"
log_echo "RMIN: $RMIN"

parse_bw_log()
{
    cat $1 | awk '{ print $2 }'
}

do_hrt_test()
{
    local master_schedopt="$2"
    local corun_schedopt="$3"
    local FILENAME=$4
    local TMPFILE=/run/$FILENAME

    killall -2 thr hrt bandwidth latency
    killall -9 cpuhog
    if [ "$1" = "cpuhog" ]; then
	log_echo "cpuhog"
	./cpuhog -c 1 $corun_schedopt &
    elif [ "$1" = "thr" ]; then
	log_echo "co-run w/ 'thr'"
	./thr -c 1 $corun_schedopt -t 1000000 -f bwlog.c1 &
    elif [ "$1" = "xorg" ]; then
	log_echo "shoud print to screen"
    fi
    # echo "[start]" > /sys/kernel/debug/tracing/trace_marker
    # echo "[finish]" > /sys/kernel/debug/tracing/trace_marker

    echo $$ > /sys/fs/cgroup/core0/tasks
    # trace-cmd record -e sched:sched_switch \
    ./hrt -c 0 -i 1000 -C 12 -I 20 $master_schedopt > $TMPFILE \
	|| error "exec failed"
    # echo $$ > /sys/fs/cgroup/system/tasks

    killall -2 thr hrt bandwidth latency cpuhog matrix
    killall -9 cpuhog
    print_settings
    sleep 1
    cp -v $TMPFILE $FILENAME-`date +%F-%H-%M`.dat
    awk '{ print $2 }' $TMPFILE > $TMPFILE.dat
    ./printstat.py --deadline=13 $TMPFILE.dat >> $outputfile
    cp $TMPFILE rawdata.dat
    gnuplot histo.scr > $FILENAME-`date +%F-%H-%M`.eps
    
    if [ "$1" = "thr" ]; then
	bwsum=0
	bwsum=`expr $bwsum + $(parse_bw_log bwlog.c1)`
	log_echo "Aggr.B/W: $bwsum"
	rm bwlog.*
    fi
    log_echo "------------"
}

test_isolation()
{
    rmmod memguard

    hw=$1

    log_echo "============================="
    date >> $outputfile
    uname -a >> $outputfile
    log_echo "============================="

    log_echo "^^ no memguard"
    # nmon -F $hw-$2-$3-out-org-solo-nmon.dat -s1 -c 6
    # do_hrt_test xorg "-o fifo" "-o normal" $hw-$2_$3-out-org-solo >& /dev/null
    nmon -F $hw-$2-$3-out-org-corun-nmon.dat -s1 -c 6
    do_hrt_test xorg "-o fifo" "-o normal" $hw-$2_$3-out-org-corun

    log_echo "^^ memguard(excl0)"
    do_init_mb "$2 $3" 0 0  >& /dev/null
    # nmon -F $hw-$2-$3-out-excl0-solo-nmon.dat -s1 -c 6
    # do_hrt_test xorg "-o fifo" "-o normal" $hw-$2_$3-out-excl0-solo >& /dev/null
    nmon -F $hw-$2-$3-out-excl0-corun-nmon.dat -s1 -c 6
    do_hrt_test xorg "-o fifo" "-o normal" $hw-$2_$3-out-excl0-corun


    log_echo "^^ memguard(excl5)"
    do_init_mb "$2 $3" 0 5  >& /dev/null
    # nmon -F $hw-$2-$3-out-excl5-solo-nmon.dat -s1 -c 6
    # do_hrt_test xorg "-o fifo" "-o normal" $hw-$2_$3-out-excl5-solo >& /dev/null
    nmon -F $hw-$2-$3-out-excl5-corun-nmon.dat -s1 -c 6
    do_hrt_test xorg "-o fifo" "-o normal" $hw-$2_$3-out-excl5-corun

    rmmod memguard
}

set_cpus "1 1 1 1"

# case 4: shared, no prefetcher
set_cpus "1 1 0 0"
disable_prefetcher
init_cgroup
# test_isolation "xeon" 600 600
# test_isolation "xeon" 1000 1000
# test_isolation "xeon" 1000 2000
# test_isolation "xeon" 1000 3000
test_isolation "xeon" 1000 4000
#test_isolation "xeon" 900 300
finish
rmmod memguard
