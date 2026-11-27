---
title: Contributing
description: "Workflow, style, tests and review expectations for changes to CardSight."
---

The canonical contributor guide is [CONTRIBUTING.md](https://github.com/YuzukiHD/CardSight/blob/master/CONTRIBUTING.md).
This page summarizes the parts that matter while editing the firmware or the docs.

## Workflow

- Open an issue for a reproducible bug or a feature, unless the change is a small documentation
  fix.
- Keep a change focused. Do not mix a driver fix, a UI change and a docs rewrite.
- Maintainers commit directly to `master`; external contributors can open pull requests against
  `master`.
- Read [CODE_OF_CONDUCT.md](https://github.com/YuzukiHD/CardSight/blob/master/CODE_OF_CONDUCT.md).

## Setup

```sh
git clone --recurse-submodules https://github.com/YuzukiHD/CardSight.git
export XUANTIE_TOOLCHAIN=/path/to/Xuantie-900-gcc-elf-newlib-x86_64-V3.2.0
cd CardSight/software
cmake --preset host && cmake --build build/host
ctest --test-dir build/host --output-on-failure
```

The docs site needs Node, Emscripten and Doxygen; see [Maintaining](maintaining.md).

## Style

- New source files start with `SPDX-License-Identifier: GPL-2.0-or-later`.
- Public functions, types and macros carry English Doxygen comments.
- Board facts go in `software/board/`; protocol drivers do not touch registers; controller drivers
  do not implement protocol state machines.
- Register initialisation goes through `INITCALL`.
- A driver that can be called from several tasks owns a mutex.
- Return negative errno values and document failure modes.

## Tests and evidence

Add a host regression test for block-layer, VFS, filesystem or proxy changes. The host tests use RAM
disks and must not touch board registers. Check UI changes with `ui_sim` and the browser simulator.

If you report a hardware result, include the board revision, card model, firmware commit, command
and serial log. Do not publish a number that cannot be reproduced from a checked-in method. The
project omits unverifiable performance figures from the documentation.

## Submodules and patches

Third-party submodules stay upstream. Board changes are kept in `software/patches/` and applied at
build time to a copy under `software/build/`. When changing a submodule pointer, make sure the
commit exists in the public repository; in particular, SyterKit must contain
`boards/yuzuki-cardsight/`.

## Documentation

Documentation is English only and has one source of truth per topic. Use quoted frontmatter titles
and descriptions, lowercase kebab-case filenames and relative `.md` links. Do not hard-code
`/CardSight/...` in content. Put hazards in a Starlight caution aside. Update the changelog when
behaviour changes.

## Pull request checklist

- [ ] Focused change with a short summary.
- [ ] Tests listed with commands and results.
- [ ] Hardware evidence for driver/board changes.
- [ ] Documentation and changelog updated for user-visible behaviour.
- [ ] Submodule pointers exist upstream.
- [ ] SPDX headers and Doxygen comments on new public declarations.
- [ ] No generated build output, credentials or private card data.
