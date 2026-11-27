---
title: Porting
description: "Add a board or peripheral: board facts, driver layering and the build hooks."
---

## Board facts

`software/board/` holds constants and structures for this board: UART, MMC, SPI, I2C, panel, touch,
clock gates and the USB buffer address. There is no device tree and no per-chip dispatch table.
Instances such as `sunxi_uart0`, `sunxi_mmc0` and `sunxi_spi1` are plain C objects.

Register-level details that belong to a controller live in that controller's driver. For example,
USB PHY setup is in `src/usb/usb_platform.c`, the panel init table is in
`src/drivers/display/panel-st7789.c`, and the SD voltage switch is in the MMC host driver.

## Driver layering

- Protocol drivers implement a protocol but do not touch controller registers. `mmc/core/sd.c`
  performs card identification, CMD11, speed-mode selection and CMD19 tuning through the host API.
- Controller drivers implement the hardware. `mmc/host/sunxi-mmc.c` handles SMHC0 clocks, voltage,
  commands, IDMAC and sampling points; it does not implement the SD state machine.
- Device drivers use subsystem APIs: `spi-sunxi.h`, `i2c-sunxi.h`, `sunxi_display.h`.
- Block, VFS, filesystem and storage-proxy code is board-independent and runs in the host tests.

## Adding a board

1. Add a board C file under `software/board/`, or edit the existing one, with the UART, MMC, SPI,
   I2C, panel, touch and USB addresses, pins and clock gates.
2. Update the linker script: PSRAM origin and length, heap symbols, stack size and any memory
   reservations. The current board links at `0x12000000` with 8 MiB of PSRAM.
3. Select the matching SyterKit board configuration in `software/cmake/syterkit.cmake` for the eFEX
   images. The FEL address must match the application link address.
4. If a controller register layout differs, change the implementation under
   `software/src/drivers/<subsystem>/`. Do not reintroduce a platform dispatch table or a device
   tree.

## Adding a peripheral

- Put the driver under `software/src/drivers/<subsystem>/`, with the implementation and its header
  together. Declare the board configuration struct and `extern` instance in the driver header; put
  the concrete instance in `software/board/cardsight_board.c`.
- Register initialisation with `INITCALL(level, priority, function)`. Anything that needs the
  scheduler, such as an LVGL task, belongs at level 3.
- Use `readl`/`writel` and `setbits32`/`clrbits32`/`clrsetbits32` from `<hal/io.h>`.
  Use `cs_clint_ticks()` with a deadline for polling loops, and `msleep`/`cs_udelay` for delays.
- For DMA, clean the cache before sending and invalidate it after receiving
  (`dcache_clean_range`, `dcache_invalidate_range`). Align buffers to 64 bytes.
- If several tasks can call a driver, serialise it with a mutex owned by the driver.

## Build hooks

Build options are in [Configuration](../reference/configuration.md). The `riscv` and `host` presets
are in `software/CMakePresets.json`; `CARDSIGHT_BUILD_EFEX` controls the SyterKit images and
`CARDSIGHT_WITH_APP` gates the application sources.

New source files under `apps/cardsight/` and its subdirectories are picked up by the globs in
`software/src/CMakeLists.txt` and the host-test CMake file. A new top-level driver directory may
need an explicit entry in `CS_PLATFORM_SOURCES`.

## Tests and simulator

Add a host test when the new code does not touch registers. UI changes can be checked with `ui_sim`
and the browser simulator. The test strategy is in [Testing](testing.md); coding and review rules are
in [CONTRIBUTING.md](https://github.com/YuzukiHD/CardSight/blob/master/CONTRIBUTING.md).
