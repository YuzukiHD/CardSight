// Check the built documentation for broken internal links and absolute base paths.
//
// Run after `npm run build`. Doxygen output and Pagefind assets are not crawled.
import { existsSync, readdirSync, readFileSync, statSync } from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const docs = path.resolve(here, '..');
const dist = path.join(docs, 'dist');
const content = path.join(docs, 'src', 'content', 'docs');
const base = '/CardSight';

if (!existsSync(dist)) {
	console.error('docs/dist is missing; run `npm run build` first');
	process.exit(1);
}

function walk(directory) {
	const files = [];
	for (const entry of readdirSync(directory)) {
		const full = path.join(directory, entry);
		if (statSync(full).isDirectory()) files.push(...walk(full));
		else files.push(full);
	}
	return files;
}

const htmlFiles = walk(dist).filter((file) => file.endsWith('.html'));
const pages = new Map();
for (const file of htmlFiles) {
	const rel = path.relative(dist, file).split(path.sep).join('/');
	let url;
	if (rel === 'index.html') url = `${base}/`;
	else if (rel.endsWith('/index.html')) url = `${base}/${rel.slice(0, -'index.html'.length)}`;
	else url = `${base}/${rel}`;
	pages.set(url, file);
}

function fileFor(url) {
	const rel = url.slice(base.length).replace(/^\/+/, '');
	const candidates = [];
	if (rel === '' || url.endsWith('/')) candidates.push(path.join(rel, 'index.html'));
	else {
		candidates.push(rel);
		if (!path.extname(rel)) {
			candidates.push(`${rel}.html`, path.join(rel, 'index.html'));
		}
	}
	for (const candidate of candidates) {
		const full = path.join(dist, candidate);
		if (full.startsWith(dist) && existsSync(full)) return full;
	}
	return null;
}

function resolve(pageUrl, href) {
	if (
		href === '' ||
		href.startsWith('#') ||
		/^(https?:|mailto:|tel:|data:|javascript:|\/\/)/i.test(href)
	) {
		return null;
	}
	const hashAt = href.indexOf('#');
	const hash = hashAt < 0 ? '' : href.slice(hashAt + 1);
	const withoutHash = hashAt < 0 ? href : href.slice(0, hashAt);
	const clean = withoutHash.replaceAll('&amp;', '&').split('?')[0];
	let pathname;
	if (clean.startsWith('/')) {
		pathname = clean;
	} else {
		try {
			pathname = new URL(clean, `https://example.invalid${pageUrl}`).pathname;
		} catch {
			return { error: `invalid URL ${href}` };
		}
	}
	let decoded = pathname;
	try {
		decoded = decodeURIComponent(pathname);
	} catch {
		// Keep the raw path; the lookup will report it if missing.
	}
	return { pathname: decoded, hash };
}

const ids = (file) => {
	const text = readFileSync(file, 'utf8');
	const found = new Set();
	for (const match of text.matchAll(/\bid\s*=\s*["']([^"']+)["']/gi)) found.add(match[1]);
	for (const match of text.matchAll(/\bname\s*=\s*["']([^"']+)["']/gi)) found.add(match[1]);
	return found;
};

const errors = [];
let checked = 0;

for (const [pageUrl, file] of pages) {
	const rel = path.relative(dist, file).split(path.sep).join('/');
	if (rel.startsWith('api/')) continue;
	const text = readFileSync(file, 'utf8');

	for (const match of text.matchAll(/\b(href|src)\s*=\s*["']([^"']+)["']/gi)) {
		const href = match[2];
		const target = resolve(pageUrl, href);
		if (target === null) continue;
		checked++;
		if (target.error) {
			errors.push(`${pageUrl}: ${target.error}`);
			continue;
		}
		if (!target.pathname.startsWith(`${base}/`) && target.pathname !== base) {
			errors.push(`${pageUrl}: link outside the site base: ${href}`);
			continue;
		}
		if (target.pathname.startsWith(`${base}/_astro/`) || target.pathname.startsWith(`${base}/pagefind/`)) {
			continue;
		}
		const targetFile = fileFor(target.pathname);
		if (!targetFile) {
			errors.push(`${pageUrl}: missing target ${target.pathname}`);
			continue;
		}
		if (target.hash && targetFile.endsWith('.html')) {
			const wanted = decodeURIComponent(target.hash);
			if (!ids(targetFile).has(wanted)) {
				errors.push(`${pageUrl}: missing anchor #${wanted} in ${target.pathname}`);
			}
		}
	}

	for (const match of text.matchAll(
		/<meta[^>]+http-equiv=["']refresh["'][^>]+content=["'][^;]+;\s*url=([^"']+)["']/gi,
	)) {
		const target = resolve(pageUrl, match[1].trim());
		if (target && target.pathname) {
			checked++;
			if (!fileFor(target.pathname)) {
				errors.push(`${pageUrl}: redirect target missing ${target.pathname}`);
			}
		}
	}
}

// Content must use relative .md links or import.meta.env.BASE_URL, not hard-coded site URLs.
for (const file of walk(content)) {
	if (!/\.(md|mdx)$/.test(file)) continue;
	const text = readFileSync(file, 'utf8');
	const shown = path.relative(docs, file);
	for (const match of text.matchAll(/\]\(\s*(\/CardSight\/|https:\/\/yuzukihd\.github\.io\/CardSight)/g)) {
		errors.push(`${shown}: hard-coded site URL ${match[1]}`);
	}
}

if (!existsSync(path.join(dist, 'api', 'index.html'))) {
	errors.push('dist/api/index.html is missing');
}
if (!existsSync(path.join(dist, 'api', 'html', 'index.html'))) {
	errors.push('dist/api/html/index.html is missing');
}

if (errors.length) {
	for (const error of errors) console.error(`error: ${error}`);
	console.error(`\n${errors.length} error(s), ${checked} link(s) checked`);
	process.exit(1);
}
console.log(`Checked ${checked} links across ${pages.size} pages; no broken internal links.`);
