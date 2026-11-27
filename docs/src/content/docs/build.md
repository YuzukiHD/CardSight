---
title: 构建
description: 交叉工具链、主机测试、发布包，以及文档站里 WebAssembly 模拟器的构建方式。
---

## 需要什么

- RISC-V 工具链：XuanTie-900 GCC elf newlib（V3.2.0 验证过），路径通过环境变量
  `XUANTIE_TOOLCHAIN` 指定，也可以在 CMake 里用 `-DXUANTIE_TOOLCHAIN=...` 覆盖。仓库不写死
  本机路径。
- 主机测试只需要 CMake ≥ 3.20 和一个 C 编译器。
- 文档站的模拟器需要 Emscripten。

第三方组件全部是 git submodule，先拉下来：

```sh
git submodule update --init --recursive
```

## 固件

```sh
export XUANTIE_TOOLCHAIN=/path/to/Xuantie-900-gcc-elf-newlib-x86_64-V3.2.0
cd software
cmake --preset riscv
cmake --build build/riscv
```

产物在 `software/build/riscv/images/`：

| 文件 | 说明 |
| --- | --- |
| `cardsight.bin` | C907 上的应用镜像，链接到 `0x12000000` |
| `init_psram_efex.bin` | E907 上初始化 PSRAM，完成后返回 FEL |
| `c907_boot_efex.bin` | E907 启动 C907，入口 `0x12000000` |

两个 eFEX 由 `software/third_party/SyterKit` 的 `yuzuki-cardsight` 板配置构建，
CMake 目标叫 `efex_stubs`，会随默认构建一起生成。烧写步骤见[烧写与启动](/CardSight/fel/)。

构建开关：

| 变量 | 默认 | 作用 |
| --- | --- | --- |
| `CARDSIGHT_WITH_APP` | ON | 是否编译应用和 LVGL |
| `CARDSIGHT_HOST_TESTS` | OFF | 只构建主机测试 |
| `CARDSIGHT_BUILD_EFEX` | ON | 是否构建两个 eFEX |
| `CARDSIGHT_WERROR` | OFF | 把自有代码的警告变成错误 |

## 主机测试

块层、VFS、FatFs、lwext4 和两个存储代理引擎不依赖板级寄存器，可以在 PC 上直接编译。测试用
RAM 盘跑完整的存储代理矩阵：

```sh
cd software
cmake --preset host
cmake --build build/host
ctest --test-dir build/host --output-on-failure
```

两个测试：

- `storage_proxy_matrix`：FAT32、exFAT、ext4、混合分区、GPT 布局、VFS。
- `lazy_proxy`：懒索引代理的读、写、覆盖、删除、容量报告、取消和限制。

另有 `build/host/tests/host/ui_sim`，把真实 LVGL 界面渲染成 PPM（写到 `/tmp/ui-*.ppm`），
用于看页面和交互状态，不注册成测试。

## 发布源码包

```sh
./software/scripts/release.sh 0.1.0
# dist/cardsight-0.1.0.tar.gz
```

脚本不打包构建产物和厂家预编译库，保留许可证与子模块引用。

## 文档站与 WebAssembly 模拟器

`docs/` 是一个独立的 Astro/Starlight 项目。`npm run dev` 或 `npm run build` 会先执行
`docs/scripts/build-wasm.mjs`：

```sh
# 一次性的 Emscripten 安装
git clone https://github.com/emscripten-core/emsdk.git ~/emsdk
~/emsdk/emsdk install latest
~/emsdk/emsdk activate latest
source ~/emsdk/emsdk_env.sh

cd docs
npm ci
npm run build      # wasm + 静态站点，输出到 docs/dist/
```

模拟器的源码在 `software/tests/wasm/`，CMake 工程独立于固件构建，编译这些文件：

- LVGL 的 core、widgets、字体，以及 `software/apps/cardsight/ui/` 下的页面、控件和材质；
- 固件里同一套字体和背景图（`fonts/`、`assets/`）；
- `ui_web.c`，提供内存 framebuffer、指针输入、固定 32 GB 卡模型和请求模拟。

Emscripten 的输出 `cardsight-ui.js` 和 `cardsight-ui.wasm` 复制到 `docs/public/ui/`，
不进 git。只改 Markdown 时可以用已有构建跳过 wasm：

```sh
CARDSIGHT_SKIP_WASM=1 npm run build
```

CI 在 push 和 PR 上跑主机测试；分支和 PR 还会构建一次文档站，master 的 `docs` 工作流负责构建
并部署到 GitHub Pages。
