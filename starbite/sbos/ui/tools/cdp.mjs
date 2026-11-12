// cdp.mjs: headless Chrome over the DevTools protocol for the sbtk tools: the shell concept loaded with the repo's
// fonts (no network), the clock fixed at Tue 10 Nov 2026 21:05, no CSS transitions or animations, sRGB, the phone
// at 390 x 802 pt with the panel's scale (1440 / 390).
import { spawn } from 'node:child_process';
import { writeFileSync, readFileSync, mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
export const root = resolve(here, '../../../..');
export const SCALE = 1440 / 390;
/* Two CSS slips in the concept (index.html sha256 5a87bbb4...), fixed as the concept means them unless
 * SBTK_CONCEPT_ASIS=1: ".dock .app span{display:none}" (meant for the labels) also hides the dock's icons and
 * badges; the app pages' ".row" rule (padding 13 16, 16 px, a border) also lands on the Battery widget's rows. */
const CONCEPT_FIXES = `.dock .app span.ai{display:block!important} .dock .app span.badge{display:grid!important}
.wbig .row{padding:0!important;font-size:12.5px!important;border-bottom:0!important;gap:0!important}
.row > span:last-child{white-space:nowrap;flex:none}`;
const sleep = ms => new Promise(r => setTimeout(r, ms));

export async function openConcept({ transitions = false } = {}) {
  const concept = process.env.SBTK_CONCEPT || join(root, 'docs/concepts/starbite-os-shell/index.html');
  const fonts = join(root, 'tools/brand/fonts');
  const prof = mkdtempSync(join(tmpdir(), 'sbtkcdp-'));
  let html = readFileSync(concept, 'utf8').replace(/<link[^>]*fonts\.g[^>]*>/g, '');
  const face = `<style>
@font-face{font-family:"Inter Tight";src:url("file://${fonts}/InterTight[wght].ttf") format("truetype");font-weight:100 900}
@font-face{font-family:"Outfit";src:url("file://${fonts}/Outfit-wght.ttf") format("truetype");font-weight:100 900}
${transitions ? '' : '*,*::before,*::after{transition:none!important;animation:none!important}'}
#hint:not(.force){opacity:0!important} #hint.force{opacity:1!important}
${process.env.SBTK_CONCEPT_ASIS ? '' : CONCEPT_FIXES}
</style>`;
  html = html.replace('<style>', face + '<style>');
  const page = join(prof, 'concept.html');
  writeFileSync(page, html);
  const fixDate = `(() => { const D = Date, T = new D(2026, 10, 10, 21, 5, 0).getTime();
    class F extends D { constructor(...a) { if (a.length) super(...a); else super(T); } static now() { return T; } }
    window.Date = F; })();`;
  const chrome = '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome';
  const port = 9300 + Math.floor(Math.random() * 600);
  const ch = spawn(chrome, ['--headless=new', `--remote-debugging-port=${port}`, `--user-data-dir=${prof}`,
    '--hide-scrollbars', '--no-first-run', '--force-color-profile=srgb', '--allow-file-access-from-files',
    '--disable-lcd-text', 'about:blank'], { stdio: 'ignore' });
  let ws, id = 0;
  const pend = new Map();
  const send = (method, params = {}) => new Promise((res, rej) => { const i = ++id; pend.set(i, [res, rej]); ws.send(JSON.stringify({ id: i, method, params })); });
  const ev = async expr => { const r = await send('Runtime.evaluate', { expression: expr, awaitPromise: true, returnByValue: true });
    if (r.exceptionDetails) throw new Error(JSON.stringify(r.exceptionDetails).slice(0, 800)); return r.result.value; };
  let tabs;
  for (let i = 0; i < 100; i++) {
    try { tabs = await (await fetch(`http://127.0.0.1:${port}/json`)).json(); if (tabs.some(t => t.type === 'page')) break; } catch {}
    await sleep(100);
  }
  ws = new WebSocket(tabs.find(t => t.type === 'page').webSocketDebuggerUrl);
  await new Promise(r => ws.addEventListener('open', r));
  ws.addEventListener('message', e => { const m = JSON.parse(e.data); if (m.id && pend.has(m.id)) { const [res, rej] = pend.get(m.id); pend.delete(m.id); m.error ? rej(new Error(m.error.message)) : res(m.result); } });
  await send('Page.enable');
  await send('Runtime.enable');
  await send('Emulation.setDeviceMetricsOverride', { width: 390, height: 802, deviceScaleFactor: SCALE, mobile: false });
  await send('Page.addScriptToEvaluateOnNewDocument', { source: fixDate });
  const reload = async () => {
    await send('Page.navigate', { url: 'file://' + page });
    for (let i = 0; i < 200; i++) { await sleep(25); if (await ev('document.readyState === "complete" && !!window.sbSet')) break; }
    await ev('document.fonts.ready.then(() => 1)');
  };
  const frames = n => ev(`new Promise(r => { let k = ${n}; const f = () => --k <= 0 ? r(1) : requestAnimationFrame(f); requestAnimationFrame(f); })`);
  const shot = async (file, clip = { x: 0, y: 0, width: 390, height: 2960 / SCALE }) => {
    const s = await send('Page.captureScreenshot', { format: 'png', clip: { ...clip, scale: 1 } });
    writeFileSync(file, Buffer.from(s.data, 'base64'));
  };
  const close = async () => { try { ws.close(); } catch {} ch.kill(); await sleep(300); rmSync(prof, { recursive: true, force: true }); };
  await reload();
  return { send, ev, reload, frames, shot, close };
}
