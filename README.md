Original repo: https://github.com/heechul/memguard

This repo is an extension of the MemGuard tool that utilizes an additional
counter for limiting write intensive applications without ruining the
performance of read intensive applications.

An example of it's utilization can be seen in [CacheDOS](https://github.com/mbechtel2/CacheDOS).

Install
===========
	# make
	# insmod memguard.ko
	    <-- Load the module by doing

Usage
===========  
Once the module is loaded, the thresholds can be set as follows:

	- per-core LLC miss threshold assignment.

	assign 500 MB/s for Cores 0,1,2,3
	# echo mb 500 500 500 500 > /sys/kernel/debug/memguard/read_limit

    - per-core LLC writeback threshold assignment.

	assign 100 MB/s for Cores 0,1,2,3
	# echo mb 100 100 100 100 > /sys/kernel/debug/memguard/write_limit
