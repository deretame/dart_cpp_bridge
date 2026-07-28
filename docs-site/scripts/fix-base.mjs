import { readdir, readFile, writeFile } from 'node:fs/promises';
import { join } from 'node:path';

const BASE = '/dart_cpp_bridge';
const DIST_DIR = 'dist';

async function* walk(dir) {
	for (const entry of await readdir(dir, { withFileTypes: true })) {
		const path = join(dir, entry.name);
		if (entry.isDirectory()) {
			yield* walk(path);
		} else if (entry.isFile() && entry.name.endsWith('.html')) {
			yield path;
		}
	}
}

const ATTRS = ['href', 'src'];

function fixBase(html) {
	for (const attr of ATTRS) {
		const regex = new RegExp(`(${attr}="\\/)(?!${BASE.slice(1)}\\/)([^"]*)"`, 'g');
		html = html.replace(regex, `$1${BASE.slice(1)}/$2"`);
	}
	return html;
}

let changedCount = 0;
for await (const file of walk(DIST_DIR)) {
	const html = await readFile(file, 'utf8');
	const fixed = fixBase(html);
	if (fixed !== html) {
		await writeFile(file, fixed, 'utf8');
		changedCount++;
		console.log(`fixed: ${file}`);
	}
}

console.log(`fixed ${changedCount} file(s)`);
