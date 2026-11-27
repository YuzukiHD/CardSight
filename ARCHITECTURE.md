# Architecture

CardSight is a TF card proxy system: it reads a card, manages partitions and filesystems, and
exports the card or a synthesized view over USB. The full architecture, boot chain, task model and
module map are in the documentation:

- <https://yuzukihd.github.io/CardSight/development/architecture/>
- <https://yuzukihd.github.io/CardSight/development/storage-proxy/>
- <https://yuzukihd.github.io/CardSight/reference/api/>

## Invariants

- Board facts are C constants and structures in `software/board/`; there is no device tree and no
  per-chip dispatch table.
- Controller drivers and protocol drivers are separate. A protocol driver calls the controller API
  declared next to it and never touches registers.
- Every initialisation is registered with `INITCALL(level, priority, function)`. Levels 0 and 1 run
  on the boot stack; levels 2 and 3 run in the boot task.
- The UI never touches the card. It reads a state snapshot and posts one request at a time to the
  worker, which owns the card, filesystems and USB.
- The block layer, VFS, FatFs, lwext4 and both proxy engines compile on the host and are tested
  against RAM disks.
- The WebAssembly simulator compiles the same UI sources as the firmware; there is no second UI.

## Source map

| Path | Responsibility |
| --- | --- |
| `software/board/` | Board constants and linker script |
| `software/include/` | Public API |
| `software/src/arch/` | Boot, traps, PLIC, CLINT, cache |
| `software/src/kernel/` | FreeRTOS glue, initcall, console, shell, logging |
| `software/src/drivers/` | UART, MMC/SD, SPI, I2C, panel, touch, CCU, pinctrl |
| `software/src/block/` | gendisk, block devices, partition parsing, layout writing |
| `software/src/fs/` | VFS, FatFs and lwext4 glue |
| `software/src/gui/` | LVGL port |
| `software/src/usb/` | MUSB and CherryUSB MSC |
| `software/apps/cardsight/` | Application, storage proxy, UI, fonts, assets |
| `software/tests/host/` | Host tests and `ui_sim` |
| `software/tests/wasm/` | Browser simulator |
