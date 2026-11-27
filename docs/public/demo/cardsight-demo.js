// Interactive CardSight UI in the documentation site.
//
// public/ui/cardsight-ui.js is the real LVGL UI compiled to WebAssembly: the
// same 240×284 panel, the five pages, the widgets and the glass material as the
// firmware. This wrapper mounts it on a canvas, forwards pointer input, and
// converts each dirty RGB565 frame to RGBA for the canvas.
import createCardSightUI from '../ui/cardsight-ui.js';

const WIDTH = 240;
const HEIGHT = 284;

// Expand 5/6-bit channels to 8-bit once. Each frame then only needs a lookup
// and a copy.
const colorTable = new Uint32Array(65536);
for (let value = 0; value < 65536; value++) {
	const r5 = (value >>> 11) & 0x1f;
	const g6 = (value >>> 5) & 0x3f;
	const b5 = value & 0x1f;
	const r = (r5 << 3) | (r5 >> 2);
	const g = (g6 << 2) | (g6 >> 4);
	const b = (b5 << 3) | (b5 >> 2);
	colorTable[value] = (r | (g << 8) | (b << 16) | 0xff000000) >>> 0;
}

/**
 * Mount the simulator in a [data-cs-sim] container.
 * @param {HTMLElement} root Container with a canvas, page buttons and scene buttons.
 */
export async function mount(root) {
	if (root.dataset.csMounted) return;
	root.dataset.csMounted = '1';

	const canvas = root.querySelector('canvas');
	const status = root.querySelector('[data-cs-status]');
	const context = canvas.getContext('2d', { alpha: false });
	const image = context.createImageData(WIDTH, HEIGHT);
	const pixels = new Uint32Array(image.data.buffer);

	canvas.width = WIDTH;
	canvas.height = HEIGHT;
	status.textContent = 'Loading the WebAssembly build…';

	let module;
	try {
		module = await createCardSightUI({
			locateFile: (file) => new URL(`../ui/${file}`, import.meta.url).href,
		});
	} catch (error) {
		console.error('CardSight UI failed to load', error);
		status.textContent = 'Failed to load the WebAssembly build; see the browser console.';
		return;
	}

	module._cardsight_web_init();
	root.classList.add('is-ready');

	// Pointer state. A press reaches LVGL immediately; a release is deferred to
	// the next LVGL cycle, otherwise a quick tap between two timer calls is lost.
	let pointerDown = false;
	let pendingUp = false;
	let pointerX = 0;
	let pointerY = 0;

	const point = (event) => {
		const rect = canvas.getBoundingClientRect();
		return {
			x: ((event.clientX - rect.left) * WIDTH) / rect.width,
			y: ((event.clientY - rect.top) * HEIGHT) / rect.height,
		};
	};

	canvas.addEventListener('pointerdown', (event) => {
		event.preventDefault();
		const p = point(event);
		pointerX = p.x;
		pointerY = p.y;
		pointerDown = true;
		canvas.setPointerCapture?.(event.pointerId);
		module._cardsight_web_pointer(Math.round(pointerX), Math.round(pointerY), 1);
	});
	canvas.addEventListener('pointermove', (event) => {
		if (!pointerDown) return;
		const p = point(event);
		pointerX = p.x;
		pointerY = p.y;
		module._cardsight_web_pointer(Math.round(pointerX), Math.round(pointerY), 1);
	});
	const release = () => {
		if (!pointerDown) return;
		pointerDown = false;
		pendingUp = true;
	};
	canvas.addEventListener('pointerup', release);
	canvas.addEventListener('pointercancel', release);
	canvas.addEventListener('pointerleave', release);

	const draw = () => {
		const pointer = module._cardsight_web_framebuffer();
		const source = new Uint16Array(module.HEAPU8.buffer, pointer, WIDTH * HEIGHT);
		for (let index = 0; index < source.length; index++) {
			pixels[index] = colorTable[source[index]];
		}
		context.putImageData(image, 0, 0);
	};

	// Page tabs and demo scenes. The buttons call the exported C entry points;
	// the UI itself is still the firmware UI.
	const pageButtons = [...root.querySelectorAll('[data-cs-page]')];
	const actionButtons = [...root.querySelectorAll('[data-cs-action]')];
	const syncPage = () => {
		const mode = module._cardsight_web_mode();
		for (const button of pageButtons) {
			const active = Number(button.dataset.csPage) === mode;
			button.classList.toggle('is-active', active);
			button.setAttribute('aria-pressed', active ? 'true' : 'false');
		}
	};
	for (const button of pageButtons) {
		button.addEventListener('click', () => {
			module._cardsight_web_page(Number(button.dataset.csPage));
			syncPage();
			status.textContent = `Switched to the "${button.textContent.trim()}" page; you can also tap the tab bar on the screen.`;
		});
	}
	for (const button of actionButtons) {
		button.addEventListener('click', () => {
			module._cardsight_web_action(Number(button.dataset.csAction));
			syncPage();
			status.textContent = button.dataset.csHint || 'Demo state updated.';
		});
	}
	syncPage();

	let previous = performance.now();
	const frame = (now) => {
		const elapsed = Math.min(100, now - previous);
		previous = now;
		module._cardsight_web_tick(elapsed);
		if (pendingUp) {
			module._cardsight_web_pointer(Math.round(pointerX), Math.round(pointerY), 0);
			pendingUp = false;
		}
		if (module._cardsight_web_dirty()) {
			draw();
		}
		requestAnimationFrame(frame);
	};

	module._cardsight_web_tick(16);
	draw();
	requestAnimationFrame(frame);
	status.textContent = 'Ready. This is the real firmware UI; click, drag and scroll.';
}
