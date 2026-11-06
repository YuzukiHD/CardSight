# 硬件说明

一块 Yuzuki CardSight：Allwinner V861/V881（`sun252iw1`），C907 RISC-V 核（RV32，
`rv32imafdc_zicsr_zifencei`，ilp32d），另有 E907 小核负责上电初期的内存初始化。所有硬件
信息都在 `software/board/cardsight_board.c` 的常量数组里，不存在设备树。

## 内存

| 区域 | 地址 | 大小 | 用途 |
|---|---|---|---|
| 片上 SRAM | `0x00100000` | 128 KiB | 低 64 KiB 保留给 USB MSC 传输缓冲（`CS_MSC_BUFFER_*`）；eFEX 也从这里运行 |
| PSRAM | `0x12000000` | 8 MiB | 镜像、`.bss`、堆；镜像链接在 `0x12000000` |
| DRAM | `0x40000000` | — | 本项目的启动流程不初始化 DRAM，不使用 |

新写的内核堆是 PSRAM 末尾的 FreeRTOS `heap_4`（2 MiB），newlib 堆从镜像末尾到 PSRAM 末尾
（约 5 MiB）。链接脚本在镜像放不下时直接报错。

## 时钟与中断

- 时钟控制：CCU `0x02001000`。总线门控与复位按寄存器 + 位号直接写：UART0 `0x090c`（门 0），
  SMHC0 `0x084c`（门 0、复位 16），SPI1 `0x096c`（门 1、复位 17），TWI0 `0x091c`（门 0、
  复位 16），USB PHY `0x0a70`（门 31、复位 30），USB OTG `0x0a8c`（门 8、复位 24）。
- 定时器：CLINT `0x34000000`，mtime `+0xbff8` 24 MHz；FreeRTOS 的机器定时器比较寄存器
  `+0x4000`。`ktime_get_us()` 直接读 mtime。
- 中断：PLIC `0x30000000`，hart0 机器模式上下文（见 `software/src/arch/plic.c`）。板级表里的编号是
  PLIC 源号；SoC 手册/HANDOFF 里的 CLIC 编号比它大 16（UART0 CLIC 18 → PLIC 2）。

## 外设

| 外设 | 基址 | 中断 | 备注 |
|---|---|---|---|
| UART0 | `0x02500000` | 2 | PH13/PH14，mux 6，115200；TX 轮询、RX 中断入队 |
| SMHC0（SD） | `0x04020000` | — | PF2 时钟、PF3 CMD、PF0/1/4/5 D0-D3，mux 2，CD=PF6 低有效；PIO 命令 + IDMAC 数据 |
| SPI1（屏） | `0x04026000` | — | PD1/2/3 mux 6；DC=PD4、RST=PD5；60 MHz（300M/5） |
| TWI0（触摸） | `0x02502000` | — | PE4/PE5 mux 8，400 kHz；CST816 @0x15，复位 PE2 |
| USB OTG | `0x04100000` | 29 | PHY 胶合 `0x04100400`，HCI PHY `0x04101800`；device 模式 |

### SD 卡

- 1.8 V 信号由 PMIC 内部的端口 F 电源开关切换：GPIO `+0x390` 选择（1=3.3 V，0=1.8 V），
  `+0x38c` 第 5 位报告状态。切到 1.8 V 的卡在断电前一直保持 1.8 V，冷启动时先按 3.3 V
  识别，失败再按 1.8 V 重试。
- 初始化：CMD0/CMD8/ACMD41（带 S18R）→ 接受则 CMD11 切 1.8 V → CMD2/3/9/7 → ACMD6 四位
  → SCR/CMD6 → 选最快模式。
- 速度模式（板级上限 200 MHz）：SDR104 200 MHz / 100 MHz、SDR50 100 MHz、SDR25 50 MHz。
  **不要使用 SMHC 模块时钟的 mux 2**：在该板上它不能让卡正常响应命令（150 MHz 调谐全失败），
  200 MHz 用 mux 1（400 MHz PLL）的 M=1 直接得到。
- 数据采样点：CMD19 调谐，3 个相位 × 64 个采样延迟；窗口盖满整张网格时保留接口默认点
  （相位 0、延迟 0）。失败后自动复位控制器、重新调谐并重试一次。
- 热插拔：一个低优先级任务每 500 ms 读一次 CD（PF6）；插入时识别并发布 `mmcblk0`，
  拔出时撤下磁盘（`del_gendisk_force`）。应用的 worker 发现磁盘消失后停止 USB 设备与
  存储代理。`mmc eject/insert/soak` 是驱动自己的测试入口：`eject` 只把 CD 状态强制为
  “不在位”，卡仍在槽里且不断电，所以重新识别后可能停在 SDR25；真实拔插会断电，重新
  插入仍走 3.3 V → CMD11 → SDR104。`mmc soak 20` 在板上为 20 次 0 失败。
- 实测（SK32G 32 GB，SDR104 200 MHz 四位）：
  - 读入片上 SRAM：**62 MiB/s**（控制器/卡的上限）；
  - 读入 PSRAM：约 **9.6 MiB/s**——瓶颈是 PSRAM 写入路径本身（CPU `memcpy` 同样约 10 MB/s），
    不是 SD 控制器；
  - 400 kHz 下 DMA 效率约 97%（189 KiB/s），确认时钟与协议本身正常。

### 显示与触摸

- ST7789 兼容屏，240×284，RGB565，SPI 模式 0，像素高字节在前（`pixels_swapped`）。
  MADCTL `0x48`（mirror-x + BGR）。初始化表在 `software/src/drivers/display/panel-st7789.c`。
  DC/RST 是普通 GPIO 输出（mux 1），SPI 控制器要置 DHB/DDB/SDM 位；漏掉任何一项都会
  出现全黑或颜色错乱（曾出现整屏黑与“发绿显红”）。
- CST816D 电容触摸，I2C 轮询（无中断/工作线程），6 字节报文从寄存器 `0x01` 读取，X 方向按
  屏幕镜像；LVGL 输入设备每次回调读一帧。
- LVGL 9.6：40 行局部缓冲，flush 直接写屏；`lvgl` 命令输出刷新次数/像素/耗时。

### USB

- MUSB 设备控制器，走 CherryUSB（设备栈 + MSC 类）。PHY 胶合寄存器：`ISCR 0x00`、
  `PHYCTRL 0x10`、`PHYSEL 0x20`（相对 `0x04100400`）；`VEND0 0x43` 写 PIO 位。
- 设备角色不接 ID/VBUS 比较器：强制 B 设备、VBUS 有效。
- 传输缓冲用保留的 SRAM 块 `0x00100000`（64 KiB），MSC 类通过
  `CONFIG_USBDEV_MSC_EXTERNAL_BUFFER` 取用。
- Windows 10/11 实测：枚举为 `VID_1D6B/PID_0105`，挂载为 exFAT 卷，可读目录与文件；
  导出代理视图时为只读，直连整盘视图时才可写。
