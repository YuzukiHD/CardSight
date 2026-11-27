// 构建期 Markdown 插件，照 YuzukiNeko 的 remark-docs.mjs 简化：
//  - ```mermaid 代码块变成 <pre class="mermaid">，由 public/enhance.js 在浏览器里画；
//    页面里第一张图前面加一个阻塞的 <script>，加载站点自己的 mermaid.min.js，不走 CDN
//  - 指向 .md 的相对链接改写成站点 URL（发布在 GitHub Pages 的子路径下）
import path from 'node:path';
import { visit } from 'unist-util-visit';

const escapeHtml = (s) => s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');

export function remarkDocs({ base = '' } = {}) {
	return (tree, file) => {
		const fp = String(file.path ?? '').replaceAll('\\', '/');
		const rel = fp.split('/src/content/docs/')[1] ?? '';
		const dir = path.posix.dirname(rel);

		const pageUrl = (url) => {
			if (/^([a-z][a-z0-9+.-]*:|#|\/\/)/i.test(url)) return url;
			const hashAt = url.indexOf('#');
			const p = hashAt < 0 ? url : url.slice(0, hashAt);
			const hash = hashAt < 0 ? '' : url.slice(hashAt);
			if (!/\.md$/i.test(p)) return url;
			let target = p.startsWith('/') ? p.slice(1) : path.posix.normalize(path.posix.join(dir, p));
			target = target.replace(/\.md$/i, '').replace(/(^|\/)(README|index)$/, '');
			return `${base}/${target}${target ? '/' : ''}${hash}`;
		};

		let mermaidLoaded = false;
		visit(tree, (node, index, parent) => {
			if (node.type === 'code' && node.lang === 'mermaid' && parent && index !== undefined) {
				const loader = mermaidLoaded ? '' : `<script src="${base}/mermaid.min.js"></script>`;
				mermaidLoaded = true;
				parent.children[index] = {
					type: 'html',
					value: `${loader}<pre class="mermaid">${escapeHtml(node.value)}</pre>`,
				};
			} else if (node.type === 'link' && typeof node.url === 'string') {
				node.url = pageUrl(node.url);
			} else if (node.type === 'html' && node.value.includes('assets/')) {
				node.value = node.value.replaceAll(/((?:data-)?src|href)="assets\//g, `$1="${base}/`);
			}
		});
	};
}
