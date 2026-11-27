---
title: Testing
description: "Host test suites, the UI renderer, the WebAssembly simulator and CI checks."
---

The host tests use RAM disks and do not need the board or any register access. They compile the real
block layer, VFS, FatFs, lwext4 and both storage-proxy engines.

## Running the tests

```sh
cd software
cmake --preset host
cmake --build build/host
ctest --test-dir build/host --output-on-failure
```

The host build requires the FatFs, lwext4 and LVGL submodules; LVGL is used by `ui_sim`.

## Suites

### `storage_proxy_matrix`

Creates three 48 MiB RAM-disk partitions and runs five filesystem mixes:

- FAT32 only;
- exFAT only;
- ext4 only;
- FAT32 + exFAT + ext4;
- ext4 + FAT32 + exFAT.

It covers size boundaries, odd chunk sizes, many files, directories, rename and rmdir,
overwrite/truncate/grow/unlink, Windows-style data-before-entry ordering, selection masks, read-only
propagation and a GPT layout's `_LAYOUT.TXT`. It covers all three filesystems and the full-index
engine.

### `lazy_proxy`

Uses two 64 MiB partitions, one FAT and one ext4, with a directory of 1001 files, a smaller
directory, a `huge` directory and nested ext4 files. It covers the read path, multi-cluster
directories, bitmap/reservation/tail behaviour, root reports, cache eviction, truncation,
cancellation, write-back and source verification, reload and stop/start.

The lazy suite covers FAT and ext4 only; `storage_proxy_matrix` is the exFAT coverage.

## UI renderer

`ui_sim` is built with the host tests but is not a registered CTest. It renders the real LVGL UI and
interaction states to PPM files in `/tmp/ui-*.ppm`:

```sh
software/build/host/tests/host/ui_sim
```

It is a development tool for checking layout and state changes without the board.

## WebAssembly simulator

`software/tests/wasm/` compiles the same UI sources with Emscripten. The docs build runs it and the
site mounts it on the [live simulator](../guide/simulator.md) page. It has no RTOS, card driver or
USB host; it supplies a fixed card fixture and a request emulator.

## CI

- `ci.yml` runs the host tests on every push and pull request.
- Its docs job runs on non-master branches: it installs Emscripten and Doxygen, builds the site, the
  wasm UI and the API, and runs `npm run check`.
- `docs.yml` runs on `master`, builds the same artifacts, checks links and deploys to GitHub Pages.

A change to a driver should add a host test when the code is register-independent. A UI change
should be checked with `ui_sim` and the browser simulator.
