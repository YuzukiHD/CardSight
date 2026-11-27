---
title: License
description: Why the project is GPL-2.0-or-later, the third-party licenses, and what to check before publishing.
---

The CardSight repository is released under **GPL-2.0-or-later**; see `LICENSE` at the root.

The new platform code (`software/src/`, `software/board/`, `software/include/`, `software/cmake/`) and the application (`software/apps/cardsight/`) use GPL-2.0-or-later.

## Why GPL

lwext4 is GPL-2.0 and CherryUSB is Apache-2.0. Apache-2.0 is compatible with GPL-2.0-or-later, but choosing GPL-2.0-only would require replacing CherryUSB or obtaining an exception, so the project uses GPL-2.0-or-later.

Third-party components are included under their own licenses; see `THIRD_PARTY.md` and the license files in each directory.

## Third parties

| Component | License | Notes |
| --- | --- | --- |
| FreeRTOS Kernel | MIT | Submodule (V11.3.3), unmodified upstream |
| LVGL | MIT | Submodule (v9.6.0), only the needed files are compiled |
| CherryUSB | Apache-2.0 | Submodule (v1.6.1) plus the board patches in `software/patches/cherryusb/` |
| lwext4 | GPL-2.0 | Submodule (latest master) plus three patches |
| FatFs | ChaN license | Submodule (R0.16); `software/src/fs/fatfs/ffconf.h` is supplied by this project |
| SyterKit | GPL-2.0-or-later | Submodule; contains the vendor's prebuilt PSRAM/DRAM initialisation library |
| Source Sans (generated font data) | SIL OFL 1.1 | `software/apps/cardsight/FONT_LICENSE.txt` |

The documentation site uses Astro, Starlight and Mermaid under their upstream licenses.

## Publishing notes

- The `software/third_party/SyterKit` submodule must point at a commit that exists in the public repository and contains the board support (`boards/yuzuki-cardsight`); otherwise `git submodule update --init` fails for everyone else. The repository is `https://github.com/YuzukiHD/SyterKit.git`.
- Source tarballs do not repack the vendor binaries under `software/third_party/SyterKit/soc/sun252iw1/lib/`; they keep only the submodule reference and the notes.
- No configuration, credentials, tokens or local reference source code are distributed with the repository.
