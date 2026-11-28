---
title: Flashing and boot
description: "Load images into RAM through FEL, write a boot image to SPI NOR with xfel, and start the console."
---

There are two ways to run the firmware. The development flow copies images into SRAM and PSRAM with
`xfel write` and starts them with `xfel exec`; the contents disappear on power-off. The persistent
flow writes a boot image to SPI NOR with `xfel spinor`, and the boot ROM loads it on every reset.
Neither flow modifies the TF card.

## Load into RAM

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

## Flash SPI NOR

`xfel write` only fills memory. Persistent firmware is written to the SPI NOR with the `spinor`
commands:

```sh
xfel spinor                                 # detect the chip and its size
xfel spinor read  0x0 <length> nor-backup.bin
xfel spinor erase 0x0 <length>
xfel spinor write 0x0 <image>
xfel spinor read  0x0 <length> verify.bin
```

Back up the existing flash before erasing, and compare `verify.bin` with the written image before
resetting the board. The address and length depend on the board's boot layout: the three FEL images
above are linked for SRAM/PSRAM and are not a NOR boot image. A NOR image must include the boot
header the ROM expects.

:::caution
Erasing or writing the wrong range can leave the board unable to boot. Keep a full-chip backup and
confirm the image layout before writing.
:::

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

After the RAM load the prompt is `cardsight>`. The UI starts at the same time and is operated by
touch.

## Cautions

:::caution
Only one board may be in FEL at a time. Do not issue FEL commands to another board while a write is
in progress, and do not remove power during `xfel write` or `xfel spinor write`.
:::

The eFEX address is fixed at `0x00100000` and the application entry at `0x12000000`. Changing the
link address means changing the build and the load commands together.
