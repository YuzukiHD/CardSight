---
title: License
description: "GPL-2.0-or-later for CardSight, and where the third-party and font notices live."
---

CardSight is released under **GPL-2.0-or-later**. The full text is in
[LICENSE](https://github.com/YuzukiHD/CardSight/blob/master/LICENSE).

## Why GPL-2.0-or-later

- lwext4 is GPL-2.0.
- CherryUSB is Apache-2.0. It is compatible with GPL-2.0-or-later but not with a GPL-2.0-only
  release without replacing it or obtaining an exception.
- The application and the platform code use the same GPL-2.0-or-later terms.

## Third-party components

The canonical inventory, with versions, licenses and patch notes, is
[THIRD_PARTY.md](https://github.com/YuzukiHD/CardSight/blob/master/THIRD_PARTY.md). It covers
FreeRTOS, LVGL, CherryUSB, lwext4, FatFs, SyterKit and the generated font data.

Third-party submodules remain upstream. Board changes are applied at build time from
`software/patches/`; the submodule trees are not modified.

## Fonts

The generated UI fonts are under the SIL Open Font License 1.1. The notices for Source Sans and
Noto Sans SC are collected in
[software/apps/cardsight/FONT_LICENSE.txt](https://github.com/YuzukiHD/CardSight/blob/master/software/apps/cardsight/FONT_LICENSE.txt).
`software/scripts/gen-fonts.py` is MIT-licensed.

## Documentation

The documentation site uses Astro, Starlight and Mermaid under their upstream licenses. The site
content follows the repository license.
