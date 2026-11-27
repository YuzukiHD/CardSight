# CardSight documentation site

Published at <https://yuzukihd.github.io/CardSight/>. Built with Astro/Starlight; the landing page
uses a static UI screenshot, and the **Live simulator** page embeds the real LVGL UI compiled to
WebAssembly.

```sh
cd docs
npm ci
npm run dev      # local preview
npm run build    # copy-vendor + wasm + Doxygen + Astro
npm run check    # internal links and generated API
```

The build needs Node.js, Emscripten and Doxygen. The full tool setup, the skip variables
(`CARDSIGHT_SKIP_WASM`, `CARDSIGHT_SKIP_API`), screenshot regeneration, redirects and the publishing
workflow are documented in the site itself:

<https://yuzukihd.github.io/CardSight/development/maintaining/>

All documentation is English. Use relative `.md` links inside `src/content/docs/`; the remark plugin
rewrites them for the GitHub Pages base path.
