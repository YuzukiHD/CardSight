---
title: Hardware
description: The Yuzuki CardSight SoC, memory map, clocks, SD, display, touch and USB wiring, with measurements.
---

A Yuzuki CardSight board: Allwinner V861/V881 (`sun252iw1`) with a C907 RISC-V core (RV32, `rv32imafdc_zicsr_zifencei`, ilp32d) and an E907 core that initialises memory at power-up. All hardware facts live in constant arrays in `software/board/cardsight_board.c`; there is no device tree.

## Memory

| Region | Address | Size | Use |
| --- | --- | --- | --- |
| On-chip SRAM | `0x00100000` | 128 KiB | The low 64 KiB are reserved for the USB MSC transfer buffer; the eFEX also runs here |
| PSRAM | `0x12000000` | 8 MiB | Image, `.bss`, heaps; the image is linked at `0x12000000` |
| DRAM | `0x40000000` | — | Not initialised by this boot flow and unused |

The kernel heap is FreeRTOS `heap_4` at the end of PSRAM (2 MiB); the newlib heap runs from the end of the image to the end of PSRAM (about 5 MiB). The linker script fails the build if the image does not fit.

## Clocks and interrupts

- Clock control is at CCU `0x02001000`. Bus gates and resets are written directly as register plus bit number: UART0 `0x090c` (gate 0), SMHC0 `0x084c` (gate 0, reset 16), SPI1 `0x096c` (gate 1, reset 17), TWI0 `0x091c` (gate 0, reset 16), USB PHY `0x0a70` (gate 31, reset 30) and USB OTG `0x0a8c` (gate 8, reset 24).
- Timers: CLINT `0x34000000`, mtime `+0xbff8` at 24 MHz; the FreeRTOS machine timer compare register is `+0x4000`. `ktime_get_us()` reads mtime directly.
- Interrupts: PLIC `0x30000000`, hart 0 machine-mode context (see `software/src/arch/plic.c`). The numbers in the board table are PLIC source numbers; the CLIC numbers in the SoC manual are 16 higher (UART0 CLIC 18 → PLIC 2).

## Peripherals

| Peripheral | Base | IRQ | Notes |
| --- | --- | --- | --- |
| UART0 | `0x02500000` | 2 | PH13/PH14, mux 6, 115200; polled TX, interrupt-driven RX |
| SMHC0 (SD) | `0x04020000` | — | PF2 clock, PF3 CMD, PF0/1/4/5 D0-D3, mux 2, CD=PF6 active low; PIO commands plus IDMAC data |
| SPI1 (panel) | `0x04026000` | — | PD1/2/3 mux 6; DC=PD4, RST=PD5; 60 MHz (300M/5) |
| TWI0 (touch) | `0x02502000` | — | PE4/PE5 mux 8, 400 kHz; CST816 at 0x15, reset PE2 |
| USB OTG | `0x04100000` | 29 | PHY glue `0x04100400`, HCI PHY `0x04101800`; device mode |

### TF card

The 1.8 V signalling rail is switched by the PMIC's port-F power switch: GPIO `+0x390` selects (1=3.3 V, 0=1.8 V) and bit 5 of `+0x38c` reports the state. A card switched to 1.8 V stays there until power is removed; a cold boot identifies at 3.3 V first and retries at 1.8 V on failure.

Initialisation order: CMD0/CMD8/ACMD41 (with S18R) → CMD11 to 1.8 V if accepted → CMD2/3/9/7 → ACMD6 for four-bit → SCR/CMD6 → fastest mode. The board caps the bus at SDR104 200 MHz; SDR104 or SDR50 at 100 MHz and SDR25 at 50 MHz are also available.

**Do not use SMHC module-clock mux 2**: on this board it cannot make the card respond correctly and tuning at 150 MHz fails completely. 200 MHz comes directly from mux 1 (400 MHz PLL) with M=1.

Sampling points are found with CMD19 tuning, 3 phases × 64 sample delays. When the window covers the whole grid, the interface default (phase 0, delay 0) is kept. After a failed tuning the driver resets the controller, retunes and retries once.

Hot-plug is handled by a low-priority task polling CD (PF6) every 500 ms. On insertion it identifies the card and publishes `mmcblk0`; on removal it removes the disk (`del_gendisk_force`). `mmc eject/insert/soak` are the driver's own test entry points: `eject` only forces the card-detect state to "absent" while the card stays powered in the slot, so re-identification may stop at SDR25. A real removal cuts power, and reinsertion goes through 3.3 V → CMD11 → SDR104 again. `mmc soak 20` runs 20 remove/identify cycles on the board with zero failures.

Measurements (SK32G 32 GB, SDR104 200 MHz four-bit):

| Path | Throughput |
| --- | --- |
| Into on-chip SRAM | 62 MiB/s (controller/card limit) |
| Into PSRAM | about 9.6 MiB/s |
| 400 kHz, DMA | about 97% efficiency (189 KiB/s) |

The PSRAM bottleneck is the write path itself: a CPU `memcpy` also reaches only about 10 MB/s, so the SD controller is not the limit.

### Display and touch

ST7789-compatible panel, 240×284, RGB565, SPI mode 0, high byte first (`pixels_swapped`). MADCTL `0x48` (mirror-x plus BGR). The init table is in `software/src/drivers/display/panel-st7789.c`.

DC/RST are ordinary GPIO outputs (mux 1), and the SPI controller needs the DHB/DDB/SDM bits set; missing any of them gives a black screen or wrong colours (both a fully black panel and swapped green/red have been seen).

CST816D capacitive touch on polled I2C (no interrupt or worker thread); the 6-byte report is read from register `0x01`, and X is mirrored to match the panel. The LVGL input device reads one frame per callback.

LVGL 9.6 uses a 40-line partial buffer and flushes directly to the panel; the `lvgl` command prints refresh count, pixel count and elapsed time.

### USB

MUSB device controller with CherryUSB (device stack plus MSC class). PHY glue registers are `ISCR 0x00`, `PHYCTRL 0x10` and `PHYSEL 0x20` relative to `0x04100400`; `VEND0 0x43` writes the PIO bit.

The device role does not use the ID/VBUS comparator: it forces a B device with VBUS valid.

The transfer buffer is the reserved 64 KiB SRAM block at `0x00100000`, obtained by the MSC class through `CONFIG_USBDEV_MSC_EXTERNAL_BUFFER`; it does not use PSRAM.

Windows 10/11 enumerates the device as `VID_1D6B/PID_0105` and mounts the exFAT volume, reading directories and files. With the proxy view the host sees the synthesised volume; with direct whole-disk export it sees the real partition table.

## Known limits

| Limit | Cause |
| --- | --- |
| 200 MHz maximum, not 150/208 MHz | SMHC module-clock mux 2 is unusable |
| PSRAM writes at about 10 MB/s | The PSRAM write path itself; CPU copies are equally slow |
| Unflushed data is lost on removal | The card is already unpowered; the proxy can only report `flush failed` |
