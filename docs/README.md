# CardSight 文档站

用 [Starlight](https://starlight.astro.build)（Astro）构建，发布在 GitHub Pages：
<https://yuzukihd.github.io/CardSight/>。

站点结构与 YuzukiNeko、lessampler 的 docs 一致：`docs/` 是一个独立的 Astro 项目，
内容在 `src/content/docs/`，侧边栏在 `sidebar.mjs`，构建期插件在 `src/plugins/remark-docs.mjs`。

首页和「屏幕界面」页里嵌了一个**真实界面的 WebAssembly 实例**：`software/tests/wasm/`
把 LVGL、五个页面、控件和玻璃材质编成 wasm，浏览器里触摸、拖动、滚动都能用。它不是
另写的网页模拟，而是固件里同一份 UI 代码。

## 本地预览

```sh
# 1. 子模块（LVGL 的源码在构建 wasm 时要用）
git submodule update --init --recursive

# 2. Emscripten
git clone https://github.com/emscripten-core/emsdk.git ~/emsdk
~/emsdk/emsdk install latest
~/emsdk/emsdk activate latest
source ~/emsdk/emsdk_env.sh

# 3. 构建站点
cd docs
npm ci
npm run dev      # http://localhost:4321/CardSight/
npm run build    # 先构建 wasm，再输出 dist/
npm run preview
```

`npm run dev` / `npm run build` 会先执行 `scripts/build-wasm.mjs`，用 `emcmake` 配置
`software/tests/wasm/`、编译 LVGL 与 UI，把 `cardsight-ui.js` 和 `.wasm` 复制到
`public/ui/`。这两个文件（以及 `public/mermaid.min.js`）是构建产物，不进 git。

如果只想改 Markdown、不碰模拟器，可以设 `CARDSIGHT_SKIP_WASM=1` 跳过构建并使用已有的
`public/ui/` 文件：

```sh
CARDSIGHT_SKIP_WASM=1 npm run build
```

第一次仍然需要完整跑一次 `npm run build` 生成 `public/ui/`。

## 目录

| 路径 | 内容 |
| --- | --- |
| `src/content/docs/` | 页面（Markdown/MDX），每页开头有 `title` 前置信息 |
| `src/components/Simulator.astro` | 界面模拟器的容器与按钮 |
| `public/demo/cardsight-demo.js` | 浏览器端包装：实例化 wasm、转发指针、把 RGB565 帧画到 canvas |
| `public/enhance.js` | 每个页面加载：画 mermaid、挂载模拟器 |
| `src/plugins/remark-docs.mjs` | 构建期插件：mermaid 代码块、`.md` 链接、`assets/` 前缀 |
| `src/styles/custom.css` | 站点样式，含模拟器的外观 |
| `sidebar.mjs` | 侧边栏 |
| `astro.config.mjs` | 站点配置，`site`/`base` 对应 GitHub Pages 地址 |

## 发布

`.github/workflows/docs.yml` 在 master 上构建站点和 wasm，然后部署到 GitHub Pages。仓库需要
先在 **Settings → Pages → Build and deployment → Source** 选择 **GitHub Actions**；没启用时
`docs` 工作流仍会构建，部署步骤自动跳过，不会把 CI 弄红。

## 添加页面

1. 在 `src/content/docs/` 下新建 `.md`，写 frontmatter：
   ```md
   ---
   title: 页面标题
   description: 一句话说明
   ---
   ```
2. 把 slug（相对路径、不带 `.md`）加进 `sidebar.mjs` 对应的分组。
3. 本地 `npm run build` 确认没有 `The slug ... does not exist`。
