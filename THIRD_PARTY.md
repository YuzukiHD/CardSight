# Third-party components

All dependencies are git submodules pinned to a release tag (or, for SyterKit, the commit that
carries this board). `git submodule update --init --recursive` prepares them.

| Component | Version / submodule | License | Path |
|---|---|---|---|
| FreeRTOS Kernel | tag `V11.3.3`, https://github.com/FreeRTOS/FreeRTOS-Kernel.git | MIT (`LICENSE.md`) | `software/third_party/FreeRTOS-Kernel/` |
| LVGL | tag `v9.6.0`, https://github.com/lvgl/lvgl.git | MIT (`LICENCE.txt`) | `software/third_party/LVGL/` |
| CherryUSB | tag `v1.6.1`, https://github.com/cherry-embedded/CherryUSB.git | Apache-2.0 (`LICENSE`) | `software/third_party/CherryUSB/` |
| lwext4 | `master` (latest commit), https://github.com/gkostka/lwext4.git | GPL-2.0 (`LICENSE`) | `software/third_party/lwext4/` |
| FatFs | tag `R0.16` (abbrev/fatfs mirror), https://github.com/abbrev/fatfs.git | FatFs license (`source/00readme.txt`) | `software/third_party/fatfs/` |
| SyterKit | commit containing this board's eFEX, https://github.com/YuzukiHD/SyterKit.git | GPL-2.0-or-later | `software/third_party/SyterKit/` |
| Source Sans (generated glyphs) | Adobe, SIL OFL 1.1 | OFL 1.1 (`FONT_LICENSE.txt`) | `software/apps/cardsight/fonts/` |

Notes:

- The project as a whole is released under GPL-2.0-or-later. CherryUSB is Apache-2.0, which is
  compatible with GPLv3/GPL-2.0-or-later; using GPL-2.0-only would require replacing CherryUSB or
  obtaining an exception.
- The CherryUSB submodule stays as upstream. The MUSB/MSC changes this board needs are in
  `software/patches/cherryusb/0001-board-support.patch`, applied at build time to a copy in
  `software/build/cherryusb/` (`software/src/CMakeLists.txt`), without modifying the submodule.
  The changes adjust device-stack interfaces and add `CONFIG_USBDEV_MSC_EXTERNAL_BUFFER` (exporting
  the reserved on-chip SRAM as the transfer buffer).
- The FatFs submodule stays as upstream. This project's `ffconf.h` is at
  `software/src/fs/fatfs/ffconf.h`; at build time the FatFs sources are copied to
  `software/build/fatfs/` and the configuration is placed next to them (`ff.c` includes
  `ffconf.h` with quotes, so the same directory wins).
- The lwext4 submodule stays as upstream. Newer `mke2fs` versions enable `metadata_csum_seed`
  (incompat feature `0x2000`) by default, which upstream does not support and rejects at mount with
  `ENOTSUP`. The board patches are in `software/patches/lwext4/`, applied at build time to a copy
  in `software/build/lwext4/` (`software/cmake/lwext4.cmake`), without modifying the submodule.
  `0001-csum-seed.patch` recognises the feature and seeds metadata checksums from
  `s_checksum_seed` in the superblock; when the feature is absent it still uses the UUID, so
  behaviour is unchanged. `0002-inode-by-number.patch` adds an inode-by-number read; the VFS ext
  directory enumeration uses it instead of resolving a full path for every entry, which cuts the
  first open of a thousand-file directory from about 10 seconds to under one.
  `0003-mkfs-last-group.patch` fixes volumes smaller than one block group: the last partial group
  was counted as a full group, so the superblock's `free_blocks_count` exceeded the volume's block
  count. Patches are applied in file-name order; a new change is one more numbered file in
  `software/patches/lwext4/`.
- The lwext4 block cache is raised to 64 4 KiB blocks (`software/cmake/lwext4.cmake`) so directory
  blocks and inode-table blocks do not evict each other while walking a large directory; each
  mounted ext volume uses about 256 KiB of heap.
- `software/third_party/SyterKit` contains the vendor's prebuilt DRAM/PSRAM initialisation library
  (`soc/sun252iw1/lib/*.a`). Those binaries are distributed with SyterKit, not as source of this
  repository; source tarballs follow SyterKit's own license and notes and do not repack them.
- Third-party directories keep upstream files unchanged; the build only selects which files to
  compile.
