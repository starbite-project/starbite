#!/usr/bin/env node
/**
 * Starbite boot menu v5 for the S9 panel (1440 x 2960), drawn by the loader
 * (shim/starlte/menu.c) AROUND Samsung's own boot logo, which S-Boot leaves on
 * screen 1:1 at x 180, y ~1144 (SAMSUNG / Galaxy S9 / Secured by Knox occupy
 * y ~1199..1575; "Powered by android" ~2664..2834 is covered by our footer).
 *
 *   top     header (Starbite mark + name, model) and the boot list:
 *           iOS 26, Windows 11, Android (dimmed: not installed) and Linux (dimmed until the
 *           OS store on UFS holds a kernel)
 *   middle  Samsung's splash, untouched; right under it the Bixby service
 *           selector: Recovery | Download
 *   bottom  the Starbite card: Volume Down past the last OS selects it and
 *           Power boots Linux kiosk; footer "Powered by Starbite"
 *
 * Every element exists as opaque sprites rendered here with rsvg-convert, so
 * the loader only copies pixels. Output build/brand/menu.sbm: "SBMENU01",
 * u32 count, then per sprite u32 id, x, y, w, h, runs, runs x { u32 length,
 * u32 0xAARRGGBB } (row-major). Sprite ids: 0 = base (drawn once),
 * 16 + 8 * item + state; items 0 iOS, 1 Windows, 2 Android, 3 Linux,
 * 4 Recovery chip, 5 Starbite card, 6 Download chip, 7 Engineer hint, 8 power-off hint; states 0 normal,
 * 1 selected, 2 starting, 3 disabled, 4 selected but inactive (the other
 * selector has focus). Also writes build/brand/menu-preview.png.
 *
 * Icons (tools/brand/icons): Apple, Windows 11, Android from simple-icons
 * 9.21.0 (CC0); Tux by Larry Ewing (lewing@isc.tamu.edu) made with The GIMP,
 * via Wikimedia Commons; wrench and download from lucide-static 0.400.0 (ISC).
 *
 *   node tools/brand/menu.mjs      (needs build/brand/overlay.png from overlay.mjs)
 */
import { execFileSync } from 'node:child_process';
import { readFileSync, writeFileSync, mkdirSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { inflateSync } from 'node:zlib';
import { starbiteMark } from './logo.mjs';

const here = dirname(fileURLToPath(import.meta.url));
const project = join(here, '../..');
const out = join(project, 'build/brand');
mkdirSync(out, { recursive: true });

const W = 1440;
// Starred Wordmark (build/brand/wordmark, the brand reference): ink #eef1f6, accent #6fb3ff,
// line #2c313b, Outfit 200/300/500. Sprites stay on pure black (Samsung's splash ground).
const INK = '#eef1f6', DIM = '#8a93a3', FAINT = '#4a5160', LINE = '#2c313b', ACCENT = '#6fb3ff', CORAL = ACCENT,
  GOLD = '#ffd43b', AMBER = '#ffb547';
const SANS = 'Outfit';
const MONO = 'Menlo';
// The brand star (4 points, curved), 200 x 200 box.
const STAR = 'M100 8 C110 80 120 90 192 100 C120 110 110 120 100 192 C90 120 80 110 8 100 C80 90 90 80 100 8Z';
const star = (x, y, size, fill) =>
  `<path transform="translate(${x} ${y}) scale(${size / 200})" d="${STAR}" fill="${fill}"/>`;
/** Advance width of s in Outfit at size/weight (tools/brand/measure.py, the real font). */
const measure = (s, size, weight) =>
  parseFloat(execFileSync(join(project, 'build/venv/bin/python'), [join(here, 'measure.py'), s, String(size), String(weight)]).toString());
/** The wordmark: "starbite" in Outfit 200 with the accent star at its top right; (x, baseline). */
function wordmark(x, y, size, { fill = INK, accent = ACCENT, anchor = 'start' } = {}) {
  const w = measure('starbite', size, 200), st = size * 0.33, gap = size * 0.06;
  const x0 = anchor === 'middle' ? x - (w + gap + st) / 2 : anchor === 'end' ? x - (w + gap + st) : x;
  return text('starbite', { x: x0, y, size, weight: 200, fill, spacing: -size * 0.01 }) +
    star(x0 + w + gap, y - size * 0.80, st, accent);
}

// ---- layout -------------------------------------------------------------
export const LAYOUT = {
  header: { y: 70, h: 150 },
  label: { y: 236, h: 40 },
  rows: { x: 70, w: W - 140, y0: 300, h: 140, stride: 172 },   // 4 OS rows, end ~956
  // Under Samsung's splash ("Secured by Knox" ends ~1575):
  service: { y: 1606, h: 40 },                      // "SERVICE · BIXBY" label
  chips: { y: 1652, h: 120, x0: 70, w: 630, gap: 40 },
  card: { x: 100, y: 1872, w: 1240, h: 560 },     // the Starbite card (from overlay.png); sprite ends ~2524 (over the footer's black top)
  hint: { y: 2866, h: 64, w: 660 },                // two hints side by side under the footer: Engineer (item 7, tap
                                                   // opens it) left, power off (item 8, both volume keys) right
  cardSrcY: 1920,                                   // where overlay.mjs drew it
  footerY: 2500,
};

// ---- PNG ------------------------------------------------------------------
function decodePng(buf) {
  let p = 8, w = 0, h = 0, bpp = 4;
  const idat = [];
  while (p < buf.length) {
    const len = buf.readUInt32BE(p);
    const type = buf.toString('ascii', p + 4, p + 8);
    const data = buf.subarray(p + 8, p + 8 + len);
    if (type === 'IHDR') {
      w = data.readUInt32BE(0); h = data.readUInt32BE(4);
      if (data[8] !== 8 || (data[9] !== 6 && data[9] !== 2) || data[12] !== 0) throw new Error('want 8-bit RGB(A)');
      bpp = data[9] === 6 ? 4 : 3;
    } else if (type === 'IDAT') idat.push(data);
    p += 12 + len;
  }
  const raw = inflateSync(Buffer.concat(idat));
  const stride = w * bpp, src = Buffer.alloc(w * h * bpp);
  for (let y = 0; y < h; y++) {
    const f = raw[y * (stride + 1)];
    const line = raw.subarray(y * (stride + 1) + 1, (y + 1) * (stride + 1));
    for (let x = 0; x < stride; x++) {
      const a = x >= bpp ? src[y * stride + x - bpp] : 0;
      const b = y > 0 ? src[(y - 1) * stride + x] : 0;
      const c = x >= bpp && y > 0 ? src[(y - 1) * stride + x - bpp] : 0;
      let v = line[x];
      if (f === 1) v += a; else if (f === 2) v += b; else if (f === 3) v += (a + b) >> 1;
      else if (f === 4) {
        const pp = a + b - c, pa = Math.abs(pp - a), pb = Math.abs(pp - b), pc = Math.abs(pp - c);
        v += pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
      }
      src[y * stride + x] = v & 255;
    }
  }
  if (bpp === 4) return { w, h, px: src };
  const px = Buffer.alloc(w * h * 4);
  for (let i = 0; i < w * h; i++) { px[i * 4] = src[i * 3]; px[i * 4 + 1] = src[i * 3 + 1]; px[i * 4 + 2] = src[i * 3 + 2]; px[i * 4 + 3] = 255; }
  return { w, h, px };
}

// Outfit comes from tools/brand/fonts through a private fontconfig (nothing installed system-wide).
const svgPng = (svg) => execFileSync('rsvg-convert', ['-f', 'png'], {
  input: svg, maxBuffer: 1 << 28,
  env: { ...process.env, FONTCONFIG_FILE: join(here, 'fonts/fonts.conf') },
});

/** Render an SVG of w x h and flatten it over black: Uint32Array 0xffRRGGBB. */
function render(w, h, body) {
  const svg = `<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" width="${w}" height="${h}" viewBox="0 0 ${w} ${h}"><rect width="${w}" height="${h}" fill="#000"/>${body}</svg>`;
  const { w: pw, h: ph, px } = decodePng(svgPng(svg));
  if (pw !== w || ph !== h) throw new Error(`render size ${pw}x${ph} != ${w}x${h}`);
  const o = new Uint32Array(w * h);
  for (let i = 0; i < w * h; i++) {
    const a = px[i * 4 + 3] / 255;
    const r = Math.round(px[i * 4] * a), g = Math.round(px[i * 4 + 1] * a), b = Math.round(px[i * 4 + 2] * a);
    o[i] = (0xff000000 | (r << 16) | (g << 8) | b) >>> 0;
  }
  return o;
}

const esc = (s) => s.replace(/&/g, '&amp;').replace(/</g, '&lt;');
const text = (s, { x, y, size, weight = 500, fill = INK, family = SANS, anchor = 'start', spacing = 0, opacity = 1 }) =>
  `<text x="${x}" y="${y}" font-family="${family}" font-size="${size}" font-weight="${weight}" fill="${fill}" text-anchor="${anchor}" letter-spacing="${spacing}" opacity="${opacity}">${esc(s)}</text>`;

// ---- icons -----------------------------------------------------------------
const iconFile = (f) => readFileSync(join(here, 'icons', f), 'utf8');
/** Nest an icon SVG at (x, y, size): brand glyphs take `fill`, Lucide line icons take `stroke`. */
function icon(f, x, y, size, { fill, stroke } = {}) {
  const src = iconFile(f).replace(/<\?xml[^>]*>/, '').replace(/<!--[\s\S]*?-->/g, '');
  const open = src.match(/<svg\b[^>]*>/)[0];
  const vb = open.match(/viewBox="([^"]+)"/)[1];
  const inner = src.slice(src.indexOf(open) + open.length, src.lastIndexOf('</svg>')).replace(/<title>[\s\S]*?<\/title>/, '');
  const paint = stroke
    ? `fill="none" stroke="${stroke}" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"`
    : fill ? `fill="${fill}"` : '';
  return `<svg x="${x}" y="${y}" width="${size}" height="${size}" viewBox="${vb}" preserveAspectRatio="xMidYMid meet"><g ${paint}>${inner}</g></svg>`;
}

// Item ids are what the loader acts on (shim/starlte/menu.c v5_os); `row` is where each sits:
// Android, iOS, Linux, Windows. Labels: "<OS version>" | "<flavour> · <kernel or build>".
const ITEMS = [
  { name: 'iOS 26', meta: '26.6.2 · XNU 12377', row: 1, icon: (x, y, s) => icon('si-apple.svg', x, y, s, { fill: '#ffffff' }), optional: true },
  { name: 'Windows 11', meta: '23H2 · NT 22631', row: 3, icon: (x, y, s) => icon('si-windows11.svg', x, y, s, { fill: '#0078d4' }), optional: true },
  // Android: the vendor 4.9 kernel + ramdisk from the UFS OS store; state 3 until installed.
  { name: 'Android 17', meta: 'LineageOS 24.0 · Linux 4.9', row: 0, icon: (x, y, s) => icon('si-android.svg', x, y, s, { fill: '#3ddc84' }), optional: true },
  // Linux: mainline 7.1 from the UFS OS store; state 3 when the store has no kernel.
  { name: 'Linux 7.1', meta: 'postmarketOS · Alpine edge', row: 2, icon: (x, y, s) => icon('tux.svg', x, y - s * 0.06, s * 1.12), optional: true },
];

// row state: 0 normal, 1 selected, 2 starting, 3 disabled, 4 selected but inactive
function rowSvg(it, state) {
  const { w, h } = LAYOUT.rows;
  const sel = state === 1 || state === 2;
  const op = state === 3 ? 0.32 : sel || state === 4 ? 1 : 0.8;
  const pill = sel
    ? `<rect x="2" y="2" width="${w - 4}" height="${h - 4}" rx="36" fill="${ACCENT}" fill-opacity="0.07" stroke="${ACCENT}" stroke-opacity="0.55" stroke-width="3"/>`
    : state === 4
      ? `<rect x="2" y="2" width="${w - 4}" height="${h - 4}" rx="36" fill="none" stroke="${LINE}" stroke-width="3"/>`
      : '';
  const ic = 76, ix = 52, iy = (h - ic) / 2;
  const meta = state === 2 ? 'starting…' : state === 3 ? 'not installed' : it.meta;
  return pill + `<g opacity="${op}">${it.icon(ix, iy, ic)}</g>` +
    text(it.name, { x: ix + ic + 40, y: h / 2 + 21, size: 60, weight: sel ? 400 : 300, fill: sel ? INK : DIM, opacity: state === 3 ? 0.45 : 1 }) +
    text(meta, { x: w - 64, y: h / 2 + 13, size: 36, weight: 300, fill: state === 2 ? ACCENT : sel ? DIM : FAINT, anchor: 'end', opacity: state === 3 ? 0.7 : 1 });
}

// chip state: 0 normal, 1 selected, 2 starting
const CHIPS = [
  { item: 4, name: 'Recovery', meta: 'TWRP 3.7.0', accent: CORAL, icon: (x, y, s, c) => icon('lucide-wrench.svg', x, y, s, { stroke: c }) },
  { item: 6, name: 'Download', meta: 'Odin mode', accent: AMBER, icon: (x, y, s, c) => icon('lucide-download.svg', x, y, s, { stroke: c }) },
];
function chipSvg(c, state) {
  const { w, h } = LAYOUT.chips;
  const on = state !== 0;
  const box = `<rect x="3" y="3" width="${w - 6}" height="${h - 6}" rx="36" fill="${on ? c.accent : '#ffffff'}" fill-opacity="${on ? 0.1 : 0}" stroke="${on ? c.accent : FAINT}" stroke-width="${on ? 4 : 3}"/>`;
  const ic = 60;
  return box + c.icon(44, (h - ic) / 2, ic, on ? c.accent : DIM) +
    text(c.name, { x: 44 + ic + 30, y: 56, size: 46, weight: on ? 400 : 300, fill: on ? INK : DIM }) +
    text(state === 2 ? 'rebooting…' : c.meta, { x: 44 + ic + 30, y: 96, size: 30, weight: 300, fill: on ? (state === 2 ? c.accent : DIM) : FAINT });
}

// ---- RLE sprites -------------------------------------------------------------
const sprites = [];
function sprite(id, x, y, w, h, px) {
  const runs = [];
  for (let i = 0; i < px.length; i++) {
    const p = px[i];
    if (runs.length && runs[runs.length - 1][1] === p) runs[runs.length - 1][0]++;
    else runs.push([1, p]);
  }
  sprites.push({ id, x, y, w, h, runs, px });
}

// Base: header + label.
{
  const { y, h } = LAYOUT.header;
  const body =
    wordmark(74, h / 2 + 34, 104) +
    text('Galaxy S9 · SM-G960F', { x: W - 74, y: h / 2 + 14, size: 36, weight: 300, fill: DIM, anchor: 'end' });
  sprite(0, 0, y, W, h, render(W, h, body));
  const l = LAYOUT.label;
  const lb = text('BOOT', { x: 74, y: 30, size: 28, weight: 500, fill: DIM, spacing: 7 }) +
    `<rect x="190" y="18" width="${W - 260}" height="2" fill="${LINE}"/>`;
  sprite(0, 0, l.y, W, l.h, render(W, l.h, lb));
  const sv = LAYOUT.service;
  const sl = text('SERVICE · BIXBY', { x: 74, y: 30, size: 28, weight: 500, fill: DIM, spacing: 7 }) +
    `<rect x="440" y="18" width="${W - 510}" height="2" fill="${LINE}"/>`;
  sprite(0, 0, sv.y, W, sv.h, render(W, sv.h, sl));
}

// Markers for the intro animation (shim/starlte/anim.c), never drawn: 1-pixel-high sprites whose
// x, y are a star's centre and whose width is its size. Id 2: the header wordmark's star. Id 3: the
// star of "Powered by starbite" on S-Boot's splash (tools/brand/splash-logo.mjs: x 255, size 126,
// baseline 1690 in the 1079 x 1747 logo.jpg, which S-Boot draws at (180, 1144)).
{
  const hs = 104, hw = measure('starbite', hs, 200), hst = hs * 0.33;
  const hx = 74 + hw + hs * 0.06, hy = LAYOUT.header.y + LAYOUT.header.h / 2 + 34 - hs * 0.80;
  sprite(2, Math.round(hx + hst / 2), Math.round(hy + hst / 2), Math.round(hst), 1, new Uint32Array(Math.round(hst)).fill(0xff000000));
  const ss = 126, sw = measure('starbite', ss, 200), sst = ss * 0.33;
  const sx = 180 + 255 - 6 + sw + ss * 0.06, sy = 1144 + 1690 - ss * 0.80;
  sprite(3, Math.round(sx + sst / 2), Math.round(sy + sst / 2), Math.round(sst), 1, new Uint32Array(Math.round(sst)).fill(0xff000000));
}

// OS rows.
const SID = (item, state) => 16 + 8 * item + state;
ITEMS.forEach((it, i) => {
  const r = LAYOUT.rows;
  const y = r.y0 + it.row * r.stride;
  const states = it.disabled ? [3] : it.optional ? [0, 1, 2, 3, 4] : [0, 1, 2, 4];
  for (const st of states) sprite(SID(i, st), r.x, y, r.w, r.h, render(r.w, r.h, rowSvg(it, st)));
});

// Bixby service chips.
CHIPS.forEach((c, k) => {
  const ch = LAYOUT.chips;
  const x = ch.x0 + k * (ch.w + ch.gap);
  for (const st of [0, 1, 2]) sprite(SID(c.item, st), x, ch.y, ch.w, ch.h, render(ch.w, ch.h, chipSvg(c, st)));
});

// Starbite card (item 5) from overlay.png: normal, selected (gold ring + label), starting.
{
  const ov = decodePng(readFileSync(join(out, 'overlay.png')));
  const c = LAYOUT.card;
  const pad = 22, lh = 70;
  const sw = c.w + 2 * pad, sh = c.h + 2 * pad + lh;
  const base = new Uint32Array(sw * sh).fill(0xff000000);
  for (let y = 0; y < c.h; y++) for (let x = 0; x < c.w; x++) {
    const s = ((LAYOUT.cardSrcY + y) * ov.w + c.x + x) * 4;
    const a = ov.px[s + 3] / 255;
    const r = Math.round(ov.px[s] * a), g = Math.round(ov.px[s + 1] * a), b = Math.round(ov.px[s + 2] * a);
    base[(y + pad) * sw + x + pad] = (0xff000000 | (r << 16) | (g << 8) | b) >>> 0;
  }
  const overlayOn = (svgBody) => {
    const top = decodePng(svgPng(`<svg xmlns="http://www.w3.org/2000/svg" width="${sw}" height="${sh}">${svgBody}</svg>`));
    const o = Uint32Array.from(base);
    for (let i = 0; i < sw * sh; i++) {
      const a = top.px[i * 4 + 3] / 255;
      if (!a) continue;
      const d = o[i];
      const mix = (sh8, s) => Math.round(s * a + ((d >> sh8) & 255) * (1 - a));
      o[i] = (0xff000000 | (mix(16, top.px[i * 4]) << 16) | (mix(8, top.px[i * 4 + 1]) << 8) | mix(0, top.px[i * 4 + 2])) >>> 0;
    }
    return o;
  };
  const ring = `<rect x="6" y="6" width="${sw - 12}" height="${c.h + 2 * pad - 12}" rx="56" fill="none" stroke="${GOLD}" stroke-width="7"/>`;
  const label = (s, fill) => text(s, { x: sw / 2, y: c.h + 2 * pad + 50, size: 36, weight: 400, fill, anchor: 'middle', spacing: 2 });
  const x = c.x - pad, y = c.y - pad;
  const dimRing = `<rect x="6" y="6" width="${sw - 12}" height="${c.h + 2 * pad - 12}" rx="56" fill="none" stroke="#ffffff" stroke-opacity="0.18" stroke-width="5"/>`;
  // The Starbite card is off the menu for now (2026-11-02): its sprites are plain black, so the
  // loader (which still draws item 5) leaves the area empty. SHOW_CARD=1 brings it back.
  const showCard = process.env.SHOW_CARD === '1';
  const blank = () => new Uint32Array(sw * sh).fill(0xff000000);
  sprite(SID(5, 0), x, y, sw, sh, showCard ? overlayOn('') : blank());
  sprite(SID(5, 1), x, y, sw, sh, showCard ? overlayOn(ring + label('PRESS POWER TO START THE LINUX KIOSK', GOLD)) : blank());
  sprite(SID(5, 2), x, y, sw, sh, showCard ? overlayOn(ring + label('STARTING THE LINUX KIOSK…', CORAL)) : blank());
  sprite(SID(5, 4), x, y, sw, sh, showCard ? overlayOn(dimRing) : blank());

  // Footer over Samsung's "Powered by android": "powered by" + the wordmark.
  const fy = LAYOUT.footerY, fh = ov.h - fy;
  sprite(0, 0, fy, W, fh, render(W, fh,
    text('powered by', { x: W / 2, y: 200, size: 44, weight: 300, fill: DIM, anchor: 'middle', spacing: 1 }) +
    wordmark(W / 2, 330, 118, { anchor: 'middle' })));
}

// Hints under "Powered by Starbite": Engineer (item 7, tappable) and power off (item 8).
{
  const hn = LAYOUT.hint;
  const hint = (s) => text(s, { x: hn.w / 2, y: 42, size: 28, weight: 300, fill: DIM, anchor: 'middle', spacing: 2 });
  sprite(SID(7, 0), W / 2 - hn.w - 20, hn.y, hn.w, hn.h, render(hn.w, hn.h, hint('HOLD BIXBY  ·  ENGINEER MODE')));
  sprite(SID(8, 0), W / 2 + 20, hn.y, hn.w, hn.h, render(hn.w, hn.h, hint('HOLD VOL+ & VOL−  ·  POWER OFF')));
}

// ---- EngineerMode chrome (ids 200+, opaque on their bar colours: shim/starlte/ui.h) ----
export const EM = { STATUS: '#000000', BAR: '#212121', NAV: '#000000', HOLO: '#33b5e5' };
{
  const app = 120;
  const appIcon =
    `<rect x="4" y="4" width="${app - 8}" height="${app - 8}" rx="22" fill="#2c2c2c"/>` +
    icon('si-android.svg', 16, 18, app - 32, { fill: '#a4c639' }) +
    icon('lucide-wrench.svg', app - 58, app - 58, 50, { stroke: EM.HOLO });
  const onBar = (w, h, body) => render(w, h, `<rect width="${w}" height="${h}" fill="${EM.BAR}"/>` + body);
  sprite(200, 0, 0, app, app, onBar(app, app, appIcon));
  sprite(201, 0, 0, 56, 96, onBar(56, 96, icon('lucide-chevron-left.svg', -4, 20, 60, { stroke: '#ffffff' })));
  const nav = 104;
  sprite(202, 0, 0, nav, nav, render(nav, nav, icon('lucide-undo-2.svg', 14, 14, 76, { stroke: '#e8e8e8' })));
  sprite(203, 0, 0, nav, nav, render(nav, nav, icon('lucide-house.svg', 14, 14, 76, { stroke: '#e8e8e8' })));
  sprite(204, 0, 0, nav, nav, render(nav, nav, icon('lucide-copy.svg', 14, 14, 76, { stroke: '#e8e8e8' })));
}

// ---- write -------------------------------------------------------------------
let size = 12;
for (const s of sprites) size += 24 + 8 * s.runs.length;
const buf = Buffer.alloc(size);
buf.write('SBMENU01', 0, 'ascii');
buf.writeUInt32LE(sprites.length, 8);
let o = 12;
for (const s of sprites) {
  for (const v of [s.id, s.x, s.y, s.w, s.h, s.runs.length]) { buf.writeUInt32LE(v, o); o += 4; }
  for (const [len, p] of s.runs) { buf.writeUInt32LE(len, o); buf.writeUInt32LE(p, o + 4); o += 8; }
}
writeFileSync(join(out, 'menu.sbm'), buf);
console.log(`build/brand/menu.sbm: ${sprites.length} sprites, ${buf.length} bytes`);

// ---- preview: Samsung's logo.jpg at (180, 1144) + base + rows (iOS selected) + card ----
{
  const H = 2960;
  const logo = join(out, 'upparam/logo.jpg');
  const fb = new Uint32Array(W * H).fill(0xff000000);
  const lpng = decodePng(svgPng(`<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" width="${W}" height="${H}"><rect width="${W}" height="${H}" fill="#000"/><image x="180" y="1144" width="1079" height="1747" xlink:href="data:image/jpeg;base64,${readFileSync(logo).toString('base64')}"/></svg>`));
  for (let i = 0; i < W * H; i++) fb[i] = (0xff000000 | (lpng.px[i * 4] << 16) | (lpng.px[i * 4 + 1] << 8) | lpng.px[i * 4 + 2]) >>> 0;
  const sel = process.env.PREVIEW === 'alt'
    ? [SID(2, 4), SID(0, 0), SID(1, 0), SID(3, 0), SID(4, 1), SID(6, 0), SID(5, 0), SID(7, 0), SID(8, 0)]
    : [SID(2, 1), SID(0, 0), SID(1, 0), SID(3, 0), SID(4, 0), SID(6, 0), SID(5, 0), SID(7, 0), SID(8, 0)];
  const want = (id) => id === 0 || sel.includes(id);
  for (const s of sprites) if (want(s.id))
    for (let y = 0; y < s.h; y++) for (let x = 0; x < s.w; x++) fb[(s.y + y) * W + s.x + x] = s.px[y * s.w + x];
  const raw = Buffer.alloc(H * (W * 4 + 1));
  for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) {
    const p = fb[y * W + x], q = y * (W * 4 + 1) + 1 + x * 4;
    raw[q] = (p >> 16) & 255; raw[q + 1] = (p >> 8) & 255; raw[q + 2] = p & 255; raw[q + 3] = 255;
  }
  const { deflateSync } = await import('node:zlib');
  const crc = (b) => { let c = ~0; for (const x of b) { c ^= x; for (let k = 0; k < 8; k++) c = c & 1 ? (c >>> 1) ^ 0xedb88320 : c >>> 1; } return ~c >>> 0; };
  const chunk = (t, d) => { const l = Buffer.alloc(4); l.writeUInt32BE(d.length); const td = Buffer.concat([Buffer.from(t), d]); const c = Buffer.alloc(4); c.writeUInt32BE(crc(td)); return Buffer.concat([l, td, c]); };
  const ih = Buffer.alloc(13); ih.writeUInt32BE(W, 0); ih.writeUInt32BE(H, 4); ih[8] = 8; ih[9] = 6;
  writeFileSync(join(out, process.env.PREVIEW === 'alt' ? 'menu-preview-alt.png' : 'menu-preview.png'), Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk('IHDR', ih), chunk('IDAT', deflateSync(raw)), chunk('IEND', Buffer.alloc(0))]));
  console.log('build/brand/menu-preview.png');
}
