---
title: Flashing and boot
description: "The three-stage FEL load, image addresses, memory map and UART setup."
---

The board boots from RAM through FEL during development. SPI NOR is not written, flash is not
erased, and the boot path does not modify the TF card.

## Three-step load

```sh
# 1. E907 initialises PSRAM and returns to FEL
xfel write 0x00100000 software/build/riscv/images/init_psram_efex.bin
xfel exec  0x00100000

# 2. C907 application image, linked at 0x12000000
xfel write 0x12000000 software/build/riscv/images/cardsight.bin

# 3. E907 releases the C907 at entry 0x12000000
xfel write 0x00100000 software/build/riscv/images/c907_boot_efex.bin
xfel exec  0x00100000
```

The two eFEX images come from the `yuzuki-cardsight` board configuration in the SyterKit submodule.
`init_psram_efex.bin` brings up the 8 MiB PSRAM at SQPI 100 MHz and returns to FEL.
`c907_boot_efex.bin` releases the C907 and starts it at the PSRAM alias `0x12000000`.

## Memory map

| Region | Address | Size | Use |
| --- | --- | --- | --- |
| On-chip SRAM | `0x00100000` | 128 KiB | Low 64 KiB: USB MSC transfer buffer. Second 64 KiB: LVGL draw buffer. Mailbox and second-hart stack at the top. |
| PSRAM | `0x12000000` | 8 MiB | Image, `.bss`, kernel heap and newlib heap |
| DRAM | `0x40000000` | — | Fallback entry in the SyterKit non-PSRAM configuration only; the board configuration uses PSRAM |

The application image is linked for PSRAM and fails the build if it does not fit. The kernel heap is
4 MiB; the newlib heap is the region between `__heap_start` and `__heap_end` after the image.

## Console

UART0 is on PH13/PH14, mux 6, at 115200 8N1. TX is polled and RX is interrupt-driven. The log
format is `[seconds.microseconds][level] module: message`, with a colour per level.

After the three steps the prompt is `cardsight>`. The UI starts at the same time and is operated by
touch.

## Cautions

:::caution
Only one board may be in FEL at a time. Do not issue FEL commands to another board while a write is
in progress, and do not remove power during `xfel write`.
:::

:::caution
Do not use this flow to write SPI NOR, modify flash partitions or replace a bootloader. The
repository does not contain a bootloader writer.
:::

The eFEX address is fixed at `0x00100000` and the application entry at `0x12000000`. Changing the
link address means changing the build and the flashing commands together.
