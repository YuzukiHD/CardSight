// Copy the browser libraries from node_modules to public/, so the site serves
// all its JavaScript itself:
//  - mermaid.min.js: injected by the remark plugin in front of the first mermaid diagram
// Runs before `astro dev` / `astro build`.
import { copyFileSync, mkdirSync } from 'node:fs';

const files = [
	['mermaid/dist/mermaid.min.js', 'mermaid.min.js'],
];

mkdirSync(new URL('../public/', import.meta.url), { recursive: true });
for (const [from, to] of files) {
	copyFileSync(new URL(`../node_modules/${from}`, import.meta.url), new URL(`../public/${to}`, import.meta.url));
}
