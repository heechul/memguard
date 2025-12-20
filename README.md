# MemGuard

MemGuard is a memory bandwidth reservation system for multi-core platforms that provides guaranteed memory bandwidth allocation to protect real-time and critical tasks from memory bandwidth contention.

For the basic version, see [README-basic.md](README-basic.md).

## Features

- Read/write separate bandwidth reservation
	- Per-core LLC miss threshold assignment
	- Per-core LLC writeback threshold assignment  
- Support for reserved bandwidth reclaiming
- Support for best-effort bandwidth sharing

## ChangeLog

- **Dec 2022**: 5.15+ kernel support
- **May 2022**: 
  - Read/write separate reservation (from [RTAS'19](https://www.ittc.ku.edu/~heechul/papers/cachedos-rtas2019-camera.pdf))
  - Bandwidth reclaiming re-enabled (originally from [RTAS'13](https://www.ittc.ku.edu/~heechul/papers/memguard-rtas13.pdf))

## Installation

### Build

```bash
make
```

### Load the module

```bash
insmod memguard.ko
```

## Usage

Once the module is loaded, configure bandwidth thresholds and controls as follows:

### Per-core LLC miss threshold assignment

Assign 500 MB/s for Cores 0,1,2,3:

```bash
echo mb 500 500 500 500 > /sys/kernel/debug/memguard/read_limit
```

### Per-core LLC writeback threshold assignment

Assign 100 MB/s for Cores 0,1,2,3:

```bash
echo mb 100 100 100 100 > /sys/kernel/debug/memguard/write_limit
```

### Reclaim control

Enable reclaiming of reserved bandwidth:

```bash
echo reclaim 1 > /sys/kernel/debug/memguard/control
```

### Exclusive mode control

**Strict reservation** - Disable best-effort sharing and use only guaranteed bandwidth:

```bash
echo exclusive 0 > /sys/kernel/debug/memguard/control
```

**Spare bandwidth sharing mode** - Enable best-effort bandwidth sharing (see RTAS'13):

```bash
echo exclusive 2 > /sys/kernel/debug/memguard/control
```

**Proportional share mode** - Enable proportional best-effort bandwidth sharing (see TC'15):

```bash
echo exclusive 5 > /sys/kernel/debug/memguard/control
```
