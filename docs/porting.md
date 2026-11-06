# 移植与扩展

## 目录约定

- `board/`：**板级事实**。全部是 C 常量和结构体（`sunxi_pin`、`sunxi_uart_config`、
  `sunxi_spi_config`、`sunxi_panel_config`、`sunxi_i2c_config`、`sunxi_mmc_config`、`SUNXI_*`
  基址/引脚/门控），实例是 `sunxi_uart0`、`sunxi_spi1`、`sunxi_panel0`、`sunxi_i2c0`、
  `sunxi_mmc0`；没有设备树，也没有芯片 `#if` 分发表。
- `src/drivers/`：控制器与设备驱动，按 Linux 的子系统布局分目录。命令、资源初始化、探测与注册
  都写在驱动实现里；无独立绑定文件。
  - `spi/spi-sunxi.h` + `spi/spi-sunxi.c`：SPI1 控制器（`sunxi_spi_init()`/
    `sunxi_spi_write()`），板级数据 `sunxi_spi1` 在头文件中声明。
  - `display/panel-st7789.h` + `display/panel-st7789.c`：ST7789 面板（复位、初始化表、窗口、
    像素），只经 `spi-sunxi.h` 的控制器 API 发数据；`display/display.c` 是显示子系统
    （`sunxi_display_*`），探测 `sunxi_panel0`、发布分辨率，并提供 shell `display color|test`。
  - `i2c/i2c-sunxi.h` + `i2c/i2c-sunxi.c`：轮询 TWI0 控制器（`sunxi_i2c_init()`/
    `sunxi_i2c_write()`/`sunxi_i2c_write_read()`），板级数据 `sunxi_i2c0` 在头文件中声明；
    `input/touchscreen/cst816.c` 只做 CST816 触摸协议。
  - `clk/sunxi-ccu.c`、`pinctrl/pinctrl-sunxi.c`、`tty/serial/uart-dw.c`：CCU 门控/复位、
    引脚与 GPIO、8250 兼容串口。
  - `mmc/`：`host/sunxi-mmc.h` + `host/sunxi-mmc.c` 只做 SMHC0 控制器（时钟、总线宽度、
    电压切换、命令引擎与 IDMAC、采样点、复位恢复、插卡引脚）；`core/sd.c` 只做 SD 协议
    （识别、CMD11 1.8 V、速度模式、CMD19 调谐、块读写带一次重试），不碰控制器寄存器；
    `card/mmcblk.c` 提供 gendisk `mmcblk0`、块操作、热插拔任务与 shell `mmc`
    （info/eject/insert/soak），公开 `mmc_host_get()`/`mmc_host_card_get()`。板级数据
    `sunxi_mmc0` 在 `host/sunxi-mmc.h`，三部分之间的契约（响应枚举、host 调用、`sd_*` 调用）
    在 `mmc/mmc-internal.h`。
- **分层规则**：协议/设备驱动不访问控制器寄存器，只调用驱动旁边声明的控制器 API；板级事实与
  引脚统一放在 `board/cardsight_board.c` 的数组里。
- `src/arch/`、`src/kernel/`：启动、陷阱、PLIC、CLINT、FreeRTOS 胶水、initcall、console、
  shell、日志。所有初始化通过 `INITCALL(level, priority, fn)` 注册。
- `src/block/`、`src/fs/`：通用块层（gendisk/block_device/分区解析）、VFS 与 FatFs/lwext4 胶水；
  **块层不依赖板级头文件**，主机测试直接编译它们。
- `include/`：公开 API（`vfs.h`、`shell.h`、`hal/*`、`usb/mass_storage.h`、`gui/lvgl_port.h`、
  `drm/sunxi_display.h`）。

## 加一块新板

1. 在 `board/` 增加一个板级 C 文件（或改现有文件）：UART、SD 控制器的基址/时钟门控/引脚、
   屏与触摸的引脚、USB 缓冲地址等。所有字段都是常量，编译期可查。
2. 更新链接脚本里的 `ORIGIN/LENGTH`（本板是 PSRAM `0x12000000`、8 MiB），并确认
   `__heap_start/__heap_end` 的余量。
3. 在 `cmake/syterkit.cmake` 里选对应的 SyterKit 板配置来生成 eFEX；启动地址要与
   `xfel write` 的地址一致。
4. 如果控制器寄存器布局不同，改 `src/drivers/<子系统>/` 里的实现；**不要**再引入
   `platform/common/` 或按芯片包含的分发表。

## 加一个新外设

- 新外设按子系统放进 `src/drivers/<子系统>/`，文件名用 `-` 连接（如 `spi/spi-sunxi.c` 与
  `spi-sunxi.h`、`display/panel-st7789.c` 与 `display/display.c`）。配置结构体与 `extern`
  实例声明放在驱动自己的头文件（如 `src/drivers/spi/spi-sunxi.h`），具体实例（基址、引脚、
  时钟门控、频率）加到 `board/cardsight_board.c`。
- 用 `INITCALL` 注册初始化；需要调度器的（比如 LVGL 任务）放在 level 3。
- 用 `<hal/io.h>` 的 `readl/writel`（值在前）与 `setbits32/clrbits32/clrsetbits32`，
  不新增私有 MMIO 包装。
- 轮询寄存器时不要用 `ktime_get_ms()` 做内层循环（64 位除法），用
  `cs_clint_ticks()` + `cs_clint_frequency()` 的截止时间；需要睡眠时用 `msleep`/`cs_udelay`。
- 数据经 DMA 进出时，发送前 `dcache_clean_range()`，接收后 `dcache_invalidate_range()`
  （`include/hal/cache.h`，T-Head CMO 指令，64 字节行）。缓冲区要按 64 字节对齐；
  非对齐的块传输在 MMC 驱动里有 PSRAM bounce 缓冲的先例。
- 驱动要能被多个任务调用时，在驱动内用 `mutex` 串行化（见 `mmc/card/mmcblk.c` 的
  `mmc_lock`）。

## 构建开关

- `CARDSIGHT_WITH_APP`：是否编应用与 LVGL（默认 ON）。
- `CARDSIGHT_HOST_TESTS`：构建主机测试。
- `CARDSIGHT_WERROR`：把自有代码的警告变成错误（默认 OFF）。
- `XUANTIE_TOOLCHAIN`：交叉工具链前缀目录；默认 `../toolchains/Xuantie-900-gcc-elf-newlib-x86_64-V3.2.0`。

## 主机测试

`tests/host/` 用本机编译器编译真实的块层、VFS、文件系统和存储代理，配 RAM 盘与桩
（`ktime`、日志、互斥）。加功能时优先在这里加回归用例；涉及寄存器的代码不要放进
主机目标。
