// Loaded on every page: draws mermaid diagrams and mounts the UI simulator on
// [data-cs-sim] containers. Mermaid is loaded by a blocking script in front of
// the first diagram; the simulator is imported only on pages that use it.
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
			if (status) status.textContent = 'Failed to load the WebAssembly build; see the browser console.';
		}
	}
}

drawMermaid().catch((error) => console.error('mermaid', error));
mountSimulators().catch((error) => console.error('CardSight simulator', error));
