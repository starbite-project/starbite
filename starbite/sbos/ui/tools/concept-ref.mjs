// concept-ref.mjs STATES.json OUTDIR: renders states of the approved shell concept in headless Chrome at the
// panel's 1440 x 2960 (cdp.mjs: repo fonts, fixed clock, no transitions), for the host diff of the sbtk toolkit.
// A state is {"name": "...", "js": "sbSet('unlock', .5); ..."}: its JS runs in a fresh page (window.__hint = 1 shows
// the "Swipe up to open" hint); OUTDIR/<name>.png and <name>.json (its return value and SB_SPEC.geometry()).
import { readFileSync, writeFileSync, mkdirSync } from 'node:fs';
import { join } from 'node:path';
import { openConcept } from './cdp.mjs';

const [statesFile, outDir] = process.argv.slice(2);
const states = JSON.parse(readFileSync(statesFile, 'utf8'));
mkdirSync(outDir, { recursive: true });
const c = await openConcept();
try {
  for (const st of states) {
    await c.reload();
    const val = await c.ev(`(async () => { const r = await (async () => { ${st.js} })();
      document.getElementById('hint').classList.toggle('force', !!window.__hint);
      return { r: r === undefined ? null : r, geom: SB_SPEC.geometry() }; })()`);
    await c.frames(3);
    await c.shot(join(outDir, st.name + '.png'));
    writeFileSync(join(outDir, st.name + '.json'), JSON.stringify(val, null, 1));
    process.stdout.write(st.name + ' ');
  }
  process.stdout.write('\n');
} finally { await c.close(); }
process.exit(0);
