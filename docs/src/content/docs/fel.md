---
title: Flashing and boot
description: Load the PSRAM initialiser, the application image and the C907 boot stub through xfel/FEL without writing flash.
---

During development the board boots entirely from RAM through FEL. SPI NOR is never written, flash is never erased, and the boot path does not modify the TF card. On power-up the BROM waits for FEL; only one board may be in FEL at a time.

## Three-step load

```sh
# 1. The E907 initialises PSRAM and returns to FEL
xfel write 0x00100000 software/build/riscv/images/init_psram_efex.bin
xfel exec  0x00100000

# 2. C907 application image, linked at 0x12000000
xfel write 0x12000000 software/build/riscv/images/cardsight.bin

# 3. E907 releases the C907 at entry 0x12000000
xfel write 0x00100000 software/build/riscv/images/c907_boot_efex.bin
xfel exec  0x00100000
```

The two eFEX images come from the `yuzuki-cardsight` board configuration in SyterKit:

- `init_psram_efex.bin` brings the 8 MiB PSRAM up at SQPI 100 MHz on the E907, prints `PSRAM ready; returning to FEL for image download at 12000000` on the UART, and returns to FEL.
- `c907_boot_efex.bin` releases the C907 and jumps only after the UART has drained the log.

The console is UART0 (PH13/PH14), 115200 8N1. The log format matches SyterKit: `[seconds.microseconds][level] module: message`, with a colour per level.

## Addresses and memory

| Region | Address | Use |
| --- | --- | --- |
| On-chip SRAM | `0x00100000` | eFEX runtime area; the low 64 KiB are reserved for the USB MSC transfer buffer |
| PSRAM | `0x12000000` | Application image, `.bss` and heaps; 8 MiB |
| DRAM | `0x40000000` | Not initialised by this boot flow and unused |

The low 64 KiB of SRAM are handed to the USB transfer buffer only after the eFEX has jumped away.

## After power-up

The UART shows the `cardsight>` prompt while the UI is drawn on the panel. Without a card, all five pages are reachable but the Direct and Proxy buttons are disabled. Inserting a card makes the driver identify it and publish `mmcblk0`; the UI then refreshes the partition information.

A RAM/FEL boot loses all memory contents; a power cycle means running the three steps again. Data on the TF card is not affected.

## Notes

- Do not issue FEL commands to another board, and do not cut power, while `xfel write` is filling PSRAM.
- Do not use this flow to write SPI NOR or modify flash partitions; the repository does not contain a bootloader writer.
- The eFEX address is fixed at `0x00100000` and the application entry at `0x12000000`; changing the link address means changing all three places together.
