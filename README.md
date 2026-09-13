# yFS for FreeBSD (FUSE v3)

A working user-space implementation of **yFS** (a journaling file system for FreeBSD) backed by **FUSE v3** (`libfuse3`), designed according to the principles presented in the USENIX FAST '03 paper:
> *Xiang Zhang, Ningning Zou, Rodney Van Meter.* **"Designing a Journaling File System for FreeBSD"**, *USENIX Conference on File and Storage Technologies (FAST '03).*

---

## 1. System Architecture & How It Works

yFS incorporates transaction-based write-ahead logging (WAL) into a Fast File System (FFS/UFS) style on-disk layout to guarantee metadata consistency and fast crash recovery without synchronous disk writes or long `fsck` scans.

```
                      +-----------------------------+
                      |    POSIX System Calls       |
                      +-----------------------------+
                                     |
                                     v
                      +-----------------------------+
                      |   FreeBSD fusefs Kernel     |
                      +-----------------------------+
                                     |  /dev/fuse
                                     v
                      +-----------------------------+
                      |    yfs_fuse (FUSE v3)       |
                      | (fuse_lowlevel_ops driver)  |
                      +-----------------------------+
                                     |
                                     v
                      +-----------------------------+
                      |       yFS Core Engine       |
                      |  (Inode, Directory, bmap)   |
                      +-----------------------------+
                                     |
                   +-----------------+-----------------+
                   |                                   |
                   v                                   v
        +---------------------+             +---------------------+
        | Transaction Manager |             |    Buffer Cache     |
        |  & Write-Ahead Log  |             |  (LRU Cached Blocks)|
        +---------------------+             +---------------------+
                   |                                   |
                   +-----------------+-----------------+
                                     |
                                     v
                      +-----------------------------+
                      |     Virtual Block Device    |
                      |    (Disk Image / Raw Dev)   |
                      +-----------------------------+
```

### On-Disk Layout

An image formatted with `newfs_yfs` is partitioned into:
1. **Superblock (Block 0)**: Filesystem magic (`0x59465331`), block size (4096B), block/inode counts, cylinder group geometry, clean unmount flags, and journal pointers.
2. **Cylinder Group Descriptors (Block 1)**: Metadata tracking per-group free blocks and free inodes.
3. **Journal Area (Blocks 2 .. N)**: A circular write-ahead transaction log.
4. **Cylinder Group 0..M**:
   - **Data Block Allocation Bitmap**: Tracks data block availability.
   - **Inode Allocation Bitmap**: Tracks inode allocation state.
   - **Inode Table**: Fixed-size 128-byte `yfs_dinode` structures (direct, single-indirect, and double-indirect block pointers).
   - **Data Blocks**: Extents storing file contents and packed `yfs_dirent` directory records.

### Transaction Lifecycle & Write-Ahead Logging (WAL)

1. **Transaction Begin (`TX_BEGIN`)**: Atomic operations (file creation, directory linking, metadata updates) obtain a unique monotonically increasing transaction ID (`txid`).
2. **Buffer Modification**: Dirty metadata blocks are tagged with the active `txid`.
3. **Transaction Commit (`TX_COMMIT`)**: Log records containing redo descriptions and block payloads are appended to the circular journal. Once the commit record is appended, the transaction is durable.
4. **Checkpointing**: Dirty buffers in memory are lazily flushed to their home block locations on disk, allowing the journal's tail pointer to advance and reclaim circular log space.
5. **Crash Recovery**: If an unclean shutdown occurs, mounting yFS triggers `Journal::recover()`, scanning the journal from tail to head, replaying all committed redo blocks, and discarding aborted/uncommitted transactions.

---

## 2. Building and Running

### Prerequisites on FreeBSD

- FreeBSD 14+ / 15+
- Clang 19 / C++17
- CMake & GNU Make
- `fusefs-libs3` (`pkg install fusefs-libs3`)
- `fusefs.ko` kernel module loaded (`kldload fusefs`)

### Compilation

```sh
mkdir -p build && cd build
cmake ..
cmake --build .
```

This produces three binaries:
- `newfs_yfs`: FreeBSD-style formatting tool for yFS images and partitions.
- `yfs_fuse`: FUSE v3 daemon.
- `yfs_test`: Unit and crash-recovery test suite.

### Formatting and Mounting

```sh
# 1. Format a 128MB virtual disk image
./build/newfs_yfs -s 128 /tmp/my_yfs.img

# 2. Mount with FUSE v3
mkdir -p /tmp/mnt_yfs
./build/yfs_fuse /tmp/my_yfs.img /tmp/mnt_yfs

# 3. Use standard POSIX commands
echo "Testing yFS on FreeBSD" > /tmp/mnt_yfs/hello.txt
ls -la /tmp/mnt_yfs
cat /tmp/mnt_yfs/hello.txt

# 4. Unmount
umount /tmp/mnt_yfs
```

---

## 3. Benchmarks & Validation: yFS vs. FreeBSD UFS (with Soft-Updates)

Benchmarks were conducted on **FreeBSD 15.1-RELEASE (x86_64)** comparing **yFS (FUSE v3 user-space)** against native kernel **UFS2 (with Soft Updates)** on memory-backed block devices.

### A. File System eXerciser (`fsx`) Stress Testing

`fsx` runs pseudorandom sequences of reads, writes, truncations, and hole creations:

| File System | `fsx` Operations | Truncate / Hole Integrity | Result |
|---|---|---|---|
| **yFS (FUSE v3)** | 5,000 Ops | Passed (0 bad data) | **A-OK** |
| **UFS2 (Kernel)** | 5,000 Ops | Passed (0 bad data) | **A-OK** |

### B. Flexible I/O Tester (`fio`) Performance Comparison

*Workload: 32MB Sequential Read/Write, Block Size = 4KB, `ioengine=psync`, 1 job.*

| Benchmark Metric | yFS (FUSE v3 User-Space) | FreeBSD UFS2 (Kernel + Soft-Updates) |
|---|---|---|
| **Sequential Write Bandwidth** | **2,353 KiB/s (~2.4 MB/s)** | **1,103 MiB/s (~1.15 GB/s)** |
| **Sequential Write IOPS** | **588 IOPS** | **282,000 IOPS** |
| **Sequential Write Latency (avg)** | **1.69 ms** | **3.07 µs** |
| **Sequential Read Bandwidth** | **14.1 MiB/s (~14.8 MB/s)** | **1,032 MiB/s (~1.08 GB/s)** |
| **Sequential Read IOPS** | **3,613 IOPS** | **264,000 IOPS** |
| **Sequential Read Latency (avg)** | **275.8 µs** | **2.60 µs** |

### Performance Analysis

1. **User-Space vs. In-Kernel Context Transitions**:
   - yFS operates in user-space via `/dev/fuse`. Every I/O request transitions through the FreeBSD VFS layer, the `fusefs.ko` kernel module, IPC context switches to `yfs_fuse`, and buffer copying.
   - UFS2 operates directly inside the FreeBSD kernel with zero user-space context switches.
2. **Transaction Logging Overhead**:
   - yFS writes transaction records and block mutations to the write-ahead journal for every metadata change.
   - UFS with Soft Updates reorders and batches dependency trees asynchronously in kernel memory without synchronous log updates.
3. **Correctness & Reliability**:
   - Both file systems successfully pass aggressive random I/O and truncation consistency checks with `fsx`.
