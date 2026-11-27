---
title: Quick start
description: "Build the firmware, load it through FEL, and reach the first boot prompt."
---

This page is the shortest path from a checkout to a running board. The build and flashing pages
hold the details; the live simulator is available if you do not have the board yet.

## Prerequisites

- A Yuzuki CardSight board and a USB cable for its OTG port.
- A TF card for testing.
- A USB-UART adapter on UART0: PH13/PH14, 115200 8N1.
- [`xfel`](https://github.com/xboot/xfel) for the FEL download.
- The XuanTie-900 GCC elf newlib toolchain, exposed through `XUANTIE_TOOLCHAIN`.
- Git, CMake 3.20 or newer, and a C compiler for the host tests.
- For the documentation site only: Node.js, Emscripten and Doxygen.

## Build

```sh
git clone --recurse-submodules https://github.com/YuzukiHD/CardSight.git
cd CardSight
export XUANTIE_TOOLCHAIN=/path/to/Xuantie-900-gcc-elf-newlib-x86_64-V3.2.0
cd software
cmake --preset riscv
cmake --build build/riscv
```

The images are written to `software/build/riscv/images/`:

| File | Purpose |
| --- | --- |
| `init_psram_efex.bin` | Initialises PSRAM on the E907 and returns to FEL |
| `cardsight.bin` | C907 application image, linked at `0x12000000` |
| `c907_boot_efex.bin` | Releases the C907 at entry `0x12000000` |

No prebuilt images are published. If you only want to inspect the UI, use the
[live simulator](../guide/simulator.md) instead.

## Flash

With the board in FEL mode:

```sh
xfel write 0x00100000 software/build/riscv/images/init_psram_efex.bin
xfel exec  0x00100000
xfel write 0x12000000 software/build/riscv/images/cardsight.bin
xfel write 0x00100000 software/build/riscv/images/c907_boot_efex.bin
xfel exec  0x00100000
```

The addresses, the eFEX split and the memory map are explained in
[Flashing and boot](flashing.md).

## First boot

Open the UART at 115200 8N1. The firmware starts the console, then the application, and the UI is
drawn on the panel. The prompt is:

```text
cardsight>
```

Useful first checks:

```text
cardsight> mmc
cardsight> blk
```

`mmc` reports the card identity, mode and tuning state. `blk` lists `mmcblk0` and its partitions.
Insert a TF card if the slot is empty; the hot-plug task identifies it and publishes the disk.

The five-page UI is described in [On-screen UI](../guide/ui.md). Use **Stop safely** before removing
the card while USB or the storage proxy is running.

:::caution
A RAM/FEL boot loses all memory contents on power loss. Data on the TF card is not affected, but
any unflushed proxy writes are lost if the card is removed. See
[USB export](../guide/usb-export.md).
:::

## Next steps

- [On-screen UI](../guide/ui.md): the five pages and their controls.
- [Card and files](../guide/card-files.md): partition and filesystem operations.
- [USB export](../guide/usb-export.md): export the card or a proxy view to a host.
- [Shell commands](../reference/cli.md): the complete serial command reference.
- [Build](../development/build.md) and [Testing](../development/testing.md): contributor workflows.
