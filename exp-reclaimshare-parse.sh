#
# Parse the outputs of exp-reclaimshare.sh
#
# 2013 (c) Heechul Yun (heechul@illinois.edu)
#
# assume 29 benchmarks
while read buf; do
    if [[ $buf = *memguard* ]]; then
	echo $buf # title
	# read benchmarks results
	for i in `seq 1 29`; do
	    # process benchmark names
	    read buf
	    if [[ $buf = *ccommit* ]]; then
		read buf # skip loading string
	    fi
	    bench_fg=`echo $buf | awk '{ print $1 }'`
	    bench_bg=`echo $buf | awk '{ print $2 }'`
	    read buf
	    fg="`echo $buf | awk '{ print $2 }'`"
	    bg="`echo $buf | awk '{ print $4 }'`"
	    echo $bench_bg $bg $bench_fg $fg 
	done
    fi
done
