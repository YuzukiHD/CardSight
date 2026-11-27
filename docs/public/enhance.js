// 每个页面都会加载：画 mermaid 图，并把界面模拟器挂到 [data-cs-sim] 容器上。
// mermaid 由 remark 插件在第一张图前面用阻塞脚本加载；模拟器只在有容器的页面
// 才动态 import，没用到 WebAssembly 的页面不会付这份流量。
const BASE = new URL('./', import.meta.url).href;

async function drawMermaid() {
	const nodes = [...document.querySelectorAll('pre.mermaid')];
	if (!nodes.length) return;
	const mermaid = window.mermaid;
	const dark = document.documentElement.dataset.theme === 'dark';
	mermaid.initialize({ startOnLoad: false, securityLevel: 'strict', theme: dark ? 'dark' : 'default' });
	await mermaid.run({ nodes });
}

async function mountSimulators() {
	const roots = [...document.querySelectorAll('[data-cs-sim]')];
	if (!roots.length) return;
	try {
		const { mount } = await import(BASE + 'demo/cardsight-demo.js');
		for (const root of roots) {
			mount(root).catch((error) => console.error('CardSight simulator', error));
		}
	} catch (error) {
		console.error('CardSight simulator', error);
		for (const root of roots) {
			const status = root.querySelector('[data-cs-status]');
			if (status) status.textContent = 'WebAssembly 载入失败，请查看浏览器控制台。';
		}
	}
}

drawMermaid().catch((error) => console.error('mermaid', error));
mountSimulators().catch((error) => console.error('CardSight simulator', error));
