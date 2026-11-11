// sbtk-icons.mjs OUTDIR: the toolkit's icons, rasterised from the concept's own SVG (sbIcons() and the page's inline
// SVG) by headless Chrome at the panel's scale, on a transparent ground: OUTDIR/<name>.png (straight RGBA; the pack
// premultiplies) and OUTDIR/icons.json (name, size in px, the margin around the box in px). The OS tiles and app
// icons are bitmaps on the phone as on iOS; this bakes them at the sizes the shell draws them.
import { mkdirSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { openConcept, SCALE } from './cdp.mjs';

const out = process.argv[2];
mkdirSync(out, { recursive: true });
const c = await openConcept();
const q = (sel, n = 0) => `document.querySelectorAll(${JSON.stringify(sel)})[${n}].outerHTML`;
const list = [];
const add = (name, js, w, h, style = '', pad = 0) => list.push({ name, js, w, h, style, pad });
const sys = await c.ev('Object.keys(sbIcons().systems)');
const apps = await c.ev('Object.keys(sbIcons().apps)');
for (const k of sys) for (const s of [46, 38, 66, 52]) add(`t${s}-${k}`, `sbIcons().systems[${JSON.stringify(k)}]`, s, s);
for (const k of apps) add(`a62-${k}`, `sbIcons().apps[${JSON.stringify(k)}]`, 62, 62);
add('sb-wifi', q('[data-sbicons] svg', 0), 17, 12);
add('sb-batt', q('[data-sbicons] svg', 1), 27, 13);
add('mark-star', q('#mark svg'), 9, 9);
add('star', q('svg.star'), 36, 36, 'opacity:.9;filter:drop-shadow(0 0 18px rgba(170,200,255,.9))', 30);
add('chev', q('.chev'), 14, 14);
add('alarm', q('.w-rect .k svg'), 11, 11, 'color:rgba(255,255,255,.7)');
add('torch', q('#q-light svg'), 20, 24, 'color:#fff');
add('torch-lit', q('#q-light svg'), 20, 24, 'color:#111');
add('flame', q('#q-em svg'), 20, 24, 'color:#fff');
add('lock-c', q('#lockglyph'), 15, 20);
add('lock-o', `${q('#lockglyph')}.replace('class="shackle"', 'class="shackle" style="transform-origin:11px 9px;transform:translateX(5px) translateY(-2px) rotate(25deg)"')`, 15, 20);
add('recovery', q('#tRecovery svg'), 15, 15);
add('lab', q('#tLab svg'), 15, 15);
add('home-batt', q('#wBattery .lab svg'), 12, 12);
add('home-star', q('#wSystems .lab svg'), 12, 12);
add('search', q('#searchBtn svg'), 13, 13);
try {
  await c.ev(`sbSet('focus', 'win11')`);
  add('arrow', q('#thumb svg'), 22, 22);
  await c.ev(`sbSet('focus', ''); sbSet('unlock', 1); sbSet('cc', 1)`);
  /* the Control Centre's glyphs, in the colours the tiles give them (white; dark #0a0f1f on a lit tile) */
  const cc = sel => q(`#ccg ${sel}`);
  add('cc-air', cc('[data-cc=air] svg'), 20, 20, 'color:#fff');
  add('cc-cell', cc('[data-cc=cell] svg'), 20, 18, 'color:#fff');
  add('cc-wifi', cc('[data-cc=wifi] svg'), 22, 16, 'color:#fff');
  add('cc-bt', cc('[data-cc=bt] svg'), 14, 20, 'color:#fff');
  add('cc-sun', cc('[data-sl=bri] svg'), 22, 22);
  add('cc-vol', cc('[data-sl=vol] svg'), 22, 22);
  for (const [k, w, h] of [['light', 16, 24], ['rot', 22, 22], ['save', 24, 14], ['timer', 22, 22], ['wifi2', 22, 22]]) {
    add('cc-' + k, cc(`[data-cc=${k}] svg`), w, h, 'color:#fff');
    add('cc-' + k + '-on', cc(`[data-cc=${k}] svg`), w, h, 'color:#0a0f1f');
  }
  add('cc-flame', q('#ccEm svg'), 20, 24, 'color:#fff');
  add('a30-calc', `sbIcons().apps.calc`, 30, 30);
  /* the apps' own glyphs (P2l): taken now, while their screen is open (later states replace the app window) */
  /* captured now, its ids made its own (the page's other tiles reuse the same gradient ids after a reload) */
  const now = async (name, sel, w, h, style = '') => {
    const svg = (await c.ev(q(sel))).replace(/id="([^"]+)"/g, `id="$1-${name}"`).replace(/url\(#([^)]+)\)/g, `url(#$1-${name})`)
      .replace(/href="#([^"]+)"/g, `href="#$1-${name}"`);
    add(name, JSON.stringify(svg), w, h, style);
  };
  await c.ev(`sbSet('cc', 0); sbSet('app', ['phone', 1])`);
  for (const k of ['fav', 'rec', 'con', 'pad']) {
    await now('tab-' + k, `.tabs [data-tab=${k}] svg`, 22, 22, 'color:rgba(255,255,255,.6)');
    await now('tab-' + k + '-on', `.tabs [data-tab=${k}] svg`, 22, 22, 'color:#6fb3ff');
  }
  await now('call', '#callBtn svg', 30, 30);
  await c.reload();
  await c.ev(`sbSet('unlock', 1); sbSet('app', ['messages', 1])`);
  await now('search15', '#appwin .sfield svg', 15, 15);
  await c.ev(`document.querySelector('[data-thread="Mum"]').click()`);
  await now('back', '#back svg', 12, 20, 'color:#6fb3ff');
  await now('send', '#send svg', 16, 16);
  /* Emergency mode (P2m): its cards' tiles, the pill's flame, the star's warm glow */
  await c.reload();
  await c.ev(`sbSet('emergency', 1)`);
  for (const [i, k] of ['heat', 'power', 'sos', 'medical'].entries()) await now('e46-' + k, `#nscroll .vgroup:nth-child(${i + 1}) .vhead > span svg`, 46, 46);
  await now('em-flame', '#lock .empill svg', 10, 10);
  add('star-e', q('svg.star'), 36, 36, 'opacity:.9', 30);
  /* the page as the lazy entries above expect it (the focus sheet's thumb, the Control Centre) */
  await c.reload();
  await c.ev(`sbSet('focus', 'win11'); sbSet('focus', ''); sbSet('unlock', 1); sbSet('cc', 1)`);
  add('heart', JSON.stringify('<svg viewBox="0 0 20 20"><text x="0" y="16" font-size="16px">\u2764\ufe0f</text></svg>'), 20, 20);
  add('a30-camera', `sbIcons().apps.camera`, 30, 30);
  await c.send('Emulation.setDefaultBackgroundColorOverride', { color: { r: 0, g: 0, b: 0, a: 0 } });
  await c.ev(`(() => { document.documentElement.style.background = 'transparent'; document.body.style.background = 'transparent';
    document.querySelector('.stage').style.visibility = 'hidden';
    const d = document.createElement('div'); d.id = 'ICON'; d.style.cssText = 'position:fixed;left:0;top:0;visibility:visible;z-index:99999';
    document.body.appendChild(d); })()`);
  const meta = [];
  for (const it of list) {
    const svg = await c.ev(it.js);
    await c.ev(`(() => { const d = document.getElementById('ICON');
      d.innerHTML = '<div style="position:absolute;left:${it.pad}px;top:${it.pad}px;width:${it.w}px;height:${it.h}px;${it.style}">' + ${JSON.stringify(svg)} + '</div>';
      const s = d.querySelector('svg'); s.style.display = 'block'; s.style.position = 'absolute'; s.style.left = '0'; s.style.top = '0';
      s.style.width = '${it.w}px'; s.style.height = '${it.h}px'; s.style.margin = '0'; s.style.opacity = '1'; s.removeAttribute('class');
      ${it.name === 'star' ? "s.parentNode.style.filter = 'drop-shadow(0 0 18px rgba(170,200,255,.9))';" : it.name === 'star-e' ? "s.parentNode.style.filter = 'drop-shadow(0 0 18px rgba(255,170,120,.9))';" : ''} })()`);
    await c.frames(2);
    const W = it.w + 2 * it.pad, H = it.h + 2 * it.pad;
    await c.shot(join(out, it.name + '.png'), { x: 0, y: 0, width: W, height: H });
    meta.push({ name: it.name, w: Math.round(W * SCALE), h: Math.round(H * SCALE), pad: it.pad * SCALE });
  }
  writeFileSync(join(out, 'icons.json'), JSON.stringify(meta, null, 1));
  console.log(`${meta.length} icons`);
} finally { await c.close(); }
process.exit(0);
