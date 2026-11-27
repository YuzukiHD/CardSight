// 把浏览器库从 node_modules 复制到 public/，站点自己提供所有脚本：
//  - mermaid.min.js：由 remark 插件插在第一张 mermaid 图前面
// `astro dev` / `astro build` 之前运行。
import { copyFileSync, mkdirSync } from 'node:fs';

const files = [
	['mermaid/dist/mermaid.min.js', 'mermaid.min.js'],
];

mkdirSync(new URL('../public/', import.meta.url), { recursive: true });
for (const [from, to] of files) {
	copyFileSync(new URL(`../node_modules/${from}`, import.meta.url), new URL(`../public/${to}`, import.meta.url));
}
