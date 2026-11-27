---
title: 存储代理与 USB
description: 把多个分区合成一个 exFAT 卷导出给主机：视图结构、写回路径、容量和限制。
---

## 两个引擎

`software/apps/cardsight/storage/` 下有两套 exFAT 视图：

| 引擎 | 块设备 | 使用场景 |
| --- | --- | --- |
| `lazy/` | `cardview1` | 界面代理页和 `lazyproxy` 命令；按需索引目录，支持写回 |
| `eager/` | `cardview0` | `storageproxy` 和 `cardsight usb` 诊断命令；启动时全量索引 |

两套引擎共用 `storage_proxy.h` 的分区信息和选择掩码，但内部实现不同。界面默认使用懒索引引擎，
因为它不需要在开始导出前遍历整张卡。

## 视图长什么样

视图是一个真实的 exFAT 卷：24 个引导扇区、FAT、分配位图、up-case 表和根目录，簇大小 4 KiB。
根目录里：

- 每个被选中的源分区对应一个文件夹，名字是 `partition1`、`partition2`，编号和源分区一致。
- 两个只读文本文件，`_LAYOUT.TXT` 记录视图的几何和预留区，`_DIR_LIMIT.TXT` 记录当前的
  目录索引上限。两者读取时现场生成，每个占一个预留簇。
- 在真机上还会给第一个选中分区建一个 `System Volume Information` 文件夹，供 Windows
  挂载可写卷时使用；主机测试不创建它。

分区文件夹之下是源目录的镜像。目录在主机第一次读取时才索引；索引结果按 LRU 保留，缓存有上限，
被淘汰的目录下次读取时重新扫。源文件按连续的簇区间映射，不复制数据。

## 写回

主机的写入不直接落在源文件上：

1. 目录项、文件名、大小变化先进入 RAM 中的 overlay。
2. 数据先写入 pending 表，攒够后合并成 staging 区，staging 区是源分区上的临时文件。
3. flush 时把 staging 数据按区间写回源文件系统，删除临时文件，更新目录项。

这样主机在任意时刻看到的都是一个完整卷，不会出现写到一半的源文件。代价是：

- overlay、pending、staging 都有上限。超过上限时写入返回 `-ENOSPC`，主机会看到磁盘满。
- 写回发生在 flush 或停止时。拔卡时若还有未刷写的数据，代理会报告 `flush failed`，这部分数据
  丢失，无法恢复。
- 源分区全部只读时，staging 找不到可写分区，主机写入失败。

界面上启动代理时 USB 强制为读写模式；`lazyproxy usb on` 可以只读导出，用于只读核对。
`storageproxy` 在任一源分区只读时把整个视图标记为只读。

## 容量

视图容量由三部分组成：

- 每个选中分区按已用空间预留的簇（ext4 按每 4 KiB 已用数据一簇，FAT/exFAT 取视图簇数和源簇数
  的较大值，再加固定文件余量）；
- 系统区（位图、up-case 表、根目录）；
- 空闲尾区，等于各源分区空闲空间换算成视图簇后的总和，上限为 8M 簇（32 GiB）。

因此主机看到的容量随卡上实际数据量变化，不是简单地复制整卡容量。剩余空间只来自尾区；
预留区里的空闲簇不会重新分配。

## 索引限制

懒索引的默认上限（`lazy_types.h`，均可用 `-D` 覆盖）：

| 宏 | 默认值 | 含义 |
| --- | --- | --- |
| `CONFIG_STORAGE_PROXY_LAZY_MAX_DIRS` | 4096 | 视图最多索引多少个目录 |
| `CONFIG_STORAGE_PROXY_LAZY_NAME_BYTES` | 256 KiB | 目录名总字节数 |
| `CONFIG_STORAGE_PROXY_LAZY_CACHE_ENTRIES` | 8192 | 所有目录缓存合计最多多少项 |
| `CONFIG_STORAGE_PROXY_LAZY_CACHE_BYTES` | 512 KiB | 所有目录缓存合计最多多少字节 |
| `CONFIG_STORAGE_PROXY_LAZY_DIR_MAX_ENTRIES` | 16384 | 单个目录最多暴露多少项 |
| `CONFIG_STORAGE_PROXY_LAZY_OVERLAY_MAX` | 1024 | 写回 overlay 的记录数 |
| `CONFIG_STORAGE_PROXY_LAZY_PENDING_SECTORS` | 1024 | 待写扇区表大小 |
| `CONFIG_STORAGE_PROXY_LAZY_RUNS` | 4 | 同时打开的 staging 文件数 |
| `CONFIG_STORAGE_PROXY_LAZY_TAIL_CLUSTERS` | 8388608 | 空闲尾区上限（簇） |

超过目录项或名称上限时，`storage_proxy_lazy_truncated()` 返回 true，主机看到的目录会缺项。
`lazyproxy status` 会打印 `truncated 1`。

## USB 设备

USB 走 MUSB 设备控制器和 CherryUSB 的 MSC 类。传输缓冲使用片上 SRAM `0x00100000` 起的
64 KiB，不占 PSRAM。

Windows 10/11 上设备枚举为 `VID_1D6B/PID_0105`，挂载为名为 `CardSight SD Card` 的 exFAT 卷。
代理视图导出时主机看到的是合成的根目录；直连模式导出整盘时看到真实的 MBR/GPT 和分区。

拔卡时 USB 设备停止，代理视图撤下，主机端表现为设备断开。插回后需要重新启动导出。

## 手动操作

```text
lazyproxy start mmcblk0 0x3
lazyproxy usb rw
lazyproxy status
lazyproxy usb off
lazyproxy stop
```

`0x3` 表示导出分区 1 和 2。不传掩码时，`lazyproxy start mmcblk0` 使用所有存在的分区。

诊断时也可以用全量索引引擎：

```text
storageproxy start mmcblk0 0x1
cardsight usb on
cardsight usb off
```

## 测试

`ctest` 里的 `lazy_proxy` 在 RAM 盘上跑真实文件系统，覆盖这些行为：只读源的目录镜像、主机写入
回写、覆盖已有文件、删除、重命名、目录缓存淘汰、索引上限截断、容量和剩余空间报告、取消、
停止后重新启动。`storage_proxy_matrix` 覆盖全量索引引擎和 FAT32/exFAT/ext4 混合分区、GPT。

两个测试都不需要板子，也不访问寄存器。
