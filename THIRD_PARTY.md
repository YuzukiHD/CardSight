# Third-party components

All dependencies are git submodules pinned to a release tag (or, for SyterKit, the commit that
carries this board). `git submodule update --init --recursive` prepares them.

| 组件 | 版本 / submodule | 许可 | 路径 |
|---|---|---|---|
| FreeRTOS Kernel | tag `V11.3.3`，https://github.com/FreeRTOS/FreeRTOS-Kernel.git | MIT（`LICENSE.md`） | `software/third_party/FreeRTOS-Kernel/` |
| LVGL | tag `v9.6.0`，https://github.com/lvgl/lvgl.git | MIT（`LICENCE.txt`） | `software/third_party/LVGL/` |
| CherryUSB | tag `v1.6.1`，https://github.com/cherry-embedded/CherryUSB.git | Apache-2.0（`LICENSE`） | `software/third_party/CherryUSB/` |
| lwext4 | `master`（最新提交），https://github.com/gkostka/lwext4.git | GPL-2.0（`LICENSE`） | `software/third_party/lwext4/` |
| FatFs | tag `R0.16`（abbrev/fatfs 镜像），https://github.com/abbrev/fatfs.git | FatFs 许可（`source/00readme.txt`） | `software/third_party/fatfs/` |
| SyterKit | 含本板 eFEX 的提交，https://github.com/YuzukiHD/SyterKit.git | GPL-2.0-or-later | `software/third_party/SyterKit/` |
| Source Sans 字体（生成的字模） | Adobe，SIL OFL 1.1 | OFL 1.1（`FONT_LICENSE.txt`） | `software/apps/cardsight/fonts/` |

说明：

- 本项目整体以 GPL-2.0-or-later 发布。CherryUSB 为 Apache-2.0，与 GPLv3/GPL-2.0-or-later
  兼容；若必须使用 GPL-2.0-only，需要替换 CherryUSB 或取得例外授权。
- CherryUSB 的 submodule 保持上游原样；本板需要的 MUSB/MSC 修改在
  `software/patches/cherryusb/0001-board-support.patch`，构建时应用到 `software/build/cherryusb/` 的副本上
  （`software/src/CMakeLists.txt`），不修改 submodule。修改包括设备栈的接口调整和
  `CONFIG_USBDEV_MSC_EXTERNAL_BUFFER`（导出保留的片上 SRAM 作为传输缓冲）。
- FatFs 的 submodule 保持上游原样；本项目的 `ffconf.h` 在 `software/src/fs/fatfs/ffconf.h`，构建时
  FatFs 源被复制到 `software/build/fatfs/` 并覆盖配置（`ff.c` 用引号包含 `ffconf.h`，同目录优先）。
- `software/third_party/SyterKit` 内含有厂家预编译的 DRAM/PSRAM 初始化库
  （`soc/sun252iw1/lib/*.a`）。这些二进制随 SyterKit 一起分发，不属于本仓库源码；
  发布源码包时按 SyterKit 自身的许可与说明处理，不在本仓库的源码 tarball 中重新打包。
- 第三方目录保持上游文件不变（仅按构建需要选择被编译的文件）。
