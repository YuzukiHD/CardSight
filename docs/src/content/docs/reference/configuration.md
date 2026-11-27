---
title: Configuration
description: "Build options, proxy limits, VFS limits, and the FatFs, lwext4, LVGL and USB settings used by CardSight."
---

Every value on this page comes from a checked-in source file. When a value is not listed, the
source is the reference.

## Build options

| Option | Default | Effect |
| --- | --- | --- |
| `CARDSIGHT_HOST_TESTS` | OFF | Build the host tests and `ui_sim` instead of the firmware |
| `CARDSIGHT_BUILD_EFEX` | ON | Build the SyterKit eFEX images |
| `CARDSIGHT_WITH_APP` | ON | Build the application sources. LVGL and the platform are always linked. |
| `CARDSIGHT_WERROR` | OFF | Treat warnings in our own code as errors |
| `XUANTIE_TOOLCHAIN` | unset | Cross-toolchain prefix directory; required for the RISC-V preset |

`CARDSIGHT_UART_BAUD` and `CARDSIGHT_BOARD` exist in `software/cmake/options.cmake` but are not read
by the build. The real UART baud rate is set in `software/board/cardsight_board.c`.

The `riscv` preset configures `build/riscv`; the `host` preset configures `build/host`. See
[Build](../development/build.md).

## VFS and path limits

| Macro | Value | Meaning |
| --- | --- | --- |
| `VFS_FD_BASE` | 3 | First file descriptor; 0..2 are the console |
| `VFS_NAME_MAX` | 255 | Longest file name, excluding the terminator |
| `VFS_PATH_MAX` | 512 | Longest path, including the terminator |
| `VFS_BLK_NAME` | 32 | Longest block-device name |
| `CONFIG_VFS_MAX_MOUNTS` | 8 | Mount table entries |
| `CONFIG_VFS_MAX_FILES` | 16 | Open file slots |
| `CONFIG_VFS_MAX_FS_TYPES` | 8 | Registered filesystem types |
| `CONFIG_EXT_FS_MAX_MOUNTS` | 3 | Separate cap for ext volumes |

The block layer uses 512-byte sectors throughout. The ext mount cap is separate from the total mount
cap.

## Lazy storage proxy

Defaults from `software/apps/cardsight/storage/lazy/lazy_types.h`:

| Macro | Default |
| --- | ---: |
| `CONFIG_STORAGE_PROXY_LAZY_MAX_DIRS` | 4096 |
| `CONFIG_STORAGE_PROXY_LAZY_NAME_BYTES` | 262144 |
| `CONFIG_STORAGE_PROXY_LAZY_CACHE_ENTRIES` | 8192 |
| `CONFIG_STORAGE_PROXY_LAZY_CACHE_BYTES` | 524288 |
| `CONFIG_STORAGE_PROXY_LAZY_DIR_MAX_ENTRIES` | 16384 |
| `CONFIG_STORAGE_PROXY_LAZY_FILE_ALLOWANCE` | 1024 |
| `CONFIG_STORAGE_PROXY_LAZY_TAIL_CLUSTERS` | 8388608 |
| `CONFIG_STORAGE_PROXY_LAZY_OVERLAY_MAX` | 1024 |
| `CONFIG_STORAGE_PROXY_LAZY_OVERLAY_NAME_BYTES` | 65536 |
| `CONFIG_STORAGE_PROXY_LAZY_PENDING_SECTORS` | 1024 |
| `CONFIG_STORAGE_PROXY_LAZY_RUNS` | 4 |
| `CONFIG_STORAGE_PROXY_LAZY_RUN_MAX_CLUSTERS` | 32768 |
| `CONFIG_STORAGE_PROXY_LAZY_RUN_GAP` | 256 |
| `CONFIG_STORAGE_PROXY_LAZY_FAT_ENTRIES` | 4096 |
| `CONFIG_STORAGE_PROXY_LAZY_MAX_RANGES` | 16384 |
| `CONFIG_STORAGE_PROXY_LAZY_DIR_SLACK_CLUSTERS` | 1 |
| `CONFIG_STORAGE_PROXY_LAZY_DIR_ENTRY_SLACK` | 64 |

The view has 8 partitions, 4 KiB clusters, 24 boot sectors, a FAT at sector 32, and a maximum depth
of 64. See [Storage proxy internals](../development/storage-proxy.md) for what each limit protects.

## Full-index storage proxy

Key values from `software/apps/cardsight/storage/eager/proxy_types.h`:

| Item | Value |
| --- | --- |
| Block device | `cardview0` |
| Cluster size | 32 KiB (64 sectors) |
| Write/batch buffer | 128 KiB |
| Pending sectors | 1024 |
| Staging runs | 8 |
| Run gap | 64 clusters |
| Lock timeout | 2000 ms |
| Root report | `_LAYOUT.TXT` |

## FatFs

`software/src/fs/fatfs/ffconf.h`:

| Option | Value |
| --- | --- |
| Read-only | No |
| `FF_USE_MKFS` | 1 |
| `FF_USE_EXPAND` | 1 |
| `FF_USE_LFN` | 3 |
| `FF_LFN_UNICODE` | 2 |
| `FF_CODE_PAGE` | 437 |
| `FF_VOLUMES` | 4 |
| `FF_MIN_SS` / `FF_MAX_SS` | 512 |
| `FF_FS_EXFAT` | 1 |
| `FF_USE_FASTSEEK` | 0 |

Formatting maps `FR_MKFS_ABORTED` to `-ENOSPC` and `FR_NOT_ENOUGH_CORE` to `-ENOMEM`.

## lwext4

`software/cmake/lwext4.cmake` sets `CONFIG_BLOCK_DEV_CACHE_SIZE=64`, so each mounted ext volume uses
about 256 KiB of block cache. Journaling is enabled. The three board patches are described in
[THIRD_PARTY.md](https://github.com/YuzukiHD/CardSight/blob/master/THIRD_PARTY.md).

## LVGL

`software/src/gui/lv_conf.h` and the LVGL port:

| Setting | Value |
| --- | --- |
| Color format | RGB888 on the panel path |
| Default refresh period | 10 ms |
| DPI | 130 |
| Draw-buffer alignment | 64 bytes |
| Draw buffer | 40 lines, 240 × 40 × 3 bytes |
| OS integration | None; LVGL runs in its own task |
| Logging | Off |

## USB

`software/src/usb/usb_config.h` and `usb_mass_storage.c`:

| Setting | Value |
| --- | --- |
| Controller | MUSB, device mode |
| Class | MSC, one LUN |
| Speed | High speed |
| MSC buffer | 64 KiB external buffer at `0x00100000` |
| VID / PID | `0x1D6B` / `0x0105` |
| Device strings | `CardSight`, `CardSight SD Card`, `CARD SIGHT MSC01`, `Mass Storage` |

The device role is forced to a B device with VBUS valid. The USB transfer buffer comes from on-chip
SRAM, not PSRAM.

## Memory and limits

The memory map, heap sizes, panel and TF-card details live in
[Hardware](hardware.md). The application limits (`APP_LAYOUT_MAX`, `APP_FILE_ENTRIES`,
`APP_FILE_NAME`, request queue depth) are documented there and in
[Architecture](../development/architecture.md) where they affect behaviour.
