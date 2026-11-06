# 启动链（FEL）

本板开发期只用 RAM/FEL 启动，不写 flash，也不擦除或改写 SD 卡。上电后 BROM 等待 FEL
（板上的 NOR 没有可启动固件）；同一时间只允许一块板处于 FEL。

## 三步加载

```sh
# 1. E907 初始化 PSRAM，完成后返回 FEL
xfel write 0x00100000 software/build/riscv/images/init_psram_efex.bin
xfel exec  0x00100000

# 2. C907 镜像（链接地址 0x12000000）
xfel write 0x12000000 software/build/riscv/images/cardsight.bin

# 3. E907 启动 C907，入口 0x12000000
xfel write 0x00100000 software/build/riscv/images/c907_boot_efex.bin
xfel exec  0x00100000
```

两个 eFEX 由 `software/third_party/SyterKit` 里的 `yuzuki-cardsight` 板配置构建（`software/cmake/syterkit.cmake`）：

- `init_psram_efex.bin`：E907 上初始化 8 MiB PSRAM（SQPI 100 MHz），UART 打印
  `PSRAM ready; returning to FEL for image download at 12000000` 后回到 FEL；
- `c907_boot_efex.bin`：E907 上释放 C907，等 UART 发完再跳转。

串口为 UART0（PH13/PH14），115200 8N1。镜像在串口上打印与 SyterKit 相同格式的日志
（`[秒.微秒][级别] 模块: 消息`，级别带颜色）。

## 注意

- eFEX 在 SRAM `0x00100000` 运行；镜像里保留低 64 KiB 作 USB MSC 缓冲，eFEX 已经跳走
  后该区域才归应用使用。
- `xfel` 写入 PSRAM 期间不要同时让另一块板处于 FEL，也不要在写 PSRAM 时断电。
- 不要用本项目的流程写 SPI NOR 或修改 flash 分区；本仓库不包含 bootloader 写入。
