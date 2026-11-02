#!/usr/bin/env node
/**
 * S-Boot's Download-mode pictures in the Starbite style (they live in UP_PARAM next to logo.jpg;
 * S-Boot draws them, so their sizes stay Samsung's and the key instructions stay true):
 *   download_L.jpg        1237 x 1310   "Downloading..." (centred; S-Boot fills the rest itself)
 *   download_error_L.jpg  1164 x  953   failed update
 *   warning_L.jpg         1440 x 2960   the custom-OS warning on the way into Download mode
 *   unlock_L.jpg           300 x  300   the "Custom" badge
 *   node tools/brand/download-screens.mjs OUT_DIR      -> OUT_DIR/*.jpg (+ .png previews)
 * Pack with tools/brand/upparam-pack.py.
 */
import { execFileSync } from 'node:child_process';
import { writeFileSync, mkdirSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const project = join(here, '../..');
const out = process.argv[2];
if (!out) throw new Error('usage: download-screens.mjs OUT_DIR');
mkdirSync(out, { recursive: true });

const INK = '#eef1f6', DIM = '#8a93a3', LINE = '#2c313b', ACCENT = '#6fb3ff', AMBER = '#ffb547', CARD = '#0e131c';
const STAR = 'M100 8 C110 80 120 90 192 100 C120 110 110 120 100 192 C90 120 80 110 8 100 C80 90 90 80 100 8Z';
const measure = (s, size, weight) =>
  parseFloat(execFileSync(join(project, 'build/venv/bin/python'), [join(here, 'measure.py'), s, String(size), String(weight)]).toString());
const esc = (s) => s.replace(/&/g, '&amp;').replace(/</g, '&lt;');
const text = (s, { x, y, size, weight = 300, fill = INK, anchor = 'start', spacing = 0 }) =>
  `<text x="${x}" y="${y}" font-family="Outfit" font-size="${size}" font-weight="${weight}" fill="${fill}" text-anchor="${anchor}" letter-spacing="${spacing}">${esc(s)}</text>`;
const star = (cx, cy, size, fill) =>
  `<path transform="translate(${cx - size / 2} ${cy - size / 2}) scale(${size / 200})" d="${STAR}" fill="${fill}"/>`;
function wordmark(x, y, size, anchor = 'start') {
  const w = measure('starbite', size, 200), st = size * 0.33, gap = size * 0.06;
  const x0 = anchor === 'middle' ? x - (w + gap + st) / 2 : x;
  return text('starbite', { x: x0, y, size, weight: 200, spacing: -size * 0.01 }) +
    star(x0 + w + gap + st / 2, y - size * 0.80 + st / 2, st, ACCENT);
}
function jpeg(name, w, h, body) {
  const svg = `<svg xmlns="http://www.w3.org/2000/svg" width="${w}" height="${h}"><rect width="${w}" height="${h}" fill="#000"/>${body}</svg>`;
  const png = execFileSync('rsvg-convert', ['-f', 'png'], {
    input: svg, maxBuffer: 1 << 28, env: { ...process.env, FONTCONFIG_FILE: join(here, 'fonts/fonts.conf') },
  });
  writeFileSync(join(out, name + '.png'), png);
  execFileSync('sips', ['-s', 'format', 'jpeg', '-s', 'formatOptions', '92', join(out, name + '.png'), '--out', join(out, name)], { stdio: 'ignore' });
  console.log(`${join(out, name)}: ${w}x${h}`);
}

// Downloading: a dark card with the arrow and a glowing star, then Samsung's lines.
{
  const W = 1237, H = 1310, cx = W / 2;
  const card = `<defs><radialGradient id="g" cx="0.5" cy="0.5" r="0.5"><stop offset="0" stop-color="${ACCENT}" stop-opacity="0.35"/><stop offset="1" stop-color="${ACCENT}" stop-opacity="0"/></radialGradient></defs>` +
    `<circle cx="${cx}" cy="335" r="330" fill="url(#g)"/>` +
    `<rect x="${cx - 223}" y="112" width="446" height="446" rx="64" fill="${CARD}" stroke="${LINE}" stroke-width="4"/>` +
    `<path d="M${cx} 190 V420 M${cx - 95} 330 L${cx} 425 L${cx + 95} 330" fill="none" stroke="${INK}" stroke-width="26" stroke-linecap="round" stroke-linejoin="round"/>` +
    `<rect x="${cx - 115}" y="472" width="230" height="20" rx="10" fill="${ACCENT}"/>` +
    star(cx + 190, 146, 70, ACCENT);
  jpeg('download_L.jpg', W, H, card +
    text('Downloading…', { x: cx, y: 790, size: 118, weight: 300, anchor: 'middle' }) +
    text('Do not turn off target', { x: cx, y: 890, size: 62, weight: 300, fill: ACCENT, anchor: 'middle' }) +
    text('Volume Down + Power for more than 7 secs', { x: cx, y: 1196, size: 48, weight: 300, fill: DIM, anchor: 'middle' }) +
    text(': Cancel (restart phone)', { x: cx, y: 1272, size: 48, weight: 300, fill: DIM, anchor: 'middle' }));
}

// Update error.
{
  const W = 1164, H = 953, cx = W / 2;
  jpeg('download_error_L.jpg', W, H,
    `<path d="M${cx} 40 L${cx + 250} 450 Q${cx + 262} 472 ${cx + 236} 472 L${cx - 236} 472 Q${cx - 262} 472 ${cx - 250} 450 Z" fill="none" stroke="${AMBER}" stroke-width="18" stroke-linejoin="round"/>` +
    `<rect x="${cx - 14}" y="170" width="28" height="170" rx="14" fill="${INK}"/><circle cx="${cx}" cy="395" r="20" fill="${INK}"/>` +
    text('An error has occurred while', { x: cx, y: 620, size: 62, anchor: 'middle' }) +
    text('updating the device software.', { x: cx, y: 700, size: 62, anchor: 'middle' }) +
    text('Use the Emergency recovery function in the', { x: cx, y: 800, size: 44, fill: DIM, anchor: 'middle' }) +
    text('Smart Switch PC software, or flash with Heimdall/Odin.', { x: cx, y: 862, size: 44, fill: DIM, anchor: 'middle' }));
}

// The custom-OS warning (full screen).
{
  const W = 1440, H = 2960, L = 96;
  const lines = (arr, y0, step, opt) => arr.map((s, i) => text(s, { x: L, y: y0 + i * step, ...opt })).join('');
  jpeg('warning_L.jpg', W, H,
    wordmark(L - 4, 230, 112) +
    text('Download mode', { x: W - L, y: 222, size: 44, fill: DIM, anchor: 'end' }) +
    `<rect x="${L}" y="300" width="${W - 2 * L}" height="3" fill="${LINE}"/>` +
    text('Warning', { x: L, y: 560, size: 168, weight: 200, fill: AMBER }) +
    lines(['A custom OS can cause critical problems', 'in phone and installed applications.'], 720, 84, { size: 62 }) +
    lines(['If you want to download a custom OS,', 'press the volume up key.', 'Otherwise, press the volume down key', 'to cancel.'], 1000, 84, { size: 62 }) +
    `<rect x="${L}" y="1400" width="${W - 2 * L}" height="3" fill="${LINE}"/>` +
    text('VOLUME UP', { x: L, y: 1560, size: 40, weight: 500, fill: DIM, spacing: 6 }) +
    text('Continue', { x: L + 470, y: 1562, size: 72, weight: 300, fill: ACCENT }) +
    text('VOLUME DOWN', { x: L, y: 1700, size: 40, weight: 500, fill: DIM, spacing: 6 }) +
    text('Cancel (restart phone)', { x: L + 470, y: 1702, size: 72, weight: 300 }) +
    `<path d="M${W / 2} 2060 L${W / 2 + 150} 2320 Q${W / 2 + 158} 2336 ${W / 2 + 140} 2336 L${W / 2 - 140} 2336 Q${W / 2 - 158} 2336 ${W / 2 - 150} 2320 Z" fill="none" stroke="${AMBER}" stroke-width="12" stroke-linejoin="round"/>` +
    `<rect x="${W / 2 - 9}" y="2140" width="18" height="110" rx="9" fill="${INK}"/><circle cx="${W / 2}" cy="2290" r="13" fill="${INK}"/>` +
    text('powered by', { x: W / 2, y: 2700, size: 44, fill: DIM, anchor: 'middle', spacing: 1 }) +
    wordmark(W / 2, 2830, 118, 'middle'));
}

// The "Custom" badge.
jpeg('unlock_L.jpg', 300, 300,
  `<path d="M104 118 V78 a48 48 0 0 1 96 0 V90" fill="none" stroke="${INK}" stroke-width="14" stroke-linecap="round" transform="translate(0 -14)"/>` +
  `<rect x="70" y="118" width="160" height="104" rx="22" fill="none" stroke="${INK}" stroke-width="12"/>` +
  star(150, 170, 44, ACCENT) +
  text('Custom', { x: 150, y: 280, size: 50, weight: 300, anchor: 'middle' }));
