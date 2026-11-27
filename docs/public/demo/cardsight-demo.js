// CardSight 文档站里的界面模拟器。
//
// public/ui/cardsight-ui.js 是 Emscripten 编译出来的真实 LVGL 界面（240×284，
// 和板上的 ST7789 一致）。这里把它挂到页面的 canvas 上，转发触摸/鼠标事件，
// 并把每个新帧的 RGB565 像素转成 canvas 能用的 RGBA。
import createCardSightUI from '../ui/cardsight-ui.js';

const WIDTH = 240;
const HEIGHT = 284;

// 5/6 位色展开到 8 位，预先算好 65536 项的查表，每帧只做一次数组查表和拷贝。
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
 * 把模拟器挂到一个 [data-cs-sim] 容器上。
 * @param {HTMLElement} root 容器，里面要有 canvas、页面按钮和场景按钮。
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
	status.textContent = '正在编译并载入 WebAssembly…';

	let module;
	try {
		module = await createCardSightUI({
			locateFile: (file) => new URL(`../ui/${file}`, import.meta.url).href,
		});
	} catch (error) {
		console.error('CardSight UI failed to load', error);
		status.textContent = 'WebAssembly 载入失败，请查看浏览器控制台。';
		return;
	}

	module._cardsight_web_init();
	root.classList.add('is-ready');

	// 触摸状态。按下立即交给 LVGL，抬起推迟到下一次 LVGL 周期之后，
	// 否则一次很快的点击可能在两次 lv_timer_handler 之间被吞掉。
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

	// 页面标签和演示场景。按钮只调用导出的 C 函数，界面本身仍是真实 UI。
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
			status.textContent = `已切换到「${button.textContent.trim()}」页；也可以直接点屏幕上方的标签栏。`;
		});
	}
	for (const button of actionButtons) {
		button.addEventListener('click', () => {
			module._cardsight_web_action(Number(button.dataset.csAction));
			syncPage();
			status.textContent = button.dataset.csHint || '演示状态已更新。';
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
	status.textContent = '就绪。这是真实界面的 WebAssembly 构建，可直接点击、拖动和滚动。';
}
