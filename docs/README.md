# CardSight documentation site

Built with [Starlight](https://starlight.astro.build) (Astro) and published on GitHub Pages:
<https://yuzukihd.github.io/CardSight/>.

The layout follows the YuzukiNeko and lessampler docs: `docs/` is a standalone Astro project, the
pages live in `src/content/docs/`, the sidebar is `sidebar.mjs`, and the build-time plugin is
`src/plugins/remark-docs.mjs`.

The landing page and the "On-screen UI" page embed a **WebAssembly build of the real UI**:
`software/tests/wasm/` compiles LVGL, the five pages, the widgets and the glass material to wasm, so
touching, dragging and scrolling in the browser run the same code as the firmware. It is not a
separate web mock-up.

## Local preview

```sh
# 1. Submodules (the wasm build needs the LVGL sources)
git submodule update --init --recursive

# 2. Emscripten
git clone https://github.com/emscripten-core/emsdk.git ~/emsdk
~/emsdk/emsdk install latest
~/emsdk/emsdk activate latest
source ~/emsdk/emsdk_env.sh

# 3. Build the site
cd docs
npm ci
npm run dev      # http://localhost:4321/CardSight/
npm run build    # builds the wasm first, then writes dist/
npm run preview
```

`npm run dev` and `npm run build` first run `scripts/build-wasm.mjs`, which configures
`software/tests/wasm/` with `emcmake`, compiles LVGL and the UI, and copies `cardsight-ui.js` and
`.wasm` into `public/ui/`. Those two files (and `public/mermaid.min.js`) are build output and are
not tracked by git.

To work on Markdown only, skip the wasm build and reuse an existing `public/ui/`:

```sh
CARDSIGHT_SKIP_WASM=1 npm run build
```

The first build still needs a full `npm run build` to create `public/ui/`.

## Layout

| Path | Contents |
| --- | --- |
| `src/content/docs/` | Pages (Markdown/MDX), each with a `title` in its frontmatter |
| `src/components/Simulator.astro` | Simulator container and controls |
| `public/demo/cardsight-demo.js` | Browser wrapper: instantiates the wasm, forwards pointer input, draws RGB565 frames on a canvas |
| `public/enhance.js` | Loaded on every page: draws mermaid and mounts the simulator |
| `src/plugins/remark-docs.mjs` | Build-time plugin: mermaid blocks, `.md` links, `assets/` prefix |
| `src/styles/custom.css` | Site styles, including the simulator |
| `sidebar.mjs` | Sidebar |
| `astro.config.mjs` | Site configuration; `site` and `base` match the GitHub Pages address |

## Publishing

`.github/workflows/docs.yml` builds the site and the wasm on master, then deploys to GitHub Pages.
The repository needs **Settings → Pages → Build and deployment → Source** set to **GitHub Actions**
once. While Pages is not enabled, the `docs` workflow still builds and skips the deploy step, so CI
stays green.

## Adding a page

1. Create a `.md` file under `src/content/docs/` with frontmatter:
   ```md
   ---
   title: Page title
   description: One-line summary
   ---
   ```
2. Add its slug (relative path without `.md`) to the matching group in `sidebar.mjs`.
3. Run `npm run build` locally and check that there is no `The slug ... does not exist` error.
