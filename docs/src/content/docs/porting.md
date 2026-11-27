---
title: Porting
description: Board facts, driver layering, adding a board or peripheral, and the build switches.
---

## Source layout

- `software/board/` holds **board facts**. Everything is a C constant or struct (`sunxi_pin`,
  `sunxi_uart_config`, `sunxi_spi_config`, `sunxi_panel_config`, `sunxi_i2c_config`,
  `sunxi_mmc_config`, `SUNXI_*` bases/pins/gates); the instances are `sunxi_uart0`, `sunxi_spi1`,
  `sunxi_panel0`, `sunxi_i2c0` and `sunxi_mmc0`. There is no device tree and no per-chip `#if`
  table.
- `software/src/drivers/` holds controller and device drivers, laid out by Linux subsystem. Commands,
  resource setup, probing and registration all live in the driver; there are no separate binding
  files.
  - `spi/spi-sunxi.h` + `spi/spi-sunxi.c`: the SPI1 controller. Board data `sunxi_spi1` is declared
    in the header.
  - `display/panel-st7789.h` + `display/panel-st7789.c`: the ST7789 panel, which sends data only
    through the controller API in `spi-sunxi.h`. `display/display.c` is the display subsystem: it
    probes `sunxi_panel0`, publishes the resolution and provides the `display color|test` shell
    commands.
  - `i2c/i2c-sunxi.h` + `i2c/i2c-sunxi.c`: the polled TWI0 controller; `input/touchscreen/cst816.c`
    implements only the CST816 touch protocol.
  - `clk/sunxi-ccu.c`, `pinctrl/pinctrl-sunxi.c` and `tty/serial/uart-dw.c`: CCU gates/resets, pins
    and GPIO, and the 8250-compatible UART.
  - `mmc/`: `host/sunxi-mmc.*` covers only the SMHC0 controller (clock, bus width, voltage switch,
    command engine and IDMAC, sampling points, reset recovery, card-detect pin); `core/sd.c` covers
    only the SD protocol (identification, CMD11 1.8 V, speed modes, CMD19 tuning, block I/O with one
    retry) and never touches controller registers; `card/mmcblk.c` provides the `mmcblk0` gendisk,
    block operations, the hot-plug task and the `mmc` shell command.
- **Layering rule**: protocol and device drivers do not access controller registers; they call the
  controller API declared next to the driver. Board facts and pins live in arrays in
  `software/board/cardsight_board.c`.
- `software/src/arch/` and `software/src/kernel/`: boot, traps, PLIC, CLINT, FreeRTOS glue,
  initcall, console, shell and logging. Every initialisation is registered with
  `INITCALL(level, priority, fn)`.
- `software/src/block/` and `software/src/fs/`: the generic block layer, the VFS and the
  FatFs/lwext4 glue. The block layer does not depend on board headers and compiles in the host
  tests.
- `software/apps/cardsight/` is split by capability: `app/` holds the state model, the request
  worker, card/file/format/layout operations and shell diagnostics; `storage/lazy/` is the
  lazy-index exFAT engine used for USB export; `storage/eager/` is the full-index engine kept for
  diagnostics; `ui/` has the theme, wallpaper, icons and screenshot at the top, `ui/core/` for the
  shell/tabs/dialogs/banner, `ui/widgets/` for widget constructors, and `ui/pages/` for the five
  pages. Private headers travel with their module; the public headers (`storage_proxy.h`,
  `proxy_lazy.h`, `ui.h`, `app.h`) stay stable.
- `software/include/`: the public API (`vfs.h`, `shell.h`, `hal/*`, `usb/mass_storage.h`,
  `gui/lvgl_port.h`, `drm/sunxi_display.h`).

## Adding a board

1. Add a board C file under `software/board/` (or edit the existing one): UART and SD controller
   bases/gates/pins, panel and touch pins, the USB buffer address, and so on. Every field is a
   constant that can be checked at build time.
2. Update `ORIGIN/LENGTH` in the linker script (this board uses PSRAM `0x12000000`, 8 MiB) and
   confirm the `__heap_start/__heap_end` margin.
3. Select the matching SyterKit board configuration in `software/cmake/syterkit.cmake` to build the
   eFEX images; the entry address must match the address used by `xfel write`.
4. If a controller has a different register layout, change the implementation under
   `software/src/drivers/<subsystem>/`; do not reintroduce `platform/common/` or a per-chip dispatch
   table.

## Adding a peripheral

- Put a new peripheral under `software/src/drivers/<subsystem>/`, with `-` in file names, for
  example `spi/spi-sunxi.c` and `spi-sunxi.h`, or `display/panel-st7789.c` and `display/display.c`.
  The configuration struct and the `extern` instance declaration belong in the driver's own header;
  the concrete instance (base, pins, clock gate, frequency) goes into
  `software/board/cardsight_board.c`.
- Register initialisation with `INITCALL`; anything that needs the scheduler, such as the LVGL task,
  goes at level 3.
- Use `readl`/`writel` (value first) and `setbits32`/`clrbits32`/`clrsetbits32` from `<hal/io.h>`;
  do not add a private MMIO wrapper.
- Do not use `ktime_get_ms()` in a register polling loop (64-bit division). Use a deadline based on
  `cs_clint_ticks()` and `cs_clint_frequency()`; sleep with `msleep`/`cs_udelay`.
- For DMA, call `dcache_clean_range()` before sending and `dcache_invalidate_range()` after
  receiving (`software/include/hal/cache.h`, T-Head CMO instructions, 64-byte lines). Align buffers
  to 64 bytes; the MMC driver has a PSRAM bounce-buffer precedent for unaligned block transfers.
- If a driver can be called from several tasks, serialise it inside the driver with a mutex (see
  `mmc_lock` in `mmc/card/mmcblk.c`).

## Build switches

| Variable | Effect |
| --- | --- |
| `CARDSIGHT_WITH_APP` | Build the application and LVGL (default ON) |
| `CARDSIGHT_HOST_TESTS` | Build the host tests |
| `CARDSIGHT_BUILD_EFEX` | Build the SyterKit eFEX images (default ON) |
| `CARDSIGHT_WERROR` | Treat warnings in our own code as errors (default OFF) |
| `XUANTIE_TOOLCHAIN` | Cross-toolchain prefix directory, supplied through the environment or `-D` |

## Host tests and the simulator

`software/tests/host/` compiles the real block layer, VFS, filesystems and storage proxy with the
host compiler, using RAM disks and stubs (`ktime`, logging, mutexes). Add regression cases there
when changing behaviour; register-dependent code must stay out of the host target.

UI changes can be checked with the `ui_sim` PPM renderer or by touching the WebAssembly simulator in
`docs/`. Both compile the same `ui/` sources, so there is no second UI to maintain. When adding a
page, update the `tiles` count in `ui_private.h`, the build order in `ui_core.c` and the tab bar.
