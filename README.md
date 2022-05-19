# MemGuard

MemGuard is a memory bandwidth reservation system for multi-core platforms. 

## ChangeLog

- May 2022 
  - read/write separate reservation (from [RTAS'19](https://www.ittc.ku.edu/~heechul/papers/cachedos-rtas2019-camera.pdf))
  - bandwidth reclaiming (re-enabled. originally from [RTAS'13](https://www.ittc.ku.edu/~heechul/papers/memguard-rtas13.pdf))
 
## Install

	# make
	# insmod memguard.ko
	    <-- Load the module by doing

## Usage
Once the module is loaded, the thresholds can be set as follows:

	- per-core LLC miss threshold assignment.

	assign 500 MB/s for Cores 0,1,2,3
	# echo mb 500 500 500 500 > /sys/kernel/debug/memguard/read_limit

	- per-core LLC writeback threshold assignment.

	assign 100 MB/s for Cores 0,1,2,3
	# echo mb 100 100 100 100 > /sys/kernel/debug/memguard/write_limit

	- reclaim control (of reserved bandwidth)

	enable
	# echo reclaim 1 > /sys/kernel/debug/memguard/control

	disable
	# echo reclaim 0 > /sys/kernel/debug/memguard/control

	- exclusive mode control (of best-effort bandwidth)

	strict reservation. (disable best-effort sharing. only use guaranteed bw)
	# echo exclusive 0 > /sys/kernel/debug/memguard/control

	spare b/w sharing mode (see RTAS13)
	# echo exclusive 2 > /sys/kernel/debug/memguard/control

	proportional share mode (see TC'15)
	# echo exclusive 5 > /sys/kernel/debug/memguard/control
