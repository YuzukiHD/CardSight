---
title: Hardware
description: "The CardSight SoC, memory map, clocks, peripherals, TF-card interface, panel, touch and USB."
---

The board is an Allwinner V861/V881 (`sun252iw1`) with a XuanTie C907 RV32 core and an E907 boot
core. Board constants live in `software/board/cardsight_board.c`; register-level details that belong
to a controller live in that controller's driver. There is no device tree.

## Memory map

| Region | Address | Size | Use |
| --- | --- | --- | --- |
| On-chip SRAM | `0x00100000` | 128 KiB | Low 64 KiB: USB MSC transfer buffer. Second 64 KiB: LVGL draw buffer. Mailbox at `0x0011F100`; second-hart stack top at `0x00120000`. |
| PSRAM | `0x12000000` | 8 MiB | Image, `.bss`, kernel heap and newlib heap |
| DRAM | `0x40000000` | — | Only the SyterKit non-PSRAM fallback entry; the board configuration boots from PSRAM |

The application is linked for PSRAM at `0x12000000`. The FreeRTOS `heap_4` is 4 MiB. The newlib heap
is the region between `__heap_start` and `__heap_end`; the boot banner prints the image and heap
sizes, and the link map records the region.

## Clocks and interrupts

- CCU at `0x02001000`; GPIO at `0x02000000`.
- CLINT at `0x34000000`, timer 24 MHz, mtime `+0xbff8`, mtimecmp `+0x4000`.
- PLIC at `0x30000000`, hart 0 machine-mode context, 150 sources. Board tables use PLIC source
  numbers; the CLIC numbers in the SoC manual are 16 higher.

## Peripherals

| Peripheral | Base | IRQ | Pins and notes |
| --- | --- | --- | --- |
| UART0 | `0x02500000` | 2 | PH13/PH14, mux 6, 115200; polled TX, interrupt RX |
| SMHC0 | `0x04020000` | — | PF2 clock, PF3 CMD, PF0/1/4/5 DAT0-3, mux 2; PF6 card detect, active low; PIO commands plus IDMAC |
| SPI1 | `0x04026000` | — | PD1/2/3 mux 6; DC on PD4, RST on PD5; 75 MHz |
| TWI0 | `0x02502000` | — | PE4/PE5 mux 8, 400 kHz; CST816 at `0x15`, reset on PE2 |
| USB OTG | `0x04100000` | 29 | MUSB device; PHY glue at `0x04100400`, HCI PHY at `0x04101800` |

## TF card

The card interface supports SDR104 at 200 MHz and 100 MHz, SDR50 at 100 MHz, and SDR25 at 50 MHz,
with four-bit ACMD6 transfers. CMD19 tuning selects the sampling point; a failed tuning resets the
controller, retunes and retries once. CMD11 switches the card to 1.8 V when it supports the voltage.

The card-detect input is PF6. A hot-plug task debounces it (five samples at 5 ms, at least three
present), identifies the card and publishes `mmcblk0`, then removes the disk on ejection. The poll
period starts at 500 ms and backs off to 5 s while identification fails.

:::note
The driver avoids the highest SMHC PLL path on this board; the supported timing modes above are the
ones it negotiates. `mmc eject` is a driver test that forces the detect state without removing
power; a real eject cuts power.
:::

## Display and touch

- ST7789-compatible panel, 240×284, SPI1 at 75 MHz.
- The panel init sets `MADCTL = 0x40` (mirror X) and `COLMOD = 0x06` (RGB666). The display publishes
  3 bytes per pixel in B, G, R order, with `pixels_swapped = false`.
- LVGL is configured for RGB888 and flushes through a 40-line partial buffer. The draw buffer is the
  second 64 KiB block of on-chip SRAM.
- `display color <rgb565>` takes an RGB565 value, expands it to RGB888 and truncates the channels to
  the panel's 6-bit input.
- DC and reset are plain GPIO outputs. The SPI controller must set the DHB/DDB/SDM bits; missing
  any of them gives a black panel or wrong colours.
- CST816D touch on I2C0: 6-byte report from register `0x01`, reset on PE2, X mirrored to match the
  panel. LVGL polls one report per input callback.

## USB

MUSB device controller with CherryUSB MSC:

- PHY glue registers `ISCR 0x00`, `PHYCTRL 0x10`, `PHYSEL 0x20` at `0x04100400`; `VEND0 0x43`
  controls the PIO bit.
- The device role is forced to a B device with VBUS valid.
- The 64 KiB transfer buffer is the low block of on-chip SRAM at `0x00100000`; PSRAM is not used.
- One LUN, high speed, MSC class, VID `0x1D6B`, PID `0x0105`.

## Known limits

- The highest SMHC PLL path is not used on this board; the driver stays with the supported timing
  modes.
- PSRAM is the slower memory path. The USB and display buffers use on-chip SRAM.
- Removing the TF card while the storage proxy has unflushed writes loses that data; the proxy
  reports a flush failure because the card is no longer powered.
- No performance figures are published as normative values. Measure on your own board with a
  documented method if you need them.
- Schematics, PCB and BOM are not published in this repository yet.

See [FAQ](../project/faq.md) for symptoms and checks, [Configuration](configuration.md) for
software limits, and [THIRD_PARTY.md](https://github.com/YuzukiHD/CardSight/blob/master/THIRD_PARTY.md)
for the board support libraries.
