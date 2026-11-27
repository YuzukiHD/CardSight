---
title: Storage proxy internals
description: "How the lazy and full-index exFAT views are laid out, write back and enforce their limits."
---

Two engines synthesize an exFAT volume from selected source partitions. Both expose the same
`storage_proxy.h` partition information; the UI uses the lazy engine, and the full-index engine is
kept for diagnostics.

| | Lazy index | Full index |
| --- | --- | --- |
| Block device | `cardview1` | `cardview0` |
| Cluster size | 4 KiB | 32 KiB |
| Source indexing | On the host's first read of a directory | At start-up |
| Root reports | `_LAYOUT.TXT` and `_DIR_LIMIT.TXT` | `_LAYOUT.TXT` |
| Used by | UI Proxy page and `lazyproxy` | `storageproxy` and `cardsight usb` |

## Lazy view

The lazy engine builds the system areas before registering the device:

- 24 boot sectors, a FAT at sector 32, an allocation bitmap and an up-case table;
- a root directory with one folder per selected partition (`partition1`, `partition2`, ...) and the
  two read-only text reports;
- on the board, a `System Volume Information` folder on the first selected partition for Windows.

Below the partition folders, a source directory is indexed the first time the host reads it. The
index is an LRU cache of directory entries and names; an evicted directory is scanned again on the
next read. Source files are mapped as cluster runs, not copied. The cache and directory limits are
configured in [Configuration](../reference/configuration.md).

## Lazy write-back

Host writes do not land on the source immediately:

1. Directory entries, names and size changes enter an in-memory overlay.
2. Data enters a pending-sector table and is coalesced into a staging file on a writable source
   partition.
3. On flush or stop, staged ranges are written back to the source, the staging file is removed and
   the directory entries are updated.

The overlay, pending table and staging runs are bounded. A write past a limit returns `-ENOSPC`.
Removing the card with pending data makes the proxy report a flush failure; the data cannot be
written back.

## Capacity and free space

The lazy view size is the sum of:

- per-partition reservations sized from the used source data (ext4 uses one view cluster per 4 KiB
  of used data; FAT/exFAT use the larger of view and source cluster counts plus a file allowance);
- the system areas (bitmap, up-case table, root directory);
- a free tail built from the sources' free space in view clusters, capped by
  `CONFIG_STORAGE_PROXY_LAZY_TAIL_CLUSTERS`.

Free space comes only from the tail; free clusters inside a reservation are not reallocated.

## Full-index view

The full-index engine walks the selected source trees at start-up, builds a complete index and
registers `cardview0`. It uses 32 KiB clusters, a 128 KiB write/batch buffer, 1024 pending sectors,
eight staging runs, a run gap of 64 clusters and a 2 s lock timeout. It exposes only `_LAYOUT.TXT` in
the root.

## Tests

`storage_proxy_matrix` covers the full-index engine with FAT32, exFAT, ext4, mixed partitions and
GPT. `lazy_proxy` covers the lazy engine's read path, cache eviction, truncation, cancellation,
write-back and restart with FAT and ext4 sources. The suites are described in [Testing](testing.md).

The user-facing steps and the read-only flag are in [USB export](../guide/usb-export.md).
