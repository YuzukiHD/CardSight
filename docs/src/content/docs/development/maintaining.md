---
title: Maintaining
description: "Build and publish the documentation, regenerate screenshots and the C API, and cut a source release."
---

## Documentation site

The site is an Astro/Starlight project in `docs/`. It needs:

- Node.js 24 or newer;
- Emscripten, for the WebAssembly UI simulator;
- Doxygen, for the C API reference.

```sh
cd docs
npm ci
npm run build     # copy-vendor + wasm + Doxygen + Astro
npm run check     # internal links and generated API
npm run preview
```

Local development:

```sh
npm run dev
```

The `predev` and `prebuild` steps run `copy-vendor`, then build the simulator, then build the API.
The generated `public/mermaid.min.js`, `public/ui/` and `public/api/` are not tracked by git.

### Skipping the heavy steps

To edit Markdown without Emscripten or Doxygen, keep an existing build and skip the generators:

```sh
CARDSIGHT_SKIP_WASM=1 CARDSIGHT_SKIP_API=1 npm run build
```

Both steps still require the corresponding `public/ui/` or `public/api/` directory from a previous
full build. The first build must run without the skip variables.

### Emscripten

```sh
git clone https://github.com/emscripten-core/emsdk.git ~/emsdk
~/emsdk/emsdk install latest
~/emsdk/emsdk activate latest
source ~/emsdk/emsdk_env.sh
```

The simulator sources are in `software/tests/wasm/`; it compiles the real UI from
`software/apps/cardsight/ui/`, fonts and assets, and writes `cardsight-ui.js` and
`cardsight-ui.wasm` to `docs/public/ui/`. The CMake project is independent of the firmware build.

### Doxygen

Install `doxygen` with the system package manager. `npm run api` reads `docs/Doxyfile`, writes the
HTML to `docs/public/api/html/`, and creates `docs/public/api/index.html` as a small redirect.
The input is the public headers only; private headers are excluded.

## Screenshots

The screenshots in `docs/src/assets/` are rendered from the firmware UI, not photographed from the
panel:

```sh
cd software
cmake --preset host && cmake --build build/host --target ui_sim
./build/host/tests/host/ui_sim    # writes /tmp/ui-*.ppm
```

The PPM output is RGB888 at 240×284. The committed PNGs are 2× nearest-neighbour enlargements,
created with Pillow as a one-off local tool:

```python
from PIL import Image
Image.open('/tmp/ui-direct.ppm').resize((480, 568), Image.NEAREST).save(
    'docs/src/assets/cardsight-ui.png', optimize=True)
```

Regenerate them when the UI changes.

## Publishing

`docs.yml` builds the site on `master` and deploys it to GitHub Pages:

<https://yuzukihd.github.io/CardSight/>

Pages is enabled for the repository with **Source: GitHub Actions**. The workflow runs on changes
under `docs/`, the application UI, the wasm sources, the public headers, the standard project files
and the workflow itself. `ci.yml` builds the same artifacts on non-master branches and pull requests
without deploying.

## Source release

```sh
./software/scripts/release.sh 0.1.0
# dist/cardsight-0.1.0.tar.gz
```

The tarball contains the licenses, README, standard project files and both font notices. It excludes
build output, `*.a`, `*.o`, `*.d`, the docs build products and the vendored mermaid copy. The
SyterKit submodule is replaced by a `SOURCE.txt` pointer.

## Submodule pointers

Third-party submodules must point at commits that exist in their public repositories. SyterKit must
contain `boards/yuzuki-cardsight/`. Check before moving a pointer:

```sh
git submodule status
git ls-remote https://github.com/YuzukiHD/SyterKit.git
```

## Documentation rules

- English only; one source of truth per topic.
- Quoted `title` and a one-sentence `description` in every page frontmatter.
- Lowercase kebab-case filenames; the slug is the path under `src/content/docs/`.
- Internal links use relative `.md` paths; the remark plugin rewrites them. Do not hard-code
  `/CardSight/...` in content.
- Moved pages get an entry in the `redirects` map in `docs/astro.config.mjs`.
- Destructive or hazardous instructions use a Starlight caution aside.
- Code blocks use `sh`, `text`, `c` or `cmake` fences; interactive sessions show the `cardsight>`
  prompt.
- Do not copy command or configuration tables between pages; link to the canonical reference.
