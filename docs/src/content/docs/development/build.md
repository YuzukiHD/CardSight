---
title: Build
description: "Toolchain, CMake presets, firmware images, host tests and the source release tarball."
---

## Requirements

- The XuanTie-900 GCC elf newlib toolchain, supplied through `XUANTIE_TOOLCHAIN` or
  `-DXUANTIE_TOOLCHAIN=...`.
- CMake 3.20 or newer.
- Git submodules: FreeRTOS, LVGL, CherryUSB, lwext4, FatFs and SyterKit.
- A C compiler for the host tests. The host build also needs the FatFs, lwext4 and LVGL submodules;
  LVGL is required by `ui_sim`.

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

The `riscv` preset uses the RISC-V toolchain file, builds the application and the two SyterKit eFEX
images, and writes:

| Image | Purpose |
| --- | --- |
| `cardsight.bin` | C907 application, linked at `0x12000000` |
| `init_psram_efex.bin` | E907 PSRAM initialiser |
| `c907_boot_efex.bin` | E907 C907-release stub |

The images are in `software/build/riscv/images/`. Loading into RAM and writing SPI NOR are in
[Flashing and boot](../getting-started/flashing.md).

Build options are in [Configuration](../reference/configuration.md). `CARDSIGHT_WITH_APP` gates the
application sources only; LVGL and the platform are always linked.

## Host tests

```sh
cd software
cmake --preset host
cmake --build build/host
ctest --test-dir build/host --output-on-failure
```

The `host` preset disables the eFEX build and enables `CARDSIGHT_HOST_TESTS`. It compiles the real
block layer, VFS, FatFs, lwext4 and both storage-proxy engines against RAM disks, and builds the
`ui_sim` renderer. The suites and the CI jobs are described in [Testing](testing.md).

## Release tarball

```sh
./software/scripts/release.sh 0.1.0
# dist/cardsight-0.1.0.tar.gz
```

The script includes the licenses, the README, the standard project files and both font notices. It
excludes build output, vendor prebuilt libraries (`*.a`), and the docs build products
(`docs/node_modules`, `docs/dist`, `docs/.astro`, `docs/public/api`, `docs/public/ui` and the
vendored mermaid copy).

## Documentation site

The site is an Astro/Starlight project with a WebAssembly UI simulator and a Doxygen API reference.
It is built from `docs/`; see [Maintaining](maintaining.md) for tools and commands.

## CI

`ci.yml` runs the host tests on every push and pull request. On non-master branches it also builds
the docs site, the WebAssembly UI and the C API, and runs the link checker. On `master`, `docs.yml`
does the same and deploys the site to GitHub Pages.
