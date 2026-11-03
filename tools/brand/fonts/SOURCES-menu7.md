# Boot menu v7 fonts

Downloaded 2026-11-03 over HTTPS from the google/fonts repository (the files fonts.googleapis.com
serves the approved concept, `docs/concepts/ios27-boot.html`). google/fonts `main` was at commit
`2fe1e8c6b137fb13041a7cfca6fdcd8a910ea4dc`; the last commits touching the three directories were
`0b58fb370093f9a9f4ff785d94405710b79de67c` (ofl/intertight, ofl/inter) and
`6e4b84c976cadb3c49a40fd9a1c203e4f7fcf2da` (ofl/jetbrainsmono). All three are under the SIL Open
Font License 1.1 (the `*-OFL.txt` next to them).

| File | URL | sha256 |
|---|---|---|
| `InterTight[wght].ttf` (3.004) | https://github.com/google/fonts/raw/main/ofl/intertight/InterTight%5Bwght%5D.ttf | `b81b73dcb64df3c230cabade7df6c5773bf863233f24c9ee51087519f1f88b6f` |
| `Inter[opsz,wght].ttf` (4.001) | https://github.com/google/fonts/raw/main/ofl/inter/Inter%5Bopsz%2Cwght%5D.ttf | `29160a80ff49ddcab2c97711247e08b1fab27a484a329ce8b813d820dc559031` |
| `JetBrainsMono[wght].ttf` (2.211) | https://github.com/google/fonts/raw/main/ofl/jetbrainsmono/JetBrainsMono%5Bwght%5D.ttf | `48715a42ec242c21e9f02692891e147d022299a52e48d5e413e1a942193ffeda` |
| `InterTight-OFL.txt` | https://github.com/google/fonts/raw/main/ofl/intertight/OFL.txt | `50240ab035cf1b6b3307940235481d515c4b6de3ab1fa843dbe59e7892cb9d58` |
| `Inter-OFL.txt` | https://github.com/google/fonts/raw/main/ofl/inter/OFL.txt | `5b9321a4298cfeb6b34354164a1c3afc3db114569984c502b9b35d988fd58c57` |
| `JetBrainsMono-OFL.txt` | https://github.com/google/fonts/raw/main/ofl/jetbrainsmono/OFL.txt | `b2fe5e8987594e9ffd1d2ca52a2f5d73eb8335243893c5d6254b5ad69269591d` |

## How they are rasterised (tools/brand/menu7-assets.py)

FreeType, grayscale, no hinting, one atlas per (family, weight, size) in `shim/starlte/sb7.h`;
HarfBuzz advances and pair kerning (every pair of the font's charset shaped, kept when no
contextual substitution changed the glyphs).

**What Chrome actually rendered.** The concept's widths (`tw` in
`docs/concepts/boot-geometry/*.json`, measured from the page as approved) match only this setup:

- Inter at **opsz 14** (the axis default), not opsz = min(px, 32): fonts.googleapis.com serves
  `Inter:wght@...` without the opsz axis. With opsz 32 the 42 px row details come out 30 px
  narrower than the concept.
- **No cv11 / ss01**: the served webfonts do not carry those alternates, so the page's
  `font-feature-settings: "cv11", "ss01"` had no effect. The approved screens show Inter's default
  two-storey `a` and default digits; with the alternates every 33-58 px Inter text is 2-10 px off.
- `tnum` on the countdown digit (font 6) does apply (25.94 px vs the concept's 26).
- JetBrains Mono at **500** for every mono text: the page loads only that weight, so its
  `font-weight: 400` texts render at 500 too.

`build/venv/bin/python tools/menu7/sb7-dump.py --check-geometry` reproduces every measured `tw`
to 0.5 px (worst 0.13 px for texts not under a transform).
