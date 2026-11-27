# CardSight

CardSight 是 Yuzuki CardSight（V861/V881，C907）板上的相机/存储应用：一块小屏幕上的分区管理、
格式化与 USB 存储代理界面。本仓库把应用从 SyterOS 中抽出，配一套为这块板子新写的极简底层，
以 **GPL-2.0-or-later** 开源。

- 应用：`software/apps/cardsight/`（LVGL 界面、分区/格式化、exFAT 视图代理）
- 底层：`software/src/`（启动、FreeRTOS 胶水、CCU/引脚、MMC/SD、块层、VFS、FatFs/lwext4、SPI 屏、触摸、USB MSC）
- 板级描述：`software/board/`（板级 C 数组/结构体与链接脚本，不使用设备树）
- 第三方：`software/third_party/`（全部是 git submodule：FreeRTOS、LVGL、CherryUSB、lwext4、FatFs、SyterKit）
- 硬件设计：`hardware/`（原理图、PCB、BOM）
- 文档：<https://yuzukihd.github.io/CardSight/>（源码在 `docs/`，含可交互的界面 WebAssembly 实例）

## 板上验证状态

在真实的 Yuzuki CardSight 上以 RAM/FEL 启动验证（镜像约 770 KiB）：

- 内核：FreeRTOS V11.3.3 + 自写 RISC-V 启动/PLIC/CLINT；UART 控制台与 shell、
  `INITCALL` 初始化链、newlib、与 SyterKit 相同格式的彩色时间戳日志。
- 存储：SD 卡识别到 **SDR104 200 MHz、四位、CMD19 调谐**；MBR/GPT 解析、
  FatFs（FAT/exFAT）与 lwext4（ext4）、格式化、VFS 挂载；存储代理可建立 exFAT 视图，
  主机写入经 overlay 和 staging 写回源分区。读入片上 SRAM 的实测为 **62 MiB/s**；读入
  PSRAM 约 9.6 MiB/s（PSRAM 写入路径限制，见[硬件说明](https://yuzukihd.github.io/CardSight/hardware/)）。
- 显示/触摸：ST7789 240×284 出图，颜色（红绿蓝白黑）与界面在实屏上确认正常；LVGL 9.6
  刷新无错误；CST816D 轮询触摸初始化成功；`cardsight ui-test` PASS。关键修复：GPIO 输出
  方向（DC/RST）、SPI 的 DHB/DDB/SDM 位、以及像素高字节在前的字节序。
- USB：MUSB + CherryUSB MSC；`cardsight usb on` 导出代理视图（读写），Windows 枚举为
  `VID_1D6B/PID_0105`，挂载 exFAT 卷并读到目录与文件（同一文件三次枚举读到的 SHA256 一致）；
  `cardsight usb disk` 导出整盘（只读），Windows 读到真实 GPT 与 FAT32 分区并读出文件；
  `cardsight usb-test` PASS。
- 拔插：驱动有热插拔任务（插入识别并发布 mmcblk0，拔出撤下磁盘），应用检测到磁盘消失后
  自动停止 USB 与存储代理。**真实拔卡/插回**（USB 导出运行中）已验证：USB 与代理安全停止，
  插回后重新识别到 SDR104 200 MHz；`mmc soak 20` 另外跑 20 次移除/重新识别 0 失败。
- 主机：`ctest` 存储代理矩阵 PASS（FAT32/exFAT/ext4/混合分区、GPT 布局、VFS）。

板子的已知限制写在[硬件说明](https://yuzukihd.github.io/CardSight/hardware/)：SMHC 模块时钟
mux 2 不可用（所以最高档是 200 MHz 而不是 150/208）；PSRAM 写入约 10 MB/s；片上 SRAM 才是
高速 DMA 的目标。拔卡后若卡上还有未刷写的数据，代理会报 `flush failed`（数据无法写回），
这是拔出时唯一无法避免的损失。

## 构建

需要 RISC-V 工具链，路径由环境变量 `XUANTIE_TOOLCHAIN` 指定（也可用
`-DXUANTIE_TOOLCHAIN=...` 覆盖）；仓库不写死本机路径。

```sh
export XUANTIE_TOOLCHAIN=/path/to/Xuantie-900-gcc-elf-newlib-x86_64-V3.2.0
git submodule update --init --recursive   # 全部第三方组件都是 submodule
cd software
cmake --preset riscv
cmake --build build/riscv
# 产物：build/riscv/images/cardsight.bin
#       build/riscv/images/init_psram_efex.bin
#       build/riscv/images/c907_boot_efex.bin   （由 SyterKit submodule 构建）
```

主机测试（无需硬件）：

```sh
cd software
cmake --preset host
cmake --build build/host
ctest --test-dir build/host --output-on-failure
```

发布源码包（不含构建产物与厂家预编译库，包含许可证）：

```sh
./software/scripts/release.sh 0.1.0     # 产物：dist/cardsight-0.1.0.tar.gz
```

文档站（可选，需要 Emscripten 来编译界面 WebAssembly 模拟器；步骤见 `docs/README.md`）：

```sh
cd docs
npm ci
npm run build        # 先构建 wasm，再输出到 docs/dist/
npm run dev          # http://localhost:4321/CardSight/
```

## 烧写与启动（FEL）

```sh
xfel write 0x00100000 software/build/riscv/images/init_psram_efex.bin
xfel exec  0x00100000          # E907 初始化 PSRAM，返回 FEL
xfel write 0x12000000 software/build/riscv/images/cardsight.bin
xfel write 0x00100000 software/build/riscv/images/c907_boot_efex.bin
xfel exec  0x00100000          # E907 启动 C907（entry = 0x12000000）
```

串口：UART0，PH13/PH14，115200 8N1。更完整的三步链、FEL 约束与 eFEX 说明见
[烧写与启动](https://yuzukihd.github.io/CardSight/fel/)。

## 使用

启动后是 `cardsight>` 命令行；应用同时把界面画在屏上（触摸操作）。

| 命令 | 作用 |
|---|---|
| `help` | 全部命令 |
| `blk` / `blkread <dev> <sector>` / `blkbench <dev> <sector> <MiB>` | 块设备列表、读扇区、顺序读测速 |
| `mmc` | SD 卡信息（容量、模式、时钟、调谐状态） |
| `mmc eject` / `insert` / `soak [n]` | 驱动热插拔路径（模拟移除、重新识别、批量 soak） |
| `mount [blk target [fstype]]` / `umount` / `ls` / `cat` / `df` / `write` | VFS |
| `display color <rgb565>` | 整屏填色（屏自检） |
| `lvgl [reset\|bench]` | LVGL 刷新统计 / 重绘基准 |
| `lazyproxy start <disk> [mask]` / `stop` / `usb on\|rw\|off` / `status` | 界面使用的懒索引 exFAT 代理 `cardview1` |
| `storageproxy start <disk> [mask]` / `stop` / `status` | 全量索引 exFAT 代理 `cardview0`（诊断用） |
| `cardsight page <direct\|proxy\|disk\|info\|layout>` | 切换界面页 |
| `cardsight ui-test` | 界面自检 |
| `cardsight usb-test` | USB 只读生命周期自检 |
| `cardsight usb on\|off\|disk` | 代理视图的 USB 导出（读写）、停止，或只读导出整盘 |

USB：`lazyproxy start mmcblk0 0x3` 和 `lazyproxy usb rw` 之后把板子的 OTG 口接到主机；
Windows 会识别为 `CardSight SD Card` 并挂载 exFAT 卷。主机的写入经 overlay 和 staging 写回
源分区；`lazyproxy usb on` 则是只读导出。`cardsight usb disk` 只读导出整卡，用于核对真实
分区表。停止用 `lazyproxy usb off` / `lazyproxy stop`，或界面里的“安全停止”。

## 目录

| 路径 | 说明 |
|---|---|
| `software/` | 软件工程根：CMake、源码、应用、测试、补丁 |
| `software/apps/cardsight/` | 应用（UI、存储代理、字体） |
| `software/src/` | 平台实现；驱动按子系统拆分（SPI、显示、触摸、I2C，MMC 分 host/core/card） |
| `software/include/` | 平台 API（`vfs.h`、`hal/*`、`usb/mass_storage.h`、`gui/lvgl_port.h`） |
| `software/tests/host/` | 主机测试（存储代理矩阵、VFS、分区布局、UI） |
| `software/patches/` | 上游组件的板级补丁（CherryUSB、lwext4） |
| `software/third_party/` | 第三方 submodule（FreeRTOS、LVGL、CherryUSB、lwext4、FatFs、SyterKit） |
| `software/board/` | 板级常量数组（`cardsight_board.c/.h`）与链接脚本（`cardsight.ld`） |
| `hardware/` | 硬件设计资料（原理图、PCB、BOM） |
| `docs/` | Astro/Starlight 文档站，含界面 WebAssembly 模拟器的构建脚本 |
| `software/tests/wasm/` | 用 Emscripten 编译真实 UI 的模拟器源码 |
| `dist/` | 发布产物（不入库） |

## 许可

本项目以 GPL-2.0-or-later 发布；第三方组件的许可见 [THIRD_PARTY.md](THIRD_PARTY.md)
与各自目录下的许可证文件。字体来自 Source Sans，按 SIL OFL 1.1 随附
（`software/apps/cardsight/FONT_LICENSE.txt`）。

> 注意：`software/third_party/SyterKit` 固定在公开仓库中包含本板支持
> （`boards/yuzuki-cardsight`）的提交上。改子模块指针时要确认该提交已经推到上游，否则外部
> 无法初始化子模块。详见[许可](https://yuzukihd.github.io/CardSight/licensing/)。
