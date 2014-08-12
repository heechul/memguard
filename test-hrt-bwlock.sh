#!/bin/bash

MGDIR=/sys/kernel/debug/memguard

error()
{
    echo "ERR: $*"
    exit 1
}

# init
do_init_mg-ss()
{
    echo "mg-ss"
    echo mb 900 100 100 100 > $MGDIR/limit
    echo exclusive 5 > $MGDIR/control
}

do_init_mg-br-ss()
{
    echo "mg-br-ss"
    echo mb 900 100 100 100 > $MGDIR/limit
    echo exclusive 5 > $MGDIR/control
    echo reclaim 1 > $MGDIR/control
}

do_load()
{
# co-runner
    for c in 1 2 3; do
	./bandwidth -c $c -t 100000 -a read &
    done
}

# test
do_test_mg()
{
    do_load
    echo "" > /sys/kernel/debug/tracing/trace
    time taskset -c 0 ./hrt-bwlock -i 400 -I 10 2> /dev/null 1> /dev/null 
    cat /sys/kernel/debug/tracing/trace > hrt-bwlock.trace
    killall -2 bandwidth
}

do_test_bw_lock()
{
    do_load
    echo "" > /sys/kernel/debug/tracing/trace
    time taskset -c 0 ./hrt-bwlock -i 400 -I 10 -b 2> /dev/null 1> /dev/null 
    cat /sys/kernel/debug/tracing/trace > hrt-bwlock.trace
    killall -2 bandwidth
}

plot()
{
    # file msut be xxx.dat form
    start=$1
    finish=$2
    file="hrt-bw_lock_${start}-${finish}"
    cat > ${file}.scr <<EOF
set terminal postscript eps enhanced color "Times-Roman" 22
set yrange [0:100000]
set xrange [$start:$finish]
plot 'hrt-bwlock.core0.dat' ti "core0" w lp, \
     'hrt-bwlock.core1.dat' ti "core1" w lp, \
     'hrt-bwlock.core2.dat' ti "core2" w lp, \
     'hrt-bwlock.core2.dat' ti "core3" w lp
EOF
    gnuplot ${file}.scr > ${file}.eps
    epspdf  ${file}.eps
}

do_graph()
{
    echo "plotting graphs"
    for core in 0 1 2 3; do
	[ ! -f hrt-bwlock.trace ] && error "hrt-bwlock.trace doesn't exist"
	cat hrt-bwlock.trace | grep "$core\]" > hrt-bwlock.trace.core$core
	grep update_statistics hrt-bwlock.trace.core$core | awk '{ print $7 }' | \
	    grep -v throttled_error > hrt-bwlock.core$core.dat
    done
    plot 5000 5100
    plot 0 10000
}

# insmod ./memguard.ko
# do_test_bw_lock
# rmmod memguard

# insmod ./memguard.ko
# do_init_mg-ss
# do_test_mg
# rmmod memguard

echo 16384 > /sys/kernel/debug/tracing/buffer_size_kb
insmod ./memguard.ko
do_init_mg-br-ss
do_test_mg
rmmod memguard

do_graph
cp hrt-bw*.pdf ~/Dropbox/tmp
