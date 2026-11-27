---
title: 烧写与启动
description: 用 xfel 通过 FEL 加载 PSRAM 初始化、应用镜像和 C907 启动桩，不写 flash。
---

开发期只用 RAM/FEL 启动：不写 SPI NOR，不擦除 flash，也不在启动流程里改写 SD 卡。上电后
BROM 等待 FEL；同一时间只允许一块板处于 FEL 模式。

## 三步加载

```sh
# 1. E907 初始化 PSRAM，完成后返回 FEL
xfel write 0x00100000 software/build/riscv/images/init_psram_efex.bin
xfel exec  0x00100000

# 2. C907 应用镜像，链接地址 0x12000000
xfel write 0x12000000 software/build/riscv/images/cardsight.bin

# 3. E907 启动 C907，入口 0x12000000
xfel write 0x00100000 software/build/riscv/images/c907_boot_efex.bin
xfel exec  0x00100000
```

两个 eFEX 来自 SyterKit 的 `yuzuki-cardsight` 板配置：

- `init_psram_efex.bin` 在 E907 上把 8 MiB PSRAM 初始化到 SQPI 100 MHz，串口打印
  `PSRAM ready; returning to FEL for image download at 12000000` 后回到 FEL。
- `c907_boot_efex.bin` 释放 C907，等 UART 把日志发完再跳转。

串口是 UART0（PH13/PH14），115200 8N1。日志格式与 SyterKit 一致：
`[秒.微秒][级别] 模块: 消息`，级别带颜色。

## 地址与内存

| 区域 | 地址 | 用途 |
| --- | --- | --- |
| 片上 SRAM | `0x00100000` | eFEX 运行区；低 64 KiB 保留给 USB MSC 传输缓冲 |
| PSRAM | `0x12000000` | 应用镜像、`.bss`、堆；8 MiB |
| DRAM | `0x40000000` | 本启动流程不初始化，不使用 |

eFEX 跳走之后，SRAM 的低 64 KiB 才由应用的 USB 传输缓冲使用。

## 上电后

串口出现 `cardsight>` 提示符，屏幕同时显示界面。没有卡时五个页面都能进入，直连和代理按钮
禁用。插入卡后驱动识别、发布 `mmcblk0`，界面自动刷新分区信息。

ramdisk/FEL 启动会丢掉所有内存内容，重新上电需要重新走三步。SD 卡上的数据不受影响。

## 注意

- `xfel write` 写 PSRAM 期间不要给另一块板执行 FEL 命令，也不要断电。
- 不要用这套流程写 SPI NOR 或改 flash 分区；仓库不包含 bootloader 写入。
- eFEX 地址固定为 `0x00100000`，应用入口固定为 `0x12000000`；换链接地址时三处要一起改。
