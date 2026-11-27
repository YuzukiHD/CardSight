# CardSight

CardSight is the camera/storage application for the Yuzuki CardSight board (V861/V881, C907): a
small-screen UI for partition management, formatting and a USB storage proxy. The application was
extracted from SyterOS and paired with a new minimal platform layer written for this board; the
repository is released under **GPL-2.0-or-later**.

- Application: `software/apps/cardsight/` (LVGL UI, partitions/formatting, exFAT view proxy)
- Platform: `software/src/` (boot, FreeRTOS glue, CCU/pins, MMC/SD, block layer, VFS, FatFs/lwext4,
  SPI panel, touch, USB MSC)
- Board description: `software/board/` (board C arrays/structs and linker script, no device tree)
- Third parties: `software/third_party/` (all git submodules: FreeRTOS, LVGL, CherryUSB, lwext4,
  FatFs, SyterKit)
- Hardware design: `hardware/` (schematics, PCB, BOM)
- Documentation: <https://yuzukihd.github.io/CardSight/> (sources in `docs/`, including an
  interactive WebAssembly build of the UI)

## Verified on the board

Validated on a real Yuzuki CardSight booting from RAM through FEL (image about 770 KiB):

- Kernel: FreeRTOS V11.3.3 plus a custom RISC-V boot path, PLIC and CLINT; UART console and shell,
  the `INITCALL` chain, newlib, and coloured timestamped logs in the same format as SyterKit.
- Storage: the SD card identifies at **SDR104 200 MHz, four-bit, CMD19 tuning**; MBR/GPT parsing,
  FatFs (FAT/exFAT) and lwext4 (ext4), formatting and VFS mounts. The storage proxy builds an exFAT
  view, and host writes go back to the source partitions through overlay and staging. Reads into
  on-chip SRAM measure **62 MiB/s**; reads into PSRAM measure about 9.6 MiB/s (a PSRAM write-path
  limit, see [Hardware](https://yuzukihd.github.io/CardSight/hardware/)).
- Display/touch: ST7789 240×284 output, with colours (red, green, blue, white, black) and the UI
  confirmed on the real panel; LVGL 9.6 refreshes without errors; CST816D polled touch initialises;
  `cardsight ui-test` PASS. Key fixes: GPIO output direction (DC/RST), the SPI DHB/DDB/SDM bits, and
  high-byte-first pixel order.
- USB: MUSB + CherryUSB MSC; `cardsight usb on` exports the proxy view (read-write), Windows
  enumerates it as `VID_1D6B/PID_0105`, mounts the exFAT volume and reads directories and files
  (the same file read across three enumerations has the same SHA256); `cardsight usb disk` exports
  the whole disk read-only, and Windows reads the real GPT and FAT32 partitions and their files;
  `cardsight usb-test` PASS.
- Hot-plug: the driver has a hot-plug task (identify and publish `mmcblk0` on insertion, remove the
  disk on removal), and the application stops USB and the proxy when the disk disappears. **A real
  eject/reinsert** while a USB export was running has been verified: USB and the proxy stop safely,
  and reinsertion identifies the card at SDR104 200 MHz again; `mmc soak 20` also ran 20
  remove/re-identify cycles with zero failures.
- Host: `ctest` storage-proxy matrix PASS (FAT32/exFAT/ext4/mixed partitions, GPT layouts, VFS).

The board's known limits are in [Hardware](https://yuzukihd.github.io/CardSight/hardware/): SMHC
module-clock mux 2 is unusable (so the top mode is 200 MHz, not 150/208 MHz); PSRAM writes run at
about 10 MB/s and on-chip SRAM is the high-speed DMA target. If unflushed data remains on the card
when it is removed, the proxy reports `flush failed` (the data cannot be written back); that is the
only unavoidable loss on removal.

## Build

The RISC-V toolchain path is supplied through the `XUANTIE_TOOLCHAIN` environment variable (or
`-DXUANTIE_TOOLCHAIN=...`); the repository does not hard-code a local path.

```sh
export XUANTIE_TOOLCHAIN=/path/to/Xuantie-900-gcc-elf-newlib-x86_64-V3.2.0
git submodule update --init --recursive   # every third-party component is a submodule
cd software
cmake --preset riscv
cmake --build build/riscv
# output: build/riscv/images/cardsight.bin
#         build/riscv/images/init_psram_efex.bin
#         build/riscv/images/c907_boot_efex.bin   (built by the SyterKit submodule)
```

Host tests (no hardware needed):

```sh
cd software
cmake --preset host
cmake --build build/host
ctest --test-dir build/host --output-on-failure
```

Source release tarball (no build output or vendor prebuilt libraries; licenses included):

```sh
./software/scripts/release.sh 0.1.0     # output: dist/cardsight-0.1.0.tar.gz
```

Documentation site (optional; Emscripten is needed for the WebAssembly UI simulator; see
`docs/README.md`):

```sh
cd docs
npm ci
npm run build        # builds the wasm first, then writes docs/dist/
npm run dev          # http://localhost:4321/CardSight/
```

## Flashing and boot (FEL)

```sh
xfel write 0x00100000 software/build/riscv/images/init_psram_efex.bin
xfel exec  0x00100000          # E907 initialises PSRAM and returns to FEL
xfel write 0x12000000 software/build/riscv/images/cardsight.bin
xfel write 0x00100000 software/build/riscv/images/c907_boot_efex.bin
xfel exec  0x00100000          # E907 starts the C907 (entry = 0x12000000)
```

Serial: UART0, PH13/PH14, 115200 8N1. The complete three-step chain, FEL constraints and the eFEX
details are in [Flashing and boot](https://yuzukihd.github.io/CardSight/fel/).

## Usage

After boot the prompt is `cardsight>`, and the application draws its UI on the panel at the same
time (touch operated).

| Command | Effect |
|---|---|
| `help` | All commands |
| `blk` / `blkread <dev> <sector>` / `blkbench <dev> <sector> <MiB>` | Block device list, read a sector, sequential read benchmark |
| `mmc` | SD card information (capacity, mode, clock, tuning state) |
| `mmc eject` / `insert` / `soak [n]` | Driver hot-plug path (simulated removal, re-identification, soak) |
| `mount [blk target [fstype]]` / `umount` / `ls` / `cat` / `df` / `write` | VFS |
| `display color <rgb565>` | Fill the panel (display self-test) |
| `lvgl [reset\|bench]` | LVGL refresh statistics / redraw benchmark |
| `lazyproxy start <disk> [mask]` / `stop` / `usb on\|rw\|off` / `status` | Lazy-index exFAT proxy `cardview1` used by the UI |
| `storageproxy start <disk> [mask]` / `stop` / `status` | Full-index exFAT proxy `cardview0` (diagnostics) |
| `cardsight page <direct\|proxy\|disk\|info\|layout>` | Switch the UI page |
| `cardsight ui-test` | UI self-test |
| `cardsight usb-test` | Read-only USB lifecycle self-test |
| `cardsight usb on\|off\|disk` | USB export of the proxy view (read-write), stop, or read-only whole-disk export |

USB: after `lazyproxy start mmcblk0 0x3` and `lazyproxy usb rw`, connect the board's OTG port to a
host; Windows identifies it as `CardSight SD Card` and mounts the exFAT volume. Host writes go back
to the source partitions through overlay and staging; `lazyproxy usb on` exports read-only instead.
`cardsight usb disk` exports the whole card read-only for checking the real partition table. Stop
with `lazyproxy usb off` / `lazyproxy stop`, or with "Stop safely" in the UI.

## Layout

| Path | Contents |
|---|---|
| `software/` | Software project root: CMake, sources, application, tests, patches |
| `software/apps/cardsight/` | Application (UI, storage proxy, fonts) |
| `software/src/` | Platform implementation; drivers split by subsystem (SPI, display, touch, I2C; MMC in host/core/card) |
| `software/include/` | Platform API (`vfs.h`, `hal/*`, `usb/mass_storage.h`, `gui/lvgl_port.h`) |
| `software/tests/host/` | Host tests (storage-proxy matrix, VFS, partition layout, UI) |
| `software/patches/` | Board patches for upstream components (CherryUSB, lwext4) |
| `software/third_party/` | Third-party submodules (FreeRTOS, LVGL, CherryUSB, lwext4, FatFs, SyterKit) |
| `software/board/` | Board constant arrays (`cardsight_board.c/.h`) and linker script (`cardsight.ld`) |
| `hardware/` | Hardware design files (schematics, PCB, BOM) |
| `docs/` | Astro/Starlight documentation site, including the WebAssembly UI simulator build |
| `software/tests/wasm/` | Simulator sources that compile the real UI with Emscripten |
| `dist/` | Release output (not tracked) |

## License

The project is released under GPL-2.0-or-later; third-party licenses are listed in
[THIRD_PARTY.md](THIRD_PARTY.md) and the license files in each directory. The fonts are generated
from Source Sans and shipped under SIL OFL 1.1 (`software/apps/cardsight/FONT_LICENSE.txt`).

> Note: `software/third_party/SyterKit` is pinned to a commit in the public repository that contains
> the board support (`boards/yuzuki-cardsight`). When changing the submodule pointer, make sure the
> commit has been pushed upstream; otherwise nobody else can initialise the submodule. See
> [License](https://yuzukihd.github.io/CardSight/licensing/).
