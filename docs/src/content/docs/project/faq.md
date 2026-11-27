---
title: FAQ and troubleshooting
description: "Common symptoms on the board and in the build, with checks and source-backed causes."
---

## The card is not detected

Check the shell first:

```text
cardsight> mmc
cardsight> blk
```

- An empty slot is not an error. Insert a TF card and wait for the hot-plug task.
- `mmc eject` forces the detect state to absent; run `mmc insert` to clear it.
- A failed identification is retried with a longer poll period. Reseat the card and check the
  UART log.
- If `blk` shows no `mmcblk0`, the MMC driver did not publish a disk; see [Hardware](../reference/hardware.md)
  for the slot and pin details.

## The panel is black or the colours are wrong

The panel path depends on several settings that must agree:

- DC and reset must be GPIO outputs, not alternate functions.
- The SPI controller must set the DHB/DDB/SDM bits.
- The panel init sets `MADCTL = 0x40` and `COLMOD = 0x06`; the display publishes RGB888 with no byte
  swap.
- LVGL is configured for RGB888, not RGB565.

The `display test` command cycles red, green, blue, white and black. If those colours are wrong, the
problem is in the panel/SPI path, not the UI.

## USB does not enumerate

- Connect the board's OTG port, not the UART port.
- Start an export first: **Connect USB** on the Direct page, or start the proxy.
- The default identity is VID `0x1D6B`, PID `0x0105`.
- `cardsight usb disk` exports the whole disk read-only; `lazyproxy usb rw` exports the synthesized
  view read-write.
- Stop USB before starting a different export; the driver refuses a second device.

## A direct export is read-only

`usb_mass_storage_force_read_only()` is sticky in the current source. After `cardsight usb disk`, a
later **Connect USB** on the Direct page can inherit the read-only flag. Stop USB and start the lazy
proxy once with `lazyproxy usb rw`, or reboot, before expecting a read-write direct export. This is a
documented limitation, not a card fault. See [USB export](../guide/usb-export.md).

## `flush failed` after removing the card

The storage proxy writes host changes to a staging area and flushes them to the source partitions.
If the card is removed first, the card is no longer powered and the staged data cannot be written
back. That data is lost. Stop the export and let it flush before removing the card.

## Formatting fails

- Stop USB first; formatting is refused while a USB export is running.
- A read-only card cannot be formatted.
- A partition that is too small for the requested filesystem is rejected. FatFs returns
  `FR_MKFS_ABORTED` for a volume below its minimum size (mapped to `-ENOSPC`) and
  `FR_NOT_ENOUGH_CORE` when its work area is insufficient (mapped to `-ENOMEM`).
- The layout editor clears the partition table; back up the card first.

## The file browser reports an error

The error sheet names the failed operation and path. `Cannot open partition` means the volume did
not probe or mount; `Directory not found` means the path is stale; `Path too long` exceeds
`VFS_PATH_MAX`; `Read failed` is a filesystem or block-layer error. Removing the card leaves the
detail view by design.

## The build fails

- Fetch submodules: `git submodule update --init --recursive`. The host build needs FatFs, lwext4
  and LVGL.
- The lwext4 patch is applied at configure time from `software/patches/lwext4/`; a modified
  submodule checkout can make the patch fail.
- SyterKit must point at a public commit containing `boards/yuzuki-cardsight/`.
- `XUANTIE_TOOLCHAIN` must point at the XuanTie newlib toolchain for the RISC-V preset.
- The docs build needs Emscripten and Doxygen. Keep a previous build and use
  `CARDSIGHT_SKIP_WASM=1` / `CARDSIGHT_SKIP_API=1` only when those directories already exist.

## Where to ask

Use [Support](https://github.com/YuzukiHD/CardSight/blob/master/SUPPORT.md) and the issue tracker,
and include the board revision, commit, toolchain, card model, commands and serial log. Security
issues follow [SECURITY.md](https://github.com/YuzukiHD/CardSight/blob/master/SECURITY.md).
