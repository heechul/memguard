# MemGuard-Basic

This is a stripped down, basic version of MemGuard that supports only the basic per-core memory bandwidth throttling capability. More specifically, it doesn't support writeback throttling, bandwidth reclaiming/sharing capability of the original MemGuard. 


## Install

	- build
	# make basic 

	- load the module
	# insmod memguard-basic.ko

## Usage
Once the module is loaded, the thresholds can be set as follows:

	- per-core LLC miss threshold assignment.

	assign 500 MB/s for Cores 0,1,2,3
	# echo mb 500 500 500 500 > /sys/kernel/debug/memguard/read_limit
