# use this to detect safe read/write budgets for the cores
CPUN=$(expr $(nproc) - 1)
echo "max cpuid = $CPUN"
for acctype in read write; do
    for c in $(seq 1 ${CPUN}); do
	bandwidth -c $c -a $acctype -t 1000 >& /dev/null &
    done
    bandwidth -c 0 -a $acctype -t 3 >& out-$acctype.txt
    killall -9 bandwidth
done
sleep 1

echo -n "Read (MB/s): "
grep "B/W" out-read.txt | awk '{ print $4 }'
echo -n "Write (MB/s): "
grep "B/W" out-write.txt | awk '{ print $4 }'
