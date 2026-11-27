---
title: 文件系统
description: 块层、分区表、VFS，以及 FatFs 与 lwext4 的挂载、格式化和限制。
---

## 块层

`software/src/block/` 不依赖板级头文件，主机测试直接编译。它提供：

- `gendisk`：一块磁盘的最小描述，包括容量、扇区大小和操作表；MMC 驱动注册 `mmcblk0`。
- `block_device`：对 `gendisk` 的打开引用，带起始扇区、扇区数和只读标志；分区也是块设备。
- 分区解析：读 LBA0 或 GPT 头，生成 `mmcblk0p1`、`mmcblk0p2`……，发布到设备树。
- 布局写入：整盘重建分区表时，按请求生成 MBR 或 GPT，并擦除旧表所在区域。

分区表支持 MBR 和 GPT。GPT 会校验头部 CRC 和分区表 CRC。分区类型、起始扇区、扇区数都进入
`struct storage_proxy_partition_info`，界面和代理直接使用。

## VFS

`software/src/fs/vfs.c` 维护挂载表，向上提供统一的 `open/read/write/seek/truncate/stat/statfs`
和目录遍历。文件系统类型在启动时注册：

| 类型名 | 实现 | 代码 |
| --- | --- | --- |
| `fat` / `fat32` | FatFs，FAT12/16/32 | `src/fs/vfs_fat.c` |
| `exfat` | FatFs 的 exFAT | `src/fs/vfs_fat.c` |
| `ext` | lwext4，ext2/3/4 | `src/fs/vfs_ext.c` |

`vfs_probe()` 只读挂载一次来判断文件系统类型、卷标签和容量；`vfs_mount_ex()` 按探测结果做
正式挂载。挂载点、打开文件和文件系统类型都有数量上限，编译期由 `CONFIG_VFS_*` 决定。

## FatFs

FatFs 来自 submodule，本项目只提供配置文件 `software/src/fs/fatfs/ffconf.h`。构建时把
`ff.c`、`ffunicode.c`、`ffsystem.c` 和 `diskio.h` 复制到 build 目录，再把本项目配置放进去，
避免 submodule 默认的 `ffconf.h` 抢先。

启用的选项包括长文件名（`FF_USE_LFN=3`）、exFAT、动态工作缓冲（`FF_USE_EXPAND=1`），
代码页是 437。格式化入口是 `f_mkfs`；FAT32 的分区最小容量和簇大小由 FatFs 决定，容量不足时
返回 `FR_NOT_ENOUGH_CORE` 或 `FR_MKFS_ABORTED`，VFS 层转成 `-ENOSPC`。

## lwext4

lwext4 来自 submodule，构建时复制到 `build/lwext4/` 并应用 `software/patches/lwext4/` 下的
三个补丁：

| 补丁 | 作用 |
| --- | --- |
| `0001-csum-seed.patch` | 支持元数据校验和种子 `CSUM_SEED`，否则挂载 Ubuntu 等发行版创建的 ext4 会校验失败 |
| `0002-inode-by-number.patch` | 目录项已经带 inode 号，按号直接读 inode，避免为每个条目解析完整路径 |
| `0003-mkfs-last-group.patch` | 修正小容量卷最后一组的空闲块统计 |

`CONFIG_BLOCK_DEV_CACHE_SIZE=64`，每个挂载卷占 256 KiB 缓存。默认的 8 个块（32 KiB）在
Debian 这种上千项目录里会让目录块和 inode 表块互相淘汰，readdir 变得很慢。journaling 打开。

## 格式化和容量

界面和 shell 的格式化都走 VFS 的 `format` 钩子：

- quick 格式化只重建文件系统元数据；full 格式化先擦除分区数据。
- FAT32 不支持小于约 33 MiB 的分区；exFAT 和 ext4 没有这个限制，但 ext4 需要至少一个块组。
- 格式化完成后应用会重新扫描分区表，界面随之刷新。

## 主机测试

`storage_proxy_matrix` 在 RAM 盘上创建 FAT32、exFAT、ext4 和混合分区，覆盖 MBR 与 GPT，
依次挂载、读写、卸载、重新挂载。`lazy_proxy` 在此之上验证代理视图对三种文件系统的读写和
缓存行为。

测试不依赖真实 SD 卡，但 RAM 盘和真实驱动的块操作接口一致；涉及 MMC 寄存器、DMA 和缓存
维护的代码不在主机目标里。
