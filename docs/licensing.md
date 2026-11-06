# 许可

本仓库（`cardsight/`）以 **GPL-2.0-or-later** 发布，见根目录 `LICENSE`。

新写的平台代码（`src/`、`board/`、`include/`、`cmake/`）与抽取出的应用（`apps/cardsight/`）
使用 GPL-2.0-or-later；应用原属 SyterOS（MIT），本轮按项目要求改为 GPL-2.0-or-later，
不再保留 MIT 声明。

## 为什么是 GPL

- lwext4 为 GPL-2.0；CherryUSB 为 Apache-2.0。Apache-2.0 与 GPL-2.0-or-later 兼容，
  但若改成 GPL-2.0-only，需要替换 CherryUSB 或取得例外授权，因此选择
  **GPL-2.0-or-later**。
- 第三方组件按各自许可随附，详见 `THIRD_PARTY.md` 与各目录下的许可证文件。

## 第三方

| 组件 | 许可 | 说明 |
|---|---|---|
| FreeRTOS Kernel | MIT | submodule（V11.3.3），上游未修改 |
| LVGL | MIT | submodule（v9.6.0），仅选编需要的文件 |
| CherryUSB | Apache-2.0 | submodule（v1.6.1）+ 本板补丁 patches/cherryusb/ |
| lwext4 | GPL-2.0 | submodule（master 最新提交） |
| FatFs | ChaN 许可 | submodule（R0.16）；配置文件 src/fs/fatfs/ffconf.h 为本项目提供 |
| SyterKit | GPL-2.0-or-later | submodule；含厂家预编译的 PSRAM/DRAM 初始化库 |
| Source Sans 字体（生成的字模） | SIL OFL 1.1 | 随附 `apps/cardsight/FONT_LICENSE.txt` |

## 发布注意

- `third_party/SyterKit` 子模块固定在一个含本板支持（`boards/yuzuki-cardsight`）的提交上；
  该提交必须先出现在公开仓库里，外部才能 clone。源码 tarball 不重新打包
  `third_party/SyterKit/soc/sun252iw1/lib/` 下的厂家二进制，只保留子模块引用与说明。
- 不随仓库分发任何配置、凭据、传输令牌或本地参考源码。
