#!/bin/bash
# Measure memguard overhead by comparing baseline bandwidth with varying guard periods.
# Requires
# - the bandwidth benchmark from Isolbench (https://github.com/CSL-KU/IsolBench)
# - hugepage support. If no hugepages are available, remove '-x' option when executing
# 	the bandwidth benchmark. Using hugepage is recommended for determinism and
# 	repeatability.
# Usage: sudo ./overhead-basic.sh

# check if root
if [ "$(id -u)" != "0" ]; then
	echo "This script must be run as root" 1>&2
	exit 1
fi
base_bw=`bandwidth -c 0 -p -20 -t 10 -x 2>/dev/null | grep "B/W" | awk '{ print $4 }'`
echo "none" 1.0 $base_bw

# Test overhead of memguard with different periods
for p in 10 20 50 100 200 500 1000 10000; do
	insmod memguard-basic.ko g_period_us=$p g_read_budget_mb=900000 g_write_budget_mb=900000
	bw=`bandwidth -c 0 -p -20  -t 10 -x 2> /dev/null | grep "B/W" | awk '{ print $4 }'`
	slowdown=`echo "scale=2; $base_bw / $bw" | bc`
	echo $p $slowdown $bw
	rmmod memguard-basic
done
