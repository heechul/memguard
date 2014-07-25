MemGuard: Memory Bandwidth Reservation System

Heechul Yun (heechul@illinois.edu)

Preparation
===========  
Recommended kernel settings are as follows:

	CONFIG_ACPI_PROCESSOR=n
	CONFIG_CPU_IDLE=n

Install
===========
	# make
	# insmod memguard.ko	
	    <-- Load the module by doing

Usage
===========  

Once the module is loaded, it provides several configuration interfaces as follows:

	- per-core bandwidth assignment.

	assign 900,100,100,100 MB/s for Core 0,1,2,3
	# echo mb 900 100 100 100 > /sys/kernel/debug/memguard/limit

	assign 70%,10%,10%,10% of guaranteed bandwidth(r_min) for Core 0,1,2,3
	# echo 70 10 10 10 > /sys/kernel/debug/memguard/limit

	assign weights(1:2:4:8) for Core 0,1,2,3
	# echo 1 2 4 8 > /sys/kernel/debug/memguard/share


	- per-task mode (use task priority as core's memory weight)

	enable per-task mode
	# echo taskprio 1 > /sys/kernel/debug/memguard/control

	disble per-task mode (=per-core mode)
	# echo taskprio 0 > /sys/kernel/debug/memguard/control


	- set maxbw (r_min in the paper)

	# echo maxbw 1200 > /sys/kernel/debug/memguard/control


	- reclaim control

	enable
	# echo reclaim 1 > /sys/kernel/debug/memguard/control

	disable
	# echo reclaim 0 > /sys/kernel/debug/memguard/control


	- exclusive mode control

	strict reservation. (disable best-effort sharing. only use guaranteed bw)
	# echo exclusive 0 > /sys/kernel/debug/memguard/control

	last exclusive. last core exclusively use the rest b/w (not in the paper)
	# echo exclusive 1 > /sys/kernel/debug/memguard/control

	spare b/w sharing mode (see RTAS13)
	# echo exclusive 2 > /sys/kernel/debug/memguard/control

	proportional share mode (see TC submission)  
	# echo exclusive 5 > /sys/kernel/debug/memguard/control
