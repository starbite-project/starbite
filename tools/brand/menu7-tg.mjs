#!/usr/bin/env node
/**
 * Menu v7 assets helper: the approved concept's own SVG artwork, extracted from
 * docs/concepts/ios27-boot.html so the assets tool never retypes it.
 *
 *   node tools/brand/menu7-tg.mjs < requests.json > artwork.json
 *
 * requests.json: [[glyph, bg], ...]   the TG(g, bg) calls to make (bg = the tile's c2, or "dark")
 * Output JSON:
 *   defs     the concept's <defs> (tsheen, the mk-* masks) and every <symbol> (g-*, i-*), verbatim
 *   symbols  { id: { viewBox, body } }
 *   tg       [ svg body of TG(g, bg) ] in request order
 *   tiles    the concept's demo entries: [{ provider, id, name, c1, c2, g, dark, svg }]
 *            (svg: the page's own tile(e) markup, for reference renders)
 *   providers [{ id, name, g, tint, tc }]
 *   sq       the squircle path (sqPath(5): 72 segments)
 *   star     STARD
 * The script's functions run in a vm context, as the page defines them.
 */
import { readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import vm from 'node:vm';

const here = dirname(fileURLToPath(import.meta.url));
const html = readFileSync(join(here, '../../docs/concepts/ios27-boot.html'), 'utf8');

const cut = (a, b) => {
  const i = html.indexOf(a), j = html.indexOf(b, i + a.length);
  if (i < 0 || j < 0) throw new Error(`concept changed: marker ${a} / ${b} not found`);
  return html.slice(i, j);
};

// The hidden sprite <svg>: its <defs> and every <symbol>.
const sprite = cut('<svg width="0" height="0"', '</svg>\n');
const defs = sprite.slice(sprite.indexOf('<defs>'), sprite.indexOf('</defs>') + 7);
const symbols = {};
for (const m of sprite.matchAll(/<symbol id="([^"]+)" viewBox="([^"]+)">([\s\S]*?)<\/symbol>/g))
  symbols[m[1]] = { viewBox: m[2], body: m[3] };

// The tile code and the provider data, run as the page runs them.
const code = cut('/* ================= OS icon tiles', 'const PB={}') ;
const ctx = { Math, String, Number, Array, Object, JSON };
vm.createContext(ctx);
vm.runInContext(code + '\n;this.__={TG,SQ,STARD,P,tile};', ctx);
const { TG, SQ, STARD, P, tile } = ctx.__;

let req = [];
try { req = JSON.parse(readFileSync(0, 'utf8') || '[]'); } catch { req = []; }

const out = {
  defs: defs + '\n' + Object.entries(symbols)
    .map(([id, s]) => `<symbol id="${id}" viewBox="${s.viewBox}">${s.body}</symbol>`).join('\n'),
  symbols,
  tg: req.map(([g, bg]) => TG(g, bg)),
  tiles: P.flatMap(p => p.e.map(e => ({
    provider: p.id, id: e.id, name: e.n, c1: e.tl[0], c2: e.tl[1], g: e.tl[2], dark: !!e.tl[3],
    svg: tile(e),
  }))),
  providers: P.map(p => ({ id: p.id, name: p.name, g: p.g, tint: p.tint, tc: p.tc })),
  sq: SQ,
  star: STARD,
};
process.stdout.write(JSON.stringify(out));
