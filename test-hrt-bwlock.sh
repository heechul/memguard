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
    bws="$1"
    echo mb $bws > $MGDIR/limit
    echo exclusive 2 > $MGDIR/control
}

do_init_mg-br-ss()
{
    echo "mg-br-ss"
    bws="$1"
    echo mb $bws > $MGDIR/limit
    echo exclusive 2 > $MGDIR/control
    echo reclaim 1 > $MGDIR/control
}

do_load()
{
    cores="$1"
# co-runner
    for c in $cores; do
	./bandwidth -c $c -t 100000 -a write > corun.c$c.txt &
    done
}

do_load_cgroup()
{
    cores="$1"
# co-runner
    for c in $cores; do
	echo $$ > /sys/fs/cgroup/core$c/tasks
	./bandwidth -c $c -t 100000 -a write > corun.c$c.txt &
    done
}

# test

print_palloc_setting()
{
    echo "> PALLOC settings:"
    for c in 0 1 2 3; do 
	echo -n "Core${c}: "
	cat /sys/fs/cgroup/core$c/palloc.bins
    done
    echo -n "use_palloc: "
    cat /sys/kernel/debug/palloc/use_palloc
    echo -n "debug_level: "
    cat /sys/kernel/debug/palloc/debug_level
    echo -n "alloc_ballance: "
    cat /sys/kernel/debug/palloc/alloc_balance # <- has a bug in ARM
    echo -n "stats:"
    cat /sys/kernel/debug/palloc/control
}

print_memguard_setting()
{
    echo "> MemGuard settings:"
    cat /sys/kernel/debug/memguard/limit
    cat /sys/kernel/debug/memguard/control
    cat /sys/kernel/debug/memguard/bwlockcnt
}


do_test_bwlock()
{
    do_load "1 2 3"
    echo "" > /sys/kernel/debug/tracing/trace
    time taskset -c 0 ./hrt-bwlock -i 400 -I 10 -b 2> /dev/null 1> /dev/null 
    cat /sys/kernel/debug/tracing/trace > hrt-bwlock.trace
    killall -2 bandwidth hrt-bwlock
}

do_test_mg()
{
    do_load "1 2 3"
    echo "" > /sys/kernel/debug/tracing/trace
    time taskset -c 0 ./hrt-bwlock -i 400 -I 10 2> /dev/null 1> /dev/null 
    cat /sys/kernel/debug/tracing/trace > hrt-bwlock.trace
    killall -2 bandwidth
}

do_test_bwlock_2()
{
    do_load "2 3"
    echo "" > /sys/kernel/debug/tracing/trace
    taskset -c 1 ./hrt-bwlock -i 100000 -m 6144 -I 10 -b >& /dev/null &
    time taskset -c 0 ./hrt-bwlock -i 400 -I 10 -b 2> /dev/null 1> /dev/null 
    cat /sys/kernel/debug/tracing/trace > hrt-bwlock.trace
    killall -2 bandwidth hrt-bwlock
}

do_test_mg_2()
{
    do_load "2 3"
    echo "" > /sys/kernel/debug/tracing/trace
    taskset -c 1 ./hrt-bwlock -i 100000 -m 6144 -I 10 >& /dev/null &
    time taskset -c 0 ./hrt-bwlock -i 400 -I 10 2> /dev/null 1> /dev/null 
    cat /sys/kernel/debug/tracing/trace > hrt-bwlock.trace
    killall -2 bandwidth hrt-bwlock
}


do_profile()
{
    echo "" > /sys/kernel/debug/tracing/trace
    sleep 10
    cat /sys/kernel/debug/tracing/trace > hrt-bwlock.trace
}

plot()
{
    # file msut be xxx.dat form
    start=$1
    finish=$2
    file="hrt-bwlock_${start}-${finish}"
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

plot_core()
{
    # file msut be xxx.dat form
    core=$1
    start=$2
    finish=$3
    file="hrt-bwlock_C${core}-${start}-${finish}"
    cat > ${file}.scr <<EOF
set terminal postscript eps enhanced color "Times-Roman" 22
set yrange [0:100000]
set xrange [$start:$finish]
plot 'hrt-bwlock.core0.dat' ti "core0" w lp
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
    plot 0 500
    plot 0 1500
    plot 0 4000
    plot 1000 1100
    plot 1000 1500
    plot 1000 2000
    plot_core 0 0 1500
    plot_core 1 0 1500
}

copy_data()
{
    echo "Copying produced data"
    dirname=$1
    mkdir -p $dirname || echo "WARN: overwrite"
    cp setting.txt hrt-bwlock*.pdf hrt-bwlock*.dat $dirname
    chown heechul.heechul $dirname
    grep "B/W" corun.c*.txt > $dirname/corun.txt
    cp -r $dirname ~/Dropbox/tmp

    echo "Enter to continue:"
    read
}

echo 16384 > /sys/kernel/debug/tracing/buffer_size_kb
tag=`date +"%m%d%y"`

insmod ./memguard.ko
do_test_bwlock
print_memguard_setting > setting.txt
# print_palloc_setting >> setting.txt
rmmod memguard


# insmod ./memguard.ko

# # do_load "2 3"
# do_profile
# killall -2 bandwidth
# # do_test_mg_3
# rmmod memguard

# insmod ./memguard.ko
# do_init_mg-ss
# do_test_mg
# rmmod memguard

do_graph
copy_data hrt-bwlock-$tag
