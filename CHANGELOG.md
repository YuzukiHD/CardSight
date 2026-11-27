# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project uses
[Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- Contributor Covenant code of conduct, contribution guide, security policy, support guide and
  GitHub issue/pull-request templates.
- Reorganized documentation: getting started, user guide, reference, development and project pages;
  a Doxygen-generated C API reference and a link checker in CI.

### Changed

- The root README is now a short entry page; details live in the documentation site.
- Documentation is English only.

### Fixed

- Corrected hardware documentation for the panel pixel format, SPI clock, heap sizes and storage
  engine differences.
- `cardsight` shell help now lists `usb <on|off|disk>`.
- Source release tarballs exclude documentation build output and generated API assets.

## [0.1.0] - 2026-11-27

Initial public source snapshot.

### Added

- TF card proxy firmware for the Yuzuki CardSight board (Allwinner V861/V881, XuanTie C907).
- Five-page LVGL touch UI: Direct, Proxy, Disk, Files and Info.
- Block layer, MBR/GPT parsing, VFS, FatFs (FAT/exFAT) and lwext4 (ext4).
- Lazy-index exFAT proxy (`cardview1`) with write-back, and a full-index proxy (`cardview0`) for
  diagnostics.
- USB mass-storage device through MUSB and CherryUSB MSC.
- Serial shell for block devices, filesystems, MMC, USB, storage proxy and UI diagnostics.
- Host tests using RAM disks, plus a UI renderer and a WebAssembly simulator.
- Astro/Starlight documentation site.

### Known limitations

- The SMHC module-clock path used for the highest SD timing is avoided on this board; the driver
  uses the supported SDR104/SDR50/SDR25 modes instead.
- PSRAM is the slower memory path; on-chip SRAM is used for the USB and display buffers.
- If the TF card is removed while the storage proxy still has unflushed writes, that data is lost
  and the proxy reports a flush failure.
- Hardware design files (schematics, PCB, BOM) are not published yet.
