---
title: Build
description: The cross toolchain, host tests, release tarball, and how the WebAssembly simulator in the docs is built.
---

## Requirements

- RISC-V toolchain: XuanTie-900 GCC elf newlib (verified with V3.2.0). Its path is given through
  the `XUANTIE_TOOLCHAIN` environment variable, or with `-DXUANTIE_TOOLCHAIN=...` on the CMake
  command line. The repository does not hard-code a local path.
- Host tests need only CMake ≥ 3.20 and a C compiler.
- The simulator in the docs needs Emscripten.

All third-party components are git submodules; fetch them first:

```sh
git submodule update --init --recursive
```

## Firmware

```sh
export XUANTIE_TOOLCHAIN=/path/to/Xuantie-900-gcc-elf-newlib-x86_64-V3.2.0
cd software
cmake --preset riscv
cmake --build build/riscv
```

The output is in `software/build/riscv/images/`:

| File | Description |
| --- | --- |
| `cardsight.bin` | Application image for the C907, linked at `0x12000000` |
| `init_psram_efex.bin` | E907 image that initialises PSRAM and returns to FEL |
| `c907_boot_efex.bin` | E907 image that releases the C907 at entry `0x12000000` |

The two eFEX images come from the `yuzuki-cardsight` board configuration in
`software/third_party/SyterKit`. The CMake target is `efex_stubs` and is built by default.
Flashing steps are in [Flashing and boot](/CardSight/fel/).

Build switches:

| Variable | Default | Effect |
| --- | --- | --- |
| `CARDSIGHT_WITH_APP` | ON | Build the application and LVGL |
| `CARDSIGHT_HOST_TESTS` | OFF | Build only the host tests |
| `CARDSIGHT_BUILD_EFEX` | ON | Build the two eFEX images |
| `CARDSIGHT_WERROR` | OFF | Treat warnings in our own code as errors |

## Host tests

The block layer, VFS, FatFs, lwext4 and both storage-proxy engines do not touch board registers, so
they compile on a PC. The tests run the full storage-proxy matrix against RAM disks:

```sh
cd software
cmake --preset host
cmake --build build/host
ctest --test-dir build/host --output-on-failure
```

Two tests run:

- `storage_proxy_matrix`: FAT32, exFAT, ext4, mixed partitions, GPT layouts and the VFS.
- `lazy_proxy`: lazy-index reads, writes, overwrites, deletes, capacity reporting, cancellation
  and limits.

`build/host/tests/host/ui_sim` is also built. It renders the real LVGL UI and interaction states
to PPM files in `/tmp/ui-*.ppm`; it is a development tool, not a registered test.

## Release tarball

```sh
./software/scripts/release.sh 0.1.0
# dist/cardsight-0.1.0.tar.gz
```

The script leaves out build output and vendor prebuilt libraries, and keeps the licenses and
submodule references.

## Docs site and the WebAssembly simulator

`docs/` is a separate Astro/Starlight project. `npm run dev` or `npm run build` first runs
`docs/scripts/build-wasm.mjs`:

```sh
# one-time Emscripten installation
git clone https://github.com/emscripten-core/emsdk.git ~/emsdk
~/emsdk/emsdk install latest
~/emsdk/emsdk activate latest
source ~/emsdk/emsdk_env.sh

cd docs
npm ci
npm run build      # wasm + static site, output in docs/dist/
```

The simulator sources live in `software/tests/wasm/`. Its CMake project is independent of the
firmware build and compiles:

- LVGL core, widgets and fonts, plus the pages, widgets and material under
  `software/apps/cardsight/ui/`;
- the same fonts and background images as the firmware (`fonts/`, `assets/`);
- `ui_web.c`, which provides the memory framebuffer, pointer input, a fixed 32 GB card model and
  the request emulator.

Emscripten writes `cardsight-ui.js` and `cardsight-ui.wasm`; the build copies them to
`docs/public/ui/`, which is not tracked by git. When only Markdown changes, an existing wasm build
can be reused:

```sh
CARDSIGHT_SKIP_WASM=1 npm run build
```

CI runs the host tests on every push and pull request. Branches and pull requests also build the
docs site, and the `docs` workflow on master builds and deploys it to GitHub Pages.
