---
title: 移植与扩展
description: 板级事实、驱动分层、新板和新外设的接入步骤，以及构建开关。
---

## 目录约定

- `software/board/` 放**板级事实**。全部是 C 常量和结构体（`sunxi_pin`、`sunxi_uart_config`、
  `sunxi_spi_config`、`sunxi_panel_config`、`sunxi_i2c_config`、`sunxi_mmc_config`、
  `SUNXI_*` 基址/引脚/门控），实例是 `sunxi_uart0`、`sunxi_spi1`、`sunxi_panel0`、
  `sunxi_i2c0`、`sunxi_mmc0`；没有设备树，也没有按芯片分发的 `#if` 表。
- `software/src/drivers/` 放控制器与设备驱动，按 Linux 的子系统布局分目录。命令、资源初始化、
  探测与注册都写在驱动实现里，没有独立的绑定文件。
  - `spi/spi-sunxi.h` + `spi/spi-sunxi.c`：SPI1 控制器，板级数据 `sunxi_spi1` 在头文件里声明。
  - `display/panel-st7789.h` + `display/panel-st7789.c`：ST7789 面板，只经 `spi-sunxi.h` 的
    控制器 API 发数据；`display/display.c` 是显示子系统，探测 `sunxi_panel0`、发布分辨率，
    提供 shell `display color|test`。
  - `i2c/i2c-sunxi.h` + `i2c/i2c-sunxi.c`：轮询 TWI0 控制器；`input/touchscreen/cst816.c`
    只做 CST816 触摸协议。
  - `clk/sunxi-ccu.c`、`pinctrl/pinctrl-sunxi.c`、`tty/serial/uart-dw.c`：CCU 门控/复位、
    引脚与 GPIO、8250 兼容串口。
  - `mmc/`：`host/sunxi-mmc.*` 只做 SMHC0 控制器（时钟、总线宽度、电压切换、命令引擎与
    IDMAC、采样点、复位恢复、插卡引脚）；`core/sd.c` 只做 SD 协议（识别、CMD11 1.8 V、
    速度模式、CMD19 调谐、块读写带一次重试），不碰控制器寄存器；`card/mmcblk.c` 提供
    gendisk `mmcblk0`、块操作、热插拔任务和 shell `mmc`。
- **分层规则**：协议/设备驱动不访问控制器寄存器，只调用驱动旁边声明的控制器 API。板级事实
  和引脚统一放在 `software/board/cardsight_board.c` 的数组里。
- `software/src/arch/`、`software/src/kernel/`：启动、陷阱、PLIC、CLINT、FreeRTOS 胶水、
  initcall、console、shell、日志。所有初始化通过 `INITCALL(level, priority, fn)` 注册。
- `software/src/block/`、`software/src/fs/`：通用块层、VFS 与 FatFs/lwext4 胶水。块层不依赖
  板级头文件，主机测试直接编译。
- `software/apps/cardsight/` 按能力分子目录：`app/` 是状态模型、请求 worker、卡片/文件/格式/
  布局操作和 shell 诊断；`storage/lazy/` 是 USB 导出的懒索引 exFAT 引擎；`storage/eager/` 是
  只用于诊断的全量索引引擎；`ui/` 顶层放主题、壁纸、图标和截图，`ui/core/` 放外壳/标签页/
  对话框/横幅，`ui/widgets/` 放控件构造函数，`ui/pages/` 放五个页面。私有头文件跟着模块走，
  公共头（`storage_proxy.h`、`proxy_lazy.h`、`ui.h`、`app.h`）保持稳定。
- `software/include/`：公开 API（`vfs.h`、`shell.h`、`hal/*`、`usb/mass_storage.h`、
  `gui/lvgl_port.h`、`drm/sunxi_display.h`）。

## 加一块新板

1. 在 `software/board/` 增加一个板级 C 文件（或改现有文件）：UART、SD 控制器的基址/时钟门控/
   引脚、屏与触摸的引脚、USB 缓冲地址等。所有字段都是常量，编译期可查。
2. 更新链接脚本里的 `ORIGIN/LENGTH`（本板是 PSRAM `0x12000000`、8 MiB），并确认
   `__heap_start/__heap_end` 的余量。
3. 在 `software/cmake/syterkit.cmake` 里选对应的 SyterKit 板配置来生成 eFEX；启动地址要与
   `xfel write` 的地址一致。
4. 如果控制器寄存器布局不同，改 `software/src/drivers/<子系统>/` 里的实现；不要再引入
   `platform/common/` 或按芯片包含的分发表。

## 加一个新外设

- 新外设按子系统放进 `software/src/drivers/<子系统>/`，文件名用 `-` 连接（如
  `spi/spi-sunxi.c` 与 `spi-sunxi.h`、`display/panel-st7789.c` 与 `display/display.c`）。
  配置结构体与 `extern` 实例声明放在驱动自己的头文件，具体实例（基址、引脚、时钟门控、频率）
  加到 `software/board/cardsight_board.c`。
- 用 `INITCALL` 注册初始化；需要调度器的（比如 LVGL 任务）放在 level 3。
- 用 `<hal/io.h>` 的 `readl/writel`（值在前）与 `setbits32/clrbits32/clrsetbits32`，
  不新增私有 MMIO 包装。
- 轮询寄存器时不要用 `ktime_get_ms()` 做内层循环（64 位除法），用 `cs_clint_ticks()` +
  `cs_clint_frequency()` 的截止时间；需要睡眠时用 `msleep`/`cs_udelay`。
- 数据经 DMA 进出时，发送前 `dcache_clean_range()`，接收后 `dcache_invalidate_range()`
  （`software/include/hal/cache.h`，T-Head CMO 指令，64 字节行）。缓冲区按 64 字节对齐；
  非对齐的块传输在 MMC 驱动里有 PSRAM bounce 缓冲的先例。
- 驱动要能被多个任务调用时，在驱动内用 `mutex` 串行化（见 `mmc/card/mmcblk.c` 的
  `mmc_lock`）。

## 构建开关

| 变量 | 作用 |
| --- | --- |
| `CARDSIGHT_WITH_APP` | 是否编译应用与 LVGL（默认 ON） |
| `CARDSIGHT_HOST_TESTS` | 构建主机测试 |
| `CARDSIGHT_BUILD_EFEX` | 是否构建 SyterKit eFEX（默认 ON） |
| `CARDSIGHT_WERROR` | 把自有代码的警告变成错误（默认 OFF） |
| `XUANTIE_TOOLCHAIN` | 交叉工具链前缀目录，必须通过环境变量或 `-D` 提供 |

## 主机测试与模拟器

`software/tests/host/` 用本机编译器编译真实的块层、VFS、文件系统和存储代理，配 RAM 盘与桩
（`ktime`、日志、互斥）。加功能时优先在这里加回归用例；涉及寄存器的代码不要放进主机目标。

界面改动可以用 `ui_sim` 渲染成 PPM 检查，或用 `docs/` 的 WebAssembly 模拟器直接触摸。两者
都编译同一份 `ui/` 源码，不维护第二套界面。加页面时同时更新 `ui_private.h` 的 `tiles` 数量、
`ui_core.c` 的构建顺序和标签栏。
