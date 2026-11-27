---
title: Shell commands
description: "The serial command reference for block devices, filesystems, MMC, USB, the storage proxy and the UI."
---

Open the UART at 115200 8N1 and the prompt is `cardsight>`. Commands are registered by name; a
wrong argument prints the command's usage text.

The shell has a 128-byte line buffer, accepts at most eight arguments, and blocks the prompt while a
command runs. It has no history or completion. `blkbench` is omitted from host-test builds.

## System

| Command | Effect |
| --- | --- |
| `help` | List commands and their help text |
| `heap` | FreeRTOS heap and C library heap usage |
| `stack` | Task stack high-water marks |
| `uptime` | Milliseconds since boot |
| `cache` | XuanTie cache registers and a bandwidth check; also `cache readline`, `cache psram <divider>` and `cache mhint <hex>` |

## Block devices

| Command | Effect |
| --- | --- |
| `blk` | List disks and partitions with size, offset, type and mount state |
| `blkread <dev> <sector>` | Read one 512-byte sector and print the first 64 bytes as hex |
| `blkbench <dev> <sector> <MiB>` | Sequential read in 256 KiB chunks; the last argument is MiB |

## TF card and hot-plug

| Command | Effect |
| --- | --- |
| `mmc` | Card identity, capacity, mode, clock, bus width, tuning and DMA counters |
| `mmc eject` | Force the card-detect state to absent; the card stays powered in the slot |
| `mmc insert` | Clear the forced absence and wait for identification |
| `mmc soak [n]` | Repeat remove/identify cycles; default 20, zero is invalid |

`mmc eject` is a driver test path, not a physical removal. A real removal cuts power and a
reinsertion goes through the normal voltage and tuning sequence.

## Filesystems

| Command | Effect |
| --- | --- |
| `mount [blk target [fstype]]` | List mounts with no arguments, otherwise mount a block device |
| `umount <target>` | Unmount |
| `ls [path]` | List a directory; default `/` |
| `cat <path>` | Print a file in 256-byte chunks |
| `df <path>` | Total and free space |
| `write <path> <text>` | Create or truncate a text file |
| `rm <path>` | Remove a file |
| `rmdir <path>` | Remove an empty directory |

## Display and UI

| Command | Effect |
| --- | --- |
| `display color <rgb565>` | Fill the panel with one RGB565 colour, expanded to the panel's RGB888 path |
| `display test` | Cycle red, green, blue, white and black |
| `panel` | State of the second hart's pixel pusher |
| `lvgl` | Display flush statistics |
| `lvgl reset` | Reset the statistics |
| `lvgl bench` | Wire and redraw benchmark |
| `screenshot` | Dump the screen as `SHOT <w> <h> <bytes>`, base64 payload, then `END` |
| `cardsight page <direct\|proxy\|disk\|info\|layout>` | Switch the UI page |
| `cardsight radius [px]` | Open the corner-radius calibration overlay |
| `cardsight ui-test` | Walk the pages and the layout editor and run the UI self-test |
| `cardsight memtest [n]` | Rebuild the UI n times (default 5, 1..50) and compare heap usage |
| `cardsight memcheck` | Check the list fade guards and the LVGL draw-buffer guard word |

`cardsight page` accepts `direct`, `proxy`, `disk`, `info` and `layout`. The Files page has no shell
subcommand. `layout` opens the layout editor.

## USB and storage proxy

| Command | Effect |
| --- | --- |
| `lazyproxy start <disk> [mask]` | Start the lazy-index exFAT view `cardview1`; a missing mask selects every present partition |
| `lazyproxy stop` | Stop the view |
| `lazyproxy usb on` | Export the view as a read-only USB device |
| `lazyproxy usb rw` | Export the view as a read-write USB device |
| `lazyproxy usb off` | Stop the USB device |
| `lazyproxy` / `lazyproxy status` | Geometry, reservations, cache, overlay, pending/staging state, I/O counters and last error |
| `storageproxy start <disk> [mask]` | Start the full-index view `cardview0` |
| `storageproxy stop` / `status` / `reset` | Stop the view, print statistics, clear counters |
| `storageproxy count <disk> [mask] [depth]` | Walk the source trees read-only and count entries |
| `cardsight usb on` | Start the full-index proxy and export it over USB |
| `cardsight usb off` | Stop USB and the proxy |
| `cardsight usb disk` | Export the whole disk read-only for host-side checks |
| `cardsight usb-test` | Run the read-only proxy and USB start/stop lifecycle test |

`lazyproxy` and `cardsight usb` use different engines: `cardview1` indexes on demand, while
`cardview0` walks the source tree at start-up. The UI proxy page uses the lazy engine. See
[USB export](../guide/usb-export.md) and
[Storage proxy internals](../development/storage-proxy.md).

:::note
`cardsight usb disk` leaves the forced read-only flag set for the next USB start. Stop USB and
start the proxy read-write, or reboot, before a read-write direct export.
:::

## Examples

Inspect the card:

```text
cardsight> mmc
cardsight> blk
```

Mount a partition and list it:

```text
cardsight> mount mmcblk0p2 /mnt/fat fat
cardsight> ls /mnt/fat
cardsight> df /mnt/fat
```

Export one partition through the lazy proxy:

```text
cardsight> lazyproxy start mmcblk0 0x2
cardsight> lazyproxy usb rw
cardsight> lazyproxy status
cardsight> lazyproxy usb off
cardsight> lazyproxy stop
```

Read a block device sequentially:

```text
cardsight> blkbench mmcblk0 2048 16
```
