#!/usr/bin/env node
/**
 * S-Boot's splash with "Powered by starbite": Samsung's own logo.jpg from UP_PARAM (SAMSUNG /
 * Galaxy S9 / Secured by Knox stay untouched), with the "Powered by android" block (y >= 1490 of
 * the 1079 x 1747 image) redrawn as "Powered by" + the Starbite wordmark. S-Boot draws logo.jpg 1:1
 * at (180, ~1144) of the 1440 x 2960 panel before our loader runs.
 *
 *   node tools/brand/splash-logo.mjs SRC_LOGO.jpg OUT_LOGO.jpg
 * Writes a baseline JPEG of the same size (sips), plus OUT_LOGO.png for a look.
 */
import { execFileSync } from 'node:child_process';
import { readFileSync, writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const project = join(here, '../..');
const [src, out] = process.argv.slice(2);
if (!src || !out) throw new Error('usage: splash-logo.mjs SRC_LOGO.jpg OUT_LOGO.jpg');

const W = 1079, H = 1747, TOP = 1490;
const INK = '#ffffff', ACCENT = '#6fb3ff';
const STAR = 'M100 8 C110 80 120 90 192 100 C120 110 110 120 100 192 C90 120 80 110 8 100 C80 90 90 80 100 8Z';
const measure = (s, size, weight) =>
  parseFloat(execFileSync(join(project, 'build/venv/bin/python'), [join(here, 'measure.py'), s, String(size), String(weight)]).toString());

// Samsung's block: "Powered by" (x 255, cap top ~1525) over "android" (x 255, baseline ~1680).
const x = 255;
const pby = `<text x="${x}" y="1570" font-family="Outfit" font-size="60" font-weight="300" fill="${INK}">Powered by</text>`;
const size = 126, ww = measure('starbite', size, 200), st = size * 0.33;
const word = `<text x="${x - 6}" y="1690" font-family="Outfit" font-size="${size}" font-weight="200" fill="${INK}" letter-spacing="${-size * 0.01}">starbite</text>` +
  `<path transform="translate(${x - 6 + ww + size * 0.06} ${1690 - size * 0.8}) scale(${st / 200})" d="${STAR}" fill="${ACCENT}"/>`;

const svg = `<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" width="${W}" height="${H}">
<image x="0" y="0" width="${W}" height="${H}" xlink:href="data:image/jpeg;base64,${readFileSync(src).toString('base64')}"/>
<rect x="0" y="${TOP}" width="${W}" height="${H - TOP}" fill="#000"/>${pby}${word}</svg>`;
const png = execFileSync('rsvg-convert', ['-f', 'png'], {
  input: svg, maxBuffer: 1 << 28, env: { ...process.env, FONTCONFIG_FILE: join(here, 'fonts/fonts.conf') },
});
writeFileSync(out + '.png', png);
execFileSync('sips', ['-s', 'format', 'jpeg', '-s', 'formatOptions', '92', out + '.png', '--out', out], { stdio: 'ignore' });
console.log(`${out}: ${readFileSync(out).length} bytes`);
