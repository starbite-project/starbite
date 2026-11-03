// cdp-shot.mjs URL OUT.png: headless Chrome in real time (animation frames at the display rate, unlike
// --virtual-time-budget), 1000 x 1500, screenshot once document.title is 'M7STOP' (15 s at most).
// Used by compare-all.sh for the intro, which stops itself at #stopat=T.
import { spawn } from 'node:child_process';
import { writeFileSync, mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

const [url, out] = process.argv.slice(2);
const chrome = '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome';
const prof = mkdtempSync(join(tmpdir(), 'm7cdp-'));
const port = 9300 + Math.floor(Math.random() * 600);
const ch = spawn(chrome, ['--headless=new', `--remote-debugging-port=${port}`, `--user-data-dir=${prof}`,
  '--window-size=1000,1500', '--hide-scrollbars', '--no-first-run', 'about:blank'], { stdio: 'ignore' });
const sleep = ms => new Promise(r => setTimeout(r, ms));
let ws, id = 0;
const pend = new Map();
const send = (method, params = {}) => new Promise(res => { const i = ++id; pend.set(i, res); ws.send(JSON.stringify({ id: i, method, params })); });
try {
  let tabs;
  for (let i = 0; i < 100; i++) {
    try { tabs = await (await fetch(`http://127.0.0.1:${port}/json`)).json(); if (tabs.some(t => t.type === 'page')) break; } catch {}
    await sleep(100);
  }
  ws = new WebSocket(tabs.find(t => t.type === 'page').webSocketDebuggerUrl);
  await new Promise(r => ws.addEventListener('open', r));
  ws.addEventListener('message', e => { const m = JSON.parse(e.data); if (m.id && pend.has(m.id)) { pend.get(m.id)(m.result); pend.delete(m.id); } });
  await send('Emulation.setDeviceMetricsOverride', { width: 1000, height: 1500, deviceScaleFactor: 1, mobile: false });
  await send('Page.navigate', { url });
  let ok = false;
  for (let i = 0; i < 300 && !ok; i++) {
    await sleep(50);
    const r = await send('Runtime.evaluate', { expression: 'document.title' });
    ok = r && r.result && r.result.value === 'M7STOP';
  }
  await sleep(150);
  const shot = await send('Page.captureScreenshot', { format: 'png' });
  writeFileSync(out, Buffer.from(shot.data, 'base64'));
  if (!ok) console.error(`cdp-shot: no M7STOP within 15 s: ${url}`);
} finally {
  try { ws && ws.close(); } catch {}
  ch.kill();
  await sleep(300);
  rmSync(prof, { recursive: true, force: true });
}
process.exit(0);
