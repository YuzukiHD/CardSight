---
title: Storage proxy and USB
description: "Present several partitions as one exFAT volume over USB: view layout, write-back path, capacity and limits."
---

## Two engines

`software/apps/cardsight/storage/` contains two exFAT views:

| Engine | Block device | Used by |
| --- | --- | --- |
| `lazy/` | `cardview1` | The Proxy page and the `lazyproxy` command; indexes directories on demand and supports write-back |
| `eager/` | `cardview0` | The `storageproxy` and `cardsight usb` diagnostics; indexes everything at start-up |

Both engines share the partition information and selection bitmask in `storage_proxy.h`, but their
internals differ. The UI uses the lazy engine because it does not have to walk the whole card
before the export starts.

## The view

The view is a real exFAT volume: 24 boot sectors, a FAT, an allocation bitmap, an up-case table
and a root directory, with 4 KiB clusters. The root contains:

- One folder per selected source partition, named `partition1`, `partition2`, and so on; the number
  matches the source partition.
- Two read-only text files, `_LAYOUT.TXT` with the view geometry and reservations, and
  `_DIR_LIMIT.TXT` with the current directory-index limits. Both are generated on read and each
  occupies one reserved cluster.
- On the board, a `System Volume Information` folder on the first selected partition, which Windows
  expects when it mounts a writable volume. Host tests do not create it.

Below the partition folders is a mirror of the source directories. A directory is indexed the first
time the host reads it; the result stays in an LRU cache with fixed limits, and an evicted directory
is scanned again on the next read. Source files are mapped as contiguous cluster runs; no data is
copied.

## Write-back

Host writes do not land on the source files directly:

1. Directory entries, names and size changes go into an in-memory overlay.
2. Data enters a pending table, is coalesced into a staging file on a source partition, and
3. a flush copies the staged data back to the source filesystem by range, removes the staging file
   and updates the directory entries.

The host always sees a complete volume, never a source file that is half written. The cost is:

- The overlay, the pending table and the staging area all have limits. A write past a limit returns
  `-ENOSPC`, which the host sees as a full disk.
- Write-back happens on flush or stop. If the card is removed with unflushed data, the proxy reports
  `flush failed` and that data is lost.
- If every source partition is read-only, staging has nowhere to go and host writes fail.

The UI forces USB into read-write mode when it starts the proxy; `lazyproxy usb on` exports it
read-only for inspection. `storageproxy` marks the whole view read-only when any source partition
is read-only.

## Capacity

The view size is the sum of:

- per-partition reservations sized from the used space (ext4 gets one view cluster per 4 KiB of
  used data; FAT/exFAT take the larger of the view-cluster and source-cluster counts, plus a fixed
  file allowance);
- the system areas (bitmap, up-case table, root directory);
- a free tail equal to the sources' free space in view clusters, capped at 8M clusters (32 GiB).

The host therefore sees a capacity that follows the data actually on the card rather than a copy of
the whole card. Free space comes only from the tail; free clusters inside a reservation are never
reallocated.

## Index limits

The lazy engine's defaults are in `lazy_types.h` and can be overridden with `-D`:

| Macro | Default | Meaning |
| --- | --- | --- |
| `CONFIG_STORAGE_PROXY_LAZY_MAX_DIRS` | 4096 | Maximum number of indexed directories |
| `CONFIG_STORAGE_PROXY_LAZY_NAME_BYTES` | 256 KiB | Total bytes of directory names |
| `CONFIG_STORAGE_PROXY_LAZY_CACHE_ENTRIES` | 8192 | Entries across all directory caches |
| `CONFIG_STORAGE_PROXY_LAZY_CACHE_BYTES` | 512 KiB | Bytes across all directory caches |
| `CONFIG_STORAGE_PROXY_LAZY_DIR_MAX_ENTRIES` | 16384 | Entries exposed by one directory |
| `CONFIG_STORAGE_PROXY_LAZY_OVERLAY_MAX` | 1024 | Write-back overlay records |
| `CONFIG_STORAGE_PROXY_LAZY_PENDING_SECTORS` | 1024 | Pending-sector table size |
| `CONFIG_STORAGE_PROXY_LAZY_RUNS` | 4 | Simultaneously open staging files |
| `CONFIG_STORAGE_PROXY_LAZY_TAIL_CLUSTERS` | 8388608 | Free-tail cap in clusters |

When a directory or name limit is hit, `storage_proxy_lazy_truncated()` returns true and the host
sees an incomplete directory. `lazyproxy status` prints `truncated 1`.

## USB device

USB uses the MUSB device controller and the CherryUSB MSC class. The transfer buffer is the 64 KiB
block of on-chip SRAM starting at `0x00100000`, not PSRAM.

Windows 10/11 enumerates the device as `VID_1D6B/PID_0105` and mounts an exFAT volume named
`CardSight SD Card`. With the proxy view the host sees the synthesised root; with direct whole-disk
export it sees the real MBR/GPT and partitions.

Removing the card stops the USB device and removes the proxy view; the host sees the device
disconnect. Reinserting the card requires starting the export again.

## Manual control

```text
lazyproxy start mmcblk0 0x3
lazyproxy usb rw
lazyproxy status
lazyproxy usb off
lazyproxy stop
```

`0x3` exports partitions 1 and 2. Without a mask, `lazyproxy start mmcblk0` selects every partition
that exists.

For diagnostics, the full-index engine is also available:

```text
storageproxy start mmcblk0 0x1
cardsight usb on
cardsight usb off
```

## Tests

The `lazy_proxy` test in `ctest` runs against real filesystems on a RAM disk and covers read-only
source mirrors, host writes and write-back, overwriting existing files, deletes, renames, cache
eviction, index-limit truncation, capacity and free-space reporting, cancel and restart.
`storage_proxy_matrix` covers the full-index engine with FAT32/exFAT/ext4 mixed partitions and GPT.

Neither test needs the board or touches a register.
