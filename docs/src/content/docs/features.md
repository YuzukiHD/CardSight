---
title: Features
description: Reading cards, partitions, formatting, file browsing, USB export, and the UI states behind them.
---

## Reading cards and partitions

The TF card goes through SMHC0: CMD0/CMD8/ACMD41 identification, CMD11 to 1.8 V when the card supports it, ACMD6 for four-bit mode, CMD19 sampling-point tuning, then the fastest mode the card offers (SDR104, SDR50 or SDR25). A failed tuning resets the controller, retunes, and retries once.

Once identification succeeds, the driver publishes the block device `mmcblk0`; the block layer parses MBR or GPT and publishes `mmcblk0p1`, `mmcblk0p2`, and so on. After probing each partition's filesystem, the UI shows its type, label, capacity and writability. A partition whose filesystem cannot be probed shows "unrecognized" and does not prevent the other partitions from being used.

Card removal is detected on the PF6 card-detect pin, polled every 500 ms. When the card disappears, the disk is removed and the application worker stops USB and the proxy; the UI falls back to the empty state.

## Formatting

A single partition can be formatted quickly or fully, with FAT32, exFAT or ext4. A full format wipes the partition before creating the filesystem and takes longer.

The whole-card layout editor rewrites the partition table (MBR or GPT) and creates partitions with the chosen filesystem. It clears the existing table, so confirmation is a long press rather than a tap. If the partition is too small, FatFs refuses to create the filesystem and the UI reports that the capacity is insufficient.

Formatting runs on the application worker; the UI only submits the request and displays progress. Formatting is refused while USB is running (`-EBUSY`), so the host and the board cannot write the same card at once.

## File browsing

The Files page lists partitions first, then descends one directory at a time. The worker reads one directory per request and keeps at most `APP_FILE_ENTRIES` (64) entries; larger directories report the total and mark the listing truncated. Entries are sorted with directories first, then names in case-insensitive order. Long names are ellipsised instead of wrapped. ext4 partitions go through lwext4 and FAT/exFAT partitions through FatFs; both paths use the VFS, so the UI does not distinguish between filesystems.

## Direct USB

The Direct page exports the whole card as a USB mass-storage device. The host sees the real partition table and filesystems, and writes land directly on the card. While connected, the page shows read and write rates computed once per second from the USB counters.

## Storage proxy

Proxy mode selects one or more partitions and synthesises an exFAT volume (block device `cardview1`) for the host. Each partition becomes a folder; the original directory tree, names and file contents are indexed on demand. Host writes enter a bounded overlay and pending table first, then are written back to the source filesystem. See [Storage proxy and USB](/CardSight/proxy/) for the details.

The Proxy and Direct pages share the start/safe-stop button. While running, the button reads "Stop safely" and switching pages does not interrupt the export.

## UI states

All five pages share one state model:

| State | UI |
| --- | --- |
| Identifying the card | A spinner and "Identifying card" over the current page; the page temporarily has no card |
| Read/write failure | A bottom sheet with the failed operation, the controller's original error, the error code and the path |
| Busy | The request is running; the tab bar does not switch pages and buttons are disabled |
| Card removed | USB and the proxy stop, the file browser leaves its detail view, and the empty state returns |
| Transferring | Read/write rates and the USB state update once per second without rebuilding the page |

## Serial diagnostics

A shell remains available outside the UI: `blk`, `mmc`, `mount`, `ls`, `cat`, `lazyproxy`, `cardsight` and others. It is used when the board has no display, when the UI misbehaves, or in production testing. The complete list is in [Shell](/CardSight/shell/).
