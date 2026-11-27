---
title: USB export
description: "Export the whole card or a synthesized exFAT view over USB, and stop safely."
---

CardSight can present storage to a host in two ways:

| Mode | What the host sees | Engine |
| --- | --- | --- |
| Direct export | The whole card, with its real partition table and filesystems | Block device `mmcblk0` |
| Storage proxy | One synthesized exFAT volume containing selected partitions as folders | Lazy-index `cardview1` (UI), full-index `cardview0` (diagnostics) |

CardSight is a USB device. Connect the board's OTG port to the host; the host sees a USB
mass-storage device. The configured IDs are `VID_1D6B` and `PID_0105`.

## Direct export

Use the **Direct** page and **Connect USB**. The host sees the card itself:

- the real MBR or GPT and the real filesystems;
- writes go directly to the card;
- the card must be writable for a read-write export.

The page shows read and write rates while the link is up. Use **Stop safely** before removing the
card.

## Proxy export

Use the **Proxy** page to select partitions, then start the proxy. The host sees one exFAT volume:

- each selected partition appears as a folder named `partition1`, `partition2`, and so on;
- the source directory tree is indexed as the host reads it;
- host writes enter an overlay and a pending table, are staged on a source partition, and are
  written back on flush or stop;
- capacity and free space follow the source volumes and the configured tail limit.

The proxy is writable when at least one selected source is writable. `lazyproxy usb on` exports the
lazy view read-only; `lazyproxy usb rw` exports it read-write. The UI starts the proxy in
read-write mode.

The view has fixed limits for directories, names, cache entries, overlay records and staging runs.
When a limit is reached, a directory can be truncated or a host write can fail with `-ENOSPC`. The
limits and the internals are in [Storage proxy internals](../development/storage-proxy.md) and
[Configuration](../reference/configuration.md).

## Safe removal

Stop USB and the proxy before removing the card:

```text
cardsight> lazyproxy usb off
cardsight> lazyproxy stop
```

or use **Stop safely** in the UI. If the card is removed with unflushed writes, the proxy reports a
flush failure and that data is lost. This is a physical limit: the card is no longer powered.

:::caution
Do not remove the TF card while a write is in flight. Stop the export and wait for the flush to
finish.
:::

## Read-only state on the direct page

`usb_mass_storage_force_read_only()` applies to the next start and is sticky in the current
firmware. After `cardsight usb disk` (read-only whole-disk export), a later **Connect USB** on the
Direct page can therefore inherit the read-only flag. Stop USB and start the lazy proxy once with
`lazyproxy usb rw`, or reboot, before expecting a read-write direct export. This behaviour is
tracked in the [FAQ](../project/faq.md); it is a documented limitation of the current source.

## Command-line variants

```text
cardsight> lazyproxy start mmcblk0 0x3
cardsight> lazyproxy usb rw
cardsight> lazyproxy status
cardsight> lazyproxy usb off
cardsight> lazyproxy stop
```

`0x3` selects partitions 1 and 2. Without a mask, `lazyproxy start mmcblk0` selects every present
partition. The diagnostic path uses the full-index engine:

```text
cardsight> storageproxy start mmcblk0 0x1
cardsight> cardsight usb on
cardsight> cardsight usb off
```

`cardsight usb` and `lazyproxy` are not interchangeable: the first exports `cardview0` (full index),
the second `cardview1` (lazy index). The complete command reference is in
[Shell commands](../reference/cli.md).
