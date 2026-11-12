// concept-geom.mjs STATEJS SEL...: after the state's JS, the box (pt, phone-relative) of every element matching each
// selector, and for elements with text, each text node's line rect and its font: a check of sbtk's own layout.
import { openConcept } from './cdp.mjs';
const [js, ...sels] = process.argv.slice(2);
const c = await openConcept();
try {
  await c.ev(`(async () => { ${js} })()`);
  const r = await c.ev(`(() => { const ph = document.getElementById('phone').getBoundingClientRect(); const out = [];
    const R = r => [+(r.left - ph.left).toFixed(2), +(r.top - ph.top).toFixed(2), +r.width.toFixed(2), +r.height.toFixed(2)];
    for (const s of ${JSON.stringify(sels)}) document.querySelectorAll(s).forEach((e, i) => {
      const o = { s: s + '#' + i, box: R(e.getBoundingClientRect()) };
      const cs = getComputedStyle(e); o.font = cs.fontWeight + ' ' + cs.fontSize + ' ' + cs.lineHeight + ' ls ' + cs.letterSpacing;
      const t = [...e.childNodes].filter(n => n.nodeType === 3 && n.textContent.trim());
      if (t.length) o.text = t.map(n => { const g = document.createRange(); g.selectNodeContents(n); return [n.textContent, R(g.getBoundingClientRect())]; });
      out.push(o); });
    return out; })()`);
  for (const o of r) console.log(JSON.stringify(o));
} finally { await c.close(); }
process.exit(0);
