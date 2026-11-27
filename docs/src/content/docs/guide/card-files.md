---
title: Card and files
description: "Insert a TF card, manage partitions and filesystems, and browse files on the panel."
---

CardSight supports FAT32, exFAT and ext4. Insert a TF card before using the Disk or Files pages.
The MMC driver identifies the card and publishes block device `mmcblk0`; the block layer publishes
`mmcblk0p1`, `mmcblk0p2`, and so on.

## Card identification

- The card is identified at the fastest supported mode: SDR104, SDR50 or SDR25.
- 1.8 V signalling is negotiated when the card supports it.
- CMD19 tuning selects the sampling point; a failed tuning resets the controller, retunes and
  retries once.
- Card detection is debounced; the hot-plug task starts at 500 ms and backs off to 5 s while
  identification fails.

If the card is missing, the UI shows **No card detected**. `mmc` and `blk` on the shell show the
driver and block-layer view. See [FAQ](../project/faq.md) for identification failures.

## Partitions and formatting

The **Disk** page shows a capacity strip and the partition list. Opening a partition shows its
filesystem and size and offers formatting:

| Operation | Effect |
| --- | --- |
| Quick format | Recreates the filesystem metadata without wiping the partition |
| Full format | Wipes the partition data first, then creates the filesystem |
| Format whole card | Opens the layout editor and rewrites the partition table |

The layout editor creates one partition covering the remaining capacity. Choose MBR or GPT and
FAT32, exFAT or ext4; the apply action is a long press. The underlying layout engine supports more
partitions, but the UI does not expose them.

Formatting is refused while USB is running, on a read-only card, or while another request is in
progress. A partition that is too small for the chosen filesystem is reported as an error; FatFs
rejects a FAT32 volume below its minimum size.

:::caution
Formatting and rebuilding the partition table erase data. Confirm the card and read the
confirmation dialog before applying.
:::

## Filesystem support

| Filesystem | Read | Write | Format | Implementation |
| --- | --- | --- | --- | --- |
| FAT12/16/32 | Yes | Yes | Yes | FatFs |
| exFAT | Yes | Yes | Yes | FatFs |
| ext2/3/4 | Yes | Yes | Yes | lwext4 |

All three go through the VFS. The UI does not need to know which implementation serves a partition.

## Browsing files

The **Files** page lists partitions, then directories one level at a time. The worker reads each
directory and returns a bounded listing:

- up to 64 entries are kept (`APP_FILE_ENTRIES`);
- directories sort first, then names case-insensitively;
- long names are ellipsised;
- a directory with more entries reports the total and is marked truncated;
- paths containing `..` are rejected.

Opening a partition starts at `/`. The **Back** button goes up one level and returns to the
partition list from the root. Removing the card leaves the detail view and clears stale state.

## Formatting from the shell

The shell exposes the same VFS operations for diagnostics:

```text
cardsight> mount mmcblk0p2 /mnt/fat fat
cardsight> ls /mnt/fat
cardsight> df /mnt/fat
```

The complete command set is in [Shell commands](../reference/cli.md). Formatting options and
filesystem limits are in [Configuration](../reference/configuration.md).
