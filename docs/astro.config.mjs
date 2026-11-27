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
				'Documentation for the Yuzuki CardSight camera/storage application on Allwinner V861/V881 (XuanTie C907): partition management, formatting, files, and USB storage proxy.',
			logo: {
				src: './src/assets/logo.svg',
				alt: 'CardSight',
			},
			favicon: '/favicon.svg',
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
