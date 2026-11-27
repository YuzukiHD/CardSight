import { defineConfig } from 'astro/config';
import { unified } from '@astrojs/markdown-remark';
import starlight from '@astrojs/starlight';
import { remarkDocs } from './src/plugins/remark-docs.mjs';
import { sidebar } from './sidebar.mjs';

// GitHub Pages project site: https://yuzukihd.github.io/CardSight/
const site = 'https://yuzukihd.github.io';
const base = '/CardSight';

export default defineConfig({
	site,
	base,
	markdown: {
		processor: unified({
			remarkPlugins: [[remarkDocs, { base }]],
		}),
	},
	integrations: [
		starlight({
			title: 'CardSight',
			description:
				'Yuzuki CardSight（Allwinner V861/V881，XuanTie C907）相机/存储卡的屏幕界面与存储代理固件文档',
			logo: {
				src: './src/assets/logo.svg',
				alt: 'CardSight',
			},
			favicon: '/favicon.svg',
			locales: {
				root: { label: '简体中文', lang: 'zh-CN' },
			},
			social: [
				{ icon: 'github', label: 'GitHub', href: 'https://github.com/YuzukiHD/CardSight' },
			],
			editLink: {
				baseUrl: 'https://github.com/YuzukiHD/CardSight/edit/master/docs/',
			},
			sidebar,
			customCss: ['./src/styles/custom.css'],
			head: [
				{ tag: 'script', attrs: { type: 'module', src: `${base}/enhance.js` } },
			],
		}),
	],
});
