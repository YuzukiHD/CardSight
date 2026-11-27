// Build-time Markdown plugin, simplified from YuzukiNeko's remark-docs.mjs:
//  - ```mermaid blocks become <pre class="mermaid">, drawn in the browser by public/enhance.js;
//    the first diagram is preceded by a blocking <script> for the site's own mermaid.min.js
//  - relative links to .md pages become site URLs (the site lives under the GitHub Pages base)
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
