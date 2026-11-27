---
title: Shell
description: "Block device, VFS, MMC, USB, proxy and UI diagnostics under the cardsight> prompt."
---

Open the UART at 115200 8N1 and the prompt is `cardsight>`. The table below comes from the
`SHELL_CMD` registrations in each module; a wrong argument prints the command's usage text.

## Block devices

| Command | Effect |
| --- | --- |
| `blk` | List block devices, capacities and read-only flags |
| `blkread <dev> <sector>` | Read one 512-byte sector and print it as hex |
| `blkbench <dev> <sector> <MiB>` | Sequential read starting at a sector, reporting throughput |

## SD card

| Command | Effect |
| --- | --- |
| `mmc` | Card information: capacity, mode, clock, bus width, tuning state |
| `mmc eject` | Force the card-detect state to "absent"; the card stays powered in the slot |
| `mmc insert` | Identify the current card again |
| `mmc soak [n]` | Repeat remove/identify n times and count failures |

`mmc eject` exercises the hot-plug path; it is not a physical removal. A real removal cuts power,
and reinsertion goes through 3.3 V → CMD11 → SDR104 again. After a simulated removal the card may
stop at SDR25.

## Filesystems

| Command | Effect |
| --- | --- |
| `mount [blk target [fstype]]` | Mount a partition; with no arguments, list mount points |
| `umount <target>` | Unmount |
| `ls [path]` | List a directory |
| `cat <path>` | Print a file |
| `df <path>` | Volume capacity and free space |
| `write <path> <text>` | Write a text file |
| `rm <path>` / `rmdir <path>` | Remove a file / an empty directory |

## USB and the storage proxy

| Command | Effect |
| --- | --- |
| `lazyproxy start <disk> [mask]` | Start the lazy-index exFAT view `cardview1`; mask is a partition bitmask |
| `lazyproxy stop` | Stop the view |
| `lazyproxy usb on\|rw\|off` | Export the view as a read-only or read-write USB device, or stop USB |
| `lazyproxy` / `lazyproxy status` | Geometry, reservations, directory/cache usage, overlay, I/O counters and last error |
| `storageproxy start <disk> [mask]` | Start the full-index view `cardview0` (diagnostics) |
| `storageproxy stop` / `status` / `count` / `reset` | Control and statistics for the full-index view |
| `cardsight usb on` | Start the proxy view and export it over USB (full-index engine) |
| `cardsight usb off` | Stop USB and the proxy |
| `cardsight usb disk` | Export the whole disk read-only, for host-side checks of the real table |
| `cardsight usb-test` | Read-only proxy and USB start/stop lifecycle test |

The Proxy page in the UI uses the lazy-index engine, which is the same code as `lazyproxy`.
`storageproxy` and `cardsight usb` are diagnostics built on the full-index engine; both walk the
partition trees at start-up.

## Display and UI

| Command | Effect |
| --- | --- |
| `display color <rgb565>` | Fill the panel, checking the display and SPI path |
| `display test` | Built-in test pattern |
| `panel` | State of the second hart's pixel pusher |
| `lvgl` | Refresh count, pixel count and elapsed time |
| `lvgl reset` | Clear the statistics |
| `lvgl bench` | Wire-speed and redraw benchmark |
| `cardsight page <direct\|proxy\|disk\|info\|layout>` | Switch the UI page |
| `cardsight radius [px]` | Open the corner-radius calibration overlay |
| `cardsight ui-test` | Walk the object tree and run the UI self-test |
| `cardsight memtest [n]` | Rebuild the UI n times and compare heap usage |
| `cardsight memcheck` | Check the list fade guards and the LVGL draw-buffer guard word |
| `screenshot` | Dump the current screen as RLE/base64 |

## System

| Command | Effect |
| --- | --- |
| `help` | List the commands |
| `heap` | Kernel heap and C library heap usage |
| `stack` | Task stack high-water marks |
| `uptime` | Milliseconds since boot |
| `cache` | XuanTie cache registers and a bandwidth check |

## Common sequences

Check the card and its partitions:

```text
mmc
blk
mount mmcblk0p2 /mnt/fat fat
ls /mnt/fat
```

Export one partition through the proxy:

```text
lazyproxy start mmcblk0 0x2
lazyproxy usb rw
```

Measure sequential reads:

```text
blkbench mmcblk0 2048 64
```

The third argument to `blkbench` is MiB, not sectors. Reads into on-chip SRAM measure about
62 MiB/s; reads into PSRAM measure about 9.6 MiB/s, limited by the PSRAM write path.
