---
title: Filesystems
description: The block layer, partition tables, the VFS, and FatFs/lwext4 mounting, formatting and limits.
---

## Block layer

`software/src/block/` does not depend on board headers and is compiled directly by the host tests.
It provides:

- `gendisk`: the minimal description of a disk, with capacity, sector size and an operations
  table; the MMC driver registers `mmcblk0`.
- `block_device`: an open reference to a `gendisk`, with a start sector, sector count and read-only
  flag. Partitions are block devices too.
- Partition parsing: read LBA0 or the GPT header and publish `mmcblk0p1`, `mmcblk0p2`, and so on.
- Layout writing: when a whole-card layout is rebuilt, generate MBR or GPT as requested and erase
  the old table area.

Both MBR and GPT are supported. GPT validates the header CRC and the partition-entry CRC. Partition
type, start sector and sector count go into `struct storage_proxy_partition_info`, which the UI and
the proxy consume directly.

## VFS

`software/src/fs/vfs.c` keeps the mount table and exposes a common `open/read/write/seek/truncate/
stat/statfs` interface plus directory iteration. Filesystem types are registered at start-up:

| Type name | Implementation | Code |
| --- | --- | --- |
| `fat` / `fat32` | FatFs, FAT12/16/32 | `src/fs/vfs_fat.c` |
| `exfat` | FatFs exFAT | `src/fs/vfs_fat.c` |
| `ext` | lwext4, ext2/3/4 | `src/fs/vfs_ext.c` |

`vfs_probe()` mounts read-only once to determine the filesystem type, volume label and capacity;
`vfs_mount_ex()` performs the real mount based on the result. Mount points, open files and
filesystem types all have fixed limits set by `CONFIG_VFS_*`.

## FatFs

FatFs comes from a submodule; the project supplies only `software/src/fs/fatfs/ffconf.h`. The build
copies `ff.c`, `ffunicode.c`, `ffsystem.c` and `diskio.h` into the build directory and puts the
project configuration next to them, so the submodule's default `ffconf.h` cannot win.

Enabled options include long file names (`FF_USE_LFN=3`), exFAT, a dynamic working buffer
(`FF_USE_EXPAND=1`), and code page 437. Formatting uses `f_mkfs`; FatFs decides the minimum FAT32
partition size and cluster size, and returns `FR_NOT_ENOUGH_CORE` or `FR_MKFS_ABORTED` when the
partition is too small, which the VFS maps to `-ENOSPC`.

## lwext4

lwext4 comes from a submodule. The build copies it to `build/lwext4/` and applies the three patches
in `software/patches/lwext4/`:

| Patch | Effect |
| --- | --- |
| `0001-csum-seed.patch` | Support the metadata checksum seed `CSUM_SEED`; without it, ext4 volumes created by distributions such as Ubuntu fail checksum verification |
| `0002-inode-by-number.patch` | Read an inode by number; a directory entry already carries the number, so listing does not resolve the full path for every entry |
| `0003-mkfs-last-group.patch` | Fix the free-block accounting of the last block group on small volumes |

`CONFIG_BLOCK_DEV_CACHE_SIZE=64`, so each mounted volume holds 256 KiB of cache. The default eight
blocks (32 KiB) make directory blocks and inode-table blocks evict each other while readdir walks a
Debian-sized directory with thousands of entries, which is slow. Journaling is enabled.

## Formatting and capacity

Formatting from the UI and the shell goes through the VFS `format` hook:

- A quick format only recreates filesystem metadata; a full format wipes the partition data first.
- FAT32 has a minimum volume size (about 33 MiB).
- After formatting, the application rescans the partition table and the UI refreshes.

## Host tests

`storage_proxy_matrix` creates FAT32, exFAT, ext4 and mixed partitions on RAM disks, covers MBR and
GPT, and mounts, reads, writes, unmounts and remounts them. `lazy_proxy` then verifies the proxy
view's read and write behaviour and caching across all three filesystems.

The tests do not need a real SD card, but the RAM disk implements the same block operations as the
driver. Code that touches MMC registers, DMA or cache maintenance is not part of the host target.
