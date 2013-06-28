#!/bin/bash
. functions

outputfile=hrt.txt
TMPFILE=/run/out.txt

init_system
rmmod memguard
set_cpus "1 1 0 0"
disable_prefetcher

log_echo "llc:$llc_miss_evt arch:${archbit}bit"
log_echo "RMIN: $RMIN"

parse_bw_log()
{
    cat $1 | awk '{ print $2 }'
}

do_thr_test()
{
    ./matrix -c 1 -f bwlog.c1 &
    #./cpuhog -c 1 &
    #./thr -c 1 -t 1000000 -f bwlog.c1 &
    #./thr -c 2 -t 1000000 -f bwlog.c2 &
    #./thr -c 3 -t 1000000 -f bwlog.c3 &
    sleep 10
    killall -2 thr hrt bandwidth latency
    killall -9 cpuhog
    sleep 1
    bwsum=0
    bwsum=`expr $bwsum + $(parse_bw_log bwlog.c1)`
    #bwsum=`expr $bwsum + $(parse_bw_log bwlog.c2)`
    #bwsum=`expr $bwsum + $(parse_bw_log bwlog.c3)`
    log_echo "Aggr.B/W: $bwsum"
    # rm bwlog.*
}

do_hrt_test()
{
    local master_schedopt="$2"
    local corun_schedopt="$3"
    killall -2 thr hrt bandwidth latency
    killall -9 cpuhog
    if [ ! "$1" = "solo" ]; then
	log_echo "co-run"
	./matrix -c 1 -f bwlog.c1&
	#./cpuhog -c 1 &
	#./thr -c 1 $corun_schedopt -t 1000000 -f bwlog.c1 &
	# ./thr -c 2 $corun_schedopt -t 1000000 -f bwlog.c2 &
	# ./thr -c 3 $corun_schedopt -t 1000000 -f bwlog.c3 &
    fi
    # echo "[start]" > /sys/kernel/debug/tracing/trace_marker
    # echo "[finish]" > /sys/kernel/debug/tracing/trace_marker

    trace-cmd record -e sched:sched_switch \
	./hrt -c 0 -i 1000 -C 12 -I 10 $master_schedopt > $TMPFILE \
	|| error "exec failed"
    killall -2 thr hrt bandwidth latency cpuhog matrix
    killall -9 cpuhog
    print_settings
    sleep 1
    ./printstat.py $TMPFILE >> $outputfile

    if [ ! "$1" = "solo" ]; then
	bwsum=0
	bwsum=`expr $bwsum + $(parse_bw_log bwlog.c1)`
	#bwsum=`expr $bwsum + $(parse_bw_log bwlog.c2)`
	#bwsum=`expr $bwsum + $(parse_bw_log bwlog.c3)`
	log_echo "Aggr.B/W: $bwsum"
	rm bwlog.*
    fi
    log_echo "------------"
}

test_isolation()
{
    rmmod memguard

#    log_echo "w/o memguard"
#    do_hrt_test solo
#    do_hrt_test co-run

    log_echo ">> no memguard"
    do_hrt_test solo "-o fifo"  "-o normal"
    #do_hrt_test co-run "-o fifo"  "-o normal"
    log_echo ">> memguard(excl2)"
    do_init_mb "900 200" 1 2  >& /dev/null
    do_hrt_test solo "-o fifo"  "-o normal"
    #do_hrt_test co-run "-o fifo"  "-o normal"

    log_echo ">> memguard(excl5)"
    do_init_mb "900 200" 1 5  >& /dev/null
    do_hrt_test solo "-o fifo"  "-o normal"
    #do_hrt_test co-run "-o fifo"  "-o normal"
    rmmod memguard


#    do_hrt_test co-run "-o normal"  "-o batch"
#    do_init_share "3 1 1 1" 1 $RMIN 5  >& /dev/null
#    do_hrt_test co-run

#    do_init_share "9 1 1 1" 1 $RMIN 5  >& /dev/null
#    do_hrt_test co-run
}

test_isolation
finish
