# Contributing to CardSight

CardSight is a TF card proxy system for the Yuzuki CardSight board. Contributions to the firmware,
the documentation and the test suite are welcome.

By participating you agree to the [Code of Conduct](CODE_OF_CONDUCT.md). For support questions use
[SUPPORT.md](SUPPORT.md); report vulnerabilities through [SECURITY.md](SECURITY.md), not a public
issue.

## Before you start

- Read the published documentation at <https://yuzukihd.github.io/CardSight/>.
- Check existing issues and pull requests before opening a new one.
- Keep changes focused. A pull request that mixes a driver fix, a UI change and a docs rewrite is
  hard to review and hard to revert.
- The maintainers commit directly to `master`. External contributors can open pull requests against
  `master`; no long-lived feature branches are required.

## Development setup

The RISC-V toolchain is supplied through `XUANTIE_TOOLCHAIN`; the repository never hard-codes a
local path.

```sh
git clone --recurse-submodules https://github.com/YuzukiHD/CardSight.git
cd CardSight
export XUANTIE_TOOLCHAIN=/path/to/Xuantie-900-gcc-elf-newlib-x86_64-V3.2.0
```

Firmware build:

```sh
cd software
cmake --preset riscv
cmake --build build/riscv
```

Host tests (FatFs, lwext4 and LVGL submodules are required; no board needed):

```sh
cd software
cmake --preset host
cmake --build build/host
ctest --test-dir build/host --output-on-failure
```

Documentation site (Node, Emscripten and Doxygen):

```sh
cd docs
npm ci
npm run build
npm run check
```

See [docs/README.md](docs/README.md) and the published
[maintaining guide](https://yuzukihd.github.io/CardSight/development/maintaining/) for the tools and
for the `CARDSIGHT_SKIP_WASM` / `CARDSIGHT_SKIP_API` shortcuts.

## Repository layout

| Path | Contents |
| --- | --- |
| `software/board/` | Board constants and linker script; no device tree |
| `software/src/` | Boot, kernel glue, drivers, block layer, VFS, GUI and USB |
| `software/apps/cardsight/` | Application, storage proxy, UI, fonts and assets |
| `software/tests/host/` | Host tests and the `ui_sim` renderer |
| `software/tests/wasm/` | WebAssembly UI simulator |
| `software/patches/` | Build-time patches for CherryUSB and lwext4 |
| `docs/` | Astro/Starlight documentation site |
| `hardware/` | Hardware design placeholder; no schematics are published yet |

## Coding style

- Use the SPDX identifier `GPL-2.0-or-later` at the top of every new source file.
- Keep Doxygen comments in English on public functions, structures and macros.
- Put board facts (bases, pins, clock gates) in `software/board/cardsight_board.c`; do not add a
  device tree or per-chip `#if` dispatch.
- Protocol drivers do not touch controller registers. Controller drivers do not implement protocol
  state machines. Call the API declared next to the driver.
- Register initialisation with `INITCALL(level, priority, function)`. Anything that needs the
  scheduler belongs at level 3.
- Use `readl`/`writel`, `setbits32`/`clrbits32`/`clrsetbits32` and the cache helpers from
  `software/include/hal/`; do not add private MMIO wrappers.
- Do not use `ktime_get_ms()` in a register polling loop. Use `cs_clint_ticks()` with a deadline,
  and `msleep`/`cs_udelay` for delays.
- Serialise a driver that can be called from several tasks with a mutex owned by the driver.
- Return negative errno values and document the failure modes.

## Tests and evidence

- Add a host regression test for block-layer, VFS, filesystem or storage-proxy changes. The tests
  use RAM disks and must not touch board registers.
- UI changes should be checked with `ui_sim` PPM output and, when possible, the WebAssembly
  simulator in the docs.
- If you report a result measured on hardware, include the board revision, card model, firmware
  commit, exact command and serial log. Do not publish a number that cannot be reproduced from a
  checked-in method or artifact.
- The project does not claim performance figures in the documentation unless they are backed by
  such a record.

## Submodules and patches

Third-party code lives in git submodules and stays upstream. Changes that a board needs are kept as
patches under `software/patches/` and applied at build time to a copy in `software/build/`.

When changing a submodule pointer, make sure the target commit exists in the public repository. In
particular, `software/third_party/SyterKit` must point at a commit containing
`boards/yuzuki-cardsight/`; otherwise nobody else can initialise the submodule.

## Documentation

- Documentation is English only and uses one source of truth per topic. Do not duplicate command
  tables, configuration values or hardware constants across pages.
- Write task-oriented pages: prerequisites, steps, expected output, related links.
- Use quoted `title` and a one-sentence `description` in frontmatter.
- Internal links use relative `.md` paths; the build plugin rewrites them. Do not hard-code
  `/CardSight/...` links in content.
- Put destructive or hazardous instructions in a Starlight caution aside.
- Update the changelog when behaviour changes.

## Pull requests

A pull request should include:

- a short summary and the problem it solves;
- the tests you ran, with commands;
- hardware evidence when the change affects a board driver;
- documentation updates for user-visible behaviour;
- a checked submodule pointer, if any.

The maintainers review for correctness, test coverage and documentation. Direct pushes to `master`
are limited to the maintainers.
