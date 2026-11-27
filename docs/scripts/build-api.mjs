// Generate the Doxygen C API reference into public/api/.
//
// Doxygen must be installed locally. CI installs it; for Markdown-only work
// CARDSIGHT_SKIP_API=1 reuses an existing public/api/ build.
import { existsSync, mkdirSync, rmSync, writeFileSync } from 'node:fs';
import { spawnSync } from 'node:child_process';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const docs = path.resolve(here, '..');
const output = path.join(docs, 'public', 'api');
const index = path.join(output, 'html', 'index.html');

function fail(message) {
	console.error(message);
	process.exit(1);
}

if (process.env.CARDSIGHT_SKIP_API === '1') {
	if (existsSync(index)) {
		console.log('[cardsight] CARDSIGHT_SKIP_API=1, using the existing public/api build');
		process.exit(0);
	}
	fail('CARDSIGHT_SKIP_API=1 but public/api has no build; run the full docs build once first');
}

const fromPath = spawnSync('sh', ['-c', 'command -v doxygen'], { encoding: 'utf8' });
const doxygen = process.env.DOXYGEN || (fromPath.status === 0 ? fromPath.stdout.trim() : '');
if (!doxygen) {
	fail(
		'doxygen not found. Install it first (for example `sudo apt install doxygen`), then run\n' +
			'`npm run build` again. For Markdown-only work set CARDSIGHT_SKIP_API=1 and reuse an\n' +
			'existing public/api build.',
	);
}

rmSync(output, { recursive: true, force: true });
mkdirSync(output, { recursive: true });

console.log('[cardsight] generating the Doxygen C API');
const result = spawnSync(doxygen, ['Doxyfile'], { cwd: docs, stdio: 'inherit' });
if (result.status !== 0) {
	fail(`doxygen exited with ${result.status ?? 'an error'}`);
}
if (!existsSync(index)) {
	fail(`doxygen did not produce ${path.relative(docs, index)}`);
}

// Starlight strips .html from internal sidebar links, so expose a stable
// /api/ entry that redirects to the generated index.
writeFileSync(
	path.join(output, 'index.html'),
	'<!doctype html><meta charset="utf-8"><title>CardSight C API</title>' +
		'<meta http-equiv="refresh" content="0; url=html/index.html">' +
		'<link rel="canonical" href="html/index.html">' +
		'<p><a href="html/index.html">CardSight C API reference</a></p>\n',
);
console.log(`[cardsight] C API written to ${path.relative(docs, output)}`);
