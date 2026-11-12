// concept-boxes.mjs STATES.json: for each state, the boxes (pt, phone space) of every element inside the open app
// window, and each text run's baseline (a Range's rects): to place the app screens exactly as Chrome lays them out.
import { readFileSync } from 'node:fs';
import { openConcept } from './cdp.mjs';

const states = JSON.parse(readFileSync(process.argv[2], 'utf8'));
const only = process.argv[3];
const c = await openConcept();
try {
  for (const st of states) {
    if (only && st.name !== only) continue;
    await c.reload();
    const out = await c.ev(`(async () => { ${st.js};
      await new Promise(r => requestAnimationFrame(() => requestAnimationFrame(r)));
      const ph = document.getElementById('phone').getBoundingClientRect(), sc = ph.width / 390;
      const f = v => +(v / sc).toFixed(2);
      const L = [];
      const root = document.querySelector(${JSON.stringify(process.env.ROOT || '#appwin')});
      const walk = (el, d) => {
        const r = el.getBoundingClientRect(), cs = getComputedStyle(el);
        if (r.width) L.push('  '.repeat(d) + el.tagName.toLowerCase() + (el.className && typeof el.className === 'string' ? '.' + el.className.split(' ').join('.') : '') +
          ' ' + f(r.left - ph.left) + ',' + f(r.top - ph.top) + ' ' + f(r.width) + 'x' + f(r.height) + ' fs ' + cs.fontSize + ' fw ' + cs.fontWeight + ' col ' + cs.color + (cs.backgroundColor !== 'rgba(0, 0, 0, 0)' ? ' bg ' + cs.backgroundColor : ''));
        for (const n of el.childNodes) {
          if (n.nodeType === 3 && n.textContent.trim()) {
            const rg = document.createRange(); rg.selectNodeContents(n);
            for (const q of rg.getClientRects()) L.push('  '.repeat(d + 1) + JSON.stringify(n.textContent.trim().slice(0, 40)) + ' ' + f(q.left - ph.left) + ',' + f(q.top - ph.top) + ' ' + f(q.width) + 'x' + f(q.height));
          } else if (n.nodeType === 1) walk(n, d + 1);
        }
      };
      walk(root, 0);
      return L.join('\\n'); })()`);
    console.log('== ' + st.name + '\n' + out);
  }
} finally { await c.close(); }
process.exit(0);
