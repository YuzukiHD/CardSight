---
title: 命令行
description: cardsight> 下的块设备、VFS、MMC、USB、代理和界面诊断命令。
---

串口 115200 8N1 打开后是 `cardsight>`。命令表来自各模块注册的 `SHELL_CMD`；参数写错时命令
会打印自己的用法。

## 块设备

| 命令 | 作用 |
| --- | --- |
| `blk` | 列出块设备、容量和只读标志 |
| `blkread <dev> <sector>` | 读一个 512 字节扇区并打印十六进制 |
| `blkbench <dev> <sector> <MiB>` | 从指定扇区顺序读，报告吞吐 |

## SD 卡

| 命令 | 作用 |
| --- | --- |
| `mmc` | 卡信息：容量、模式、时钟、位宽、调谐状态 |
| `mmc eject` | 把 CD 状态强制为“不在位”，卡仍在槽里且不断电 |
| `mmc insert` | 重新识别当前卡 |
| `mmc soak [n]` | 重复移除/识别 n 次，统计失败 |

`mmc eject` 模拟的是热插拔路径，不是真拔卡。真拔卡会断电，重新插入仍走 3.3 V → CMD11 →
SDR104；模拟移除后重新识别可能停在 SDR25。

## 文件系统

| 命令 | 作用 |
| --- | --- |
| `mount [blk target [fstype]]` | 挂载分区；不带参数时列出挂载点 |
| `umount <target>` | 卸载 |
| `ls [path]` | 列目录 |
| `cat <path>` | 读文件到控制台 |
| `df <path>` | 卷容量和剩余空间 |
| `write <path> <text>` | 写文本文件 |
| `rm <path>` / `rmdir <path>` | 删除文件 / 空目录 |

## USB 与存储代理

| 命令 | 作用 |
| --- | --- |
| `lazyproxy start <disk> [mask]` | 启动懒索引 exFAT 视图 `cardview1`；mask 是分区位掩码 |
| `lazyproxy stop` | 停止视图 |
| `lazyproxy usb on\|rw\|off` | 把视图导出为只读或读写 USB 设备，或停止 USB |
| `lazyproxy` / `lazyproxy status` | 几何、预留区、目录/缓存用量、overlay、读写计数、最后错误 |
| `storageproxy start <disk> [mask]` | 启动全量索引视图 `cardview0`（诊断用） |
| `storageproxy stop` / `status` / `count` / `reset` | 全量索引视图的控制和统计 |
| `cardsight usb on` | 启动代理视图并导出 USB（走全量索引引擎） |
| `cardsight usb off` | 停止 USB 和代理 |
| `cardsight usb disk` | 只读导出整盘，供主机核对真实分区表 |
| `cardsight usb-test` | 只读代理 + USB 的启动/停止生命周期自检 |

界面里的代理页使用懒索引引擎，和 `lazyproxy` 是同一套代码。`storageproxy` 和 `cardsight usb`
保留给诊断，两者都基于全量索引，启动时会遍历分区目录。

## 显示与界面

| 命令 | 作用 |
| --- | --- |
| `display color <rgb565>` | 整屏填色，检查屏和 SPI 通路 |
| `display test` | 内置测试图 |
| `panel` | 第二个 hart 的像素推送状态 |
| `lvgl` | 刷新次数、像素数、耗时 |
| `lvgl reset` | 清零统计 |
| `lvgl bench` | 线速和重绘基准 |
| `cardsight page <direct\|proxy\|disk\|info\|layout>` | 切换界面页 |
| `cardsight radius [px]` | 打开圆角校准叠层 |
| `cardsight ui-test` | 遍历对象树，做界面自检 |
| `cardsight memtest [n]` | 重建界面 n 次，比较堆用量 |
| `cardsight memcheck` | 检查列表淡出条和 LVGL 绘制缓冲的守护字 |
| `screenshot` | 把当前屏幕导出为 RLE/base64 |

## 系统

| 命令 | 作用 |
| --- | --- |
| `help` | 列出命令 |
| `heap` | 内核堆和 C 库堆的使用量 |
| `stack` | 各任务栈的高水位 |
| `uptime` | 启动后的毫秒数 |
| `cache` | XuanTie 缓存寄存器与带宽检查 |

## 常用组合

确认卡和分区：

```text
mmc
blk
mount mmcblk0p2 /mnt/fat fat
ls /mnt/fat
```

把一个分区通过代理导出给电脑：

```text
lazyproxy start mmcblk0 0x2
lazyproxy usb rw
```

测顺序读：

```text
blkbench mmcblk0 2048 64
```

`blkbench` 的第三个参数是 MiB，不是扇区数。读入片上 SRAM 时实测约 62 MiB/s；读入 PSRAM
时约 9.6 MiB/s，瓶颈在 PSRAM 写入路径。
