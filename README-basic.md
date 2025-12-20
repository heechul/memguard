# MemGuard-Basic

A minimal version of MemGuard that provides per-core memory bandwidth throttling. It omits writeback throttling, bandwidth reclaiming, and sharing found in the full MemGuard. Use this when you need a lightweight guard with minimal dependencies.

For the full feature set, see [README.md](README.md).

## Capabilities
- Per-core LLC miss-based throttling
- Lightweight module with reduced complexity

## Not Included
- Writeback throttling
- Bandwidth reclaiming or sharing
- Exclusive-mode controls

## Install

Build the basic module:
```bash
make basic
```

Load it:
```bash
insmod memguard-basic.ko
```

## Usage
Once loaded, set per-core LLC miss thresholds (values in MB/s):

Example: assign 500 MB/s for cores 0–3
```bash
echo mb 500 500 500 500 > /sys/kernel/debug/memguard/read_limit
```

Unload when done:
```bash
rmmod memguard
```
