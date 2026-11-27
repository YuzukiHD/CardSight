// Build the WebAssembly UI and copy it next to the docs pages.
//
// The C sources and the LVGL checkout live in ../software. Emscripten must be
// active in the environment (`emsdk activate latest` or the setup-emsdk action
// in CI); emcmake is looked up on PATH, then under $EMSDK.
import { cpSync, existsSync, mkdirSync, rmSync } from 'node:fs';
import { spawnSync } from 'node:child_process';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const site = path.resolve(here, '..');
const root = path.resolve(site, '..');
const source = path.join(root, 'software', 'tests', 'wasm');
const build = path.join(root, 'software', 'build', 'wasm');
const output = path.join(site, 'public', 'ui');

function tool(name) {
	const fromPath = spawnSync('sh', ['-c', `command -v ${name}`], { encoding: 'utf8' });
	if (fromPath.status === 0) return fromPath.stdout.trim();
	const local = process.env.EMSDK ? path.join(process.env.EMSDK, 'upstream', 'emscripten', name) : '';
	return local && existsSync(local) ? local : '';
}

if (process.env.CARDSIGHT_SKIP_WASM === '1') {
	if (existsSync(path.join(output, 'cardsight-ui.js')) && existsSync(path.join(output, 'cardsight-ui.wasm'))) {
		console.log('[cardsight] CARDSIGHT_SKIP_WASM=1, using the existing public/ui build');
		process.exit(0);
	}
	console.error('CARDSIGHT_SKIP_WASM=1 but public/ui has no build; run the wasm build once first');
	process.exit(1);
}

const emcmake = tool('emcmake');
if (!emcmake) {
	console.error(
		'emcmake not found. Install Emscripten and activate it first:\n' +
		'  git clone https://github.com/emscripten-core/emsdk.git ~/emsdk\n' +
		'  ~/emsdk/emsdk install latest && ~/emsdk/emsdk activate latest\n' +
		'  source ~/emsdk/emsdk_env.sh\n' +
		'Then run npm run build again.',
	);
	process.exit(1);
}

if (!existsSync(path.join(root, 'software', 'third_party', 'LVGL', 'lvgl.h'))) {
	console.error('software/third_party/LVGL is not checked out; run: git submodule update --init --recursive');
	process.exit(1);
}

function run(command, args, cwd = root) {
	const result = spawnSync(command, args, { cwd, stdio: 'inherit' });
	if (result.status !== 0) {
		process.exit(result.status ?? 1);
	}
}

console.log('[cardsight] configuring the WebAssembly UI');
run(emcmake, ['cmake', '-S', source, '-B', build, '-DCMAKE_BUILD_TYPE=Release']);
console.log('[cardsight] building the WebAssembly UI');
run('cmake', ['--build', build, '--parallel']);

rmSync(output, { recursive: true, force: true });
mkdirSync(output, { recursive: true });
for (const file of ['cardsight-ui.js', 'cardsight-ui.wasm']) {
	const from = path.join(build, file);
	if (!existsSync(from)) {
		console.error(`build did not produce ${from}`);
		process.exit(1);
	}
	cpSync(from, path.join(output, file));
}
console.log(`[cardsight] WebAssembly UI written to ${path.relative(root, output)}`);
