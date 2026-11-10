#!/usr/bin/env python3
# sbtk-fonts.py OUTDIR: the toolkit's fonts. stb_truetype reads static TrueType only and does no OpenType layout,
# so this makes, from the repo's variable fonts (tools/brand/fonts, OFL):
#   OUTDIR/<name>.ttf    a static instance (Inter Tight 400..800, Outfit 300), subset to the toolkit's characters
#   OUTDIR/<name>.lay    what the C side needs of the layout HarfBuzz would do (as Chrome does):
#                        "SBTKLAY1", u32 npairs, u32 ntnum, u32 colon_case,
#                        ntnum x (u32 codepoint, u32 glyph), npairs x (u32 g1 << 16 | g2, i32 adjust in font units)
#                        sorted by key. Kerning is measured by shaping every pair with HarfBuzz (GPOS kern lookups,
#                        extension-wrapped included); tnum maps the figures (and : ; , .) to their tabular glyphs; colon_case is
#                        the glyph Inter's calt puts between two figures ("21:05").
# Needs fontTools and uharfbuzz (build/venv).
import os, struct, sys
from fontTools.ttLib import TTFont
from fontTools.varLib import instancer
from fontTools import subset
import uharfbuzz as hb

here = os.path.dirname(os.path.abspath(__file__))
root = os.path.abspath(os.path.join(here, '../../../..'))
src = os.path.join(root, 'tools/brand/fonts')
out = sys.argv[1]
os.makedirs(out, exist_ok=True)

CHARS = [c for c in range(0x20, 0x7f)] + [c for c in range(0xa0, 0x100)] + \
    [0x2013, 0x2014, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2026, 0x2212, 0x2192, 0x2190, 0x00b7, 0x2009, 0x202f]
FONTS = [('it400', 'InterTight[wght].ttf', 400), ('it500', 'InterTight[wght].ttf', 500),
         ('it600', 'InterTight[wght].ttf', 600), ('it700', 'InterTight[wght].ttf', 700),
         ('it800', 'InterTight[wght].ttf', 800), ('of300', 'Outfit-wght.ttf', 300)]

for name, file, w in FONTS:
    vf = TTFont(os.path.join(src, file))
    st = instancer.instantiateVariableFont(vf, {'wght': w}, updateFontNames=False)
    opts = subset.Options()
    opts.layout_features = ['kern', 'tnum', 'calt', 'case', 'ccmp', 'locl', 'liga']
    opts.name_IDs = ['*']
    opts.notdef_outline = True
    opts.glyph_names = True
    opts.hinting = False
    ss = subset.Subsetter(opts)
    ss.populate(unicodes=CHARS)
    ss.subset(st)
    path = os.path.join(out, name + '.ttf')
    st.save(path)

    f = TTFont(path)
    cmap = f.getBestCmap()
    order = f.getGlyphOrder()
    gid = {n: i for i, n in enumerate(order)}
    hmtx = f['hmtx']
    face = hb.Face(hb.Blob.from_file_path(path))
    font = hb.Font(face)

    def shape(s, feat=None):
        b = hb.Buffer()
        b.add_str(s)
        b.guess_segment_properties()
        hb.shape(font, b, feat or {})
        return b.glyph_infos, b.glyph_positions

    # kerning for every glyph pair (alternates included: the calt colon, the tabular figures), read from the
    # GPOS 'kern' lookups (PairPos formats 1 and 2, extension-wrapped too): first matching subtable per lookup
    gpos = f['GPOS'].table
    lk = sorted({li for fr in gpos.FeatureList.FeatureRecord if fr.FeatureTag == 'kern' for li in fr.Feature.LookupListIndex})
    pairs = {}
    names = order
    for li in lk:
        L = gpos.LookupList.Lookup[li]
        subs = [st.ExtSubTable if L.LookupType == 9 else st for st in L.SubTable]
        done = set()
        for st in subs:
            if st.LookupType != 2:
                continue
            cov = st.Coverage.glyphs
            if st.Format == 1:
                for gi, ps in zip(cov, st.PairSet):
                    for pv in ps.PairValueRecord:
                        k = gid[gi] << 16 | gid[pv.SecondGlyph]
                        if k in done or pv.SecondGlyph not in gid:
                            continue
                        done.add(k)
                        v = getattr(pv.Value1, 'XAdvance', 0) if pv.Value1 else 0
                        if v:
                            pairs[k] = pairs.get(k, 0) + v
            else:
                c1 = st.ClassDef1.classDefs if st.ClassDef1 else {}
                c2 = st.ClassDef2.classDefs if st.ClassDef2 else {}
                for g1 in cov:
                    r1 = st.Class1Record[c1.get(g1, 0)]
                    for g2 in names:
                        k = gid[g1] << 16 | gid[g2]
                        if k in done:
                            continue
                        rec = r1.Class2Record[c2.get(g2, 0)]
                        done.add(k)
                        v = getattr(rec.Value1, 'XAdvance', 0) if rec.Value1 else 0
                        if v:
                            pairs[k] = pairs.get(k, 0) + v
    pairs = {k: v for k, v in pairs.items() if v}
    tnum = []
    TN = '0123456789:;,.'
    inf, _ = shape(TN, {'tnum': True, 'calt': False})
    for i, c in enumerate(TN):
        if inf[i].codepoint != gid.get(cmap[ord(c)]):
            tnum.append((ord(c), inf[i].codepoint))
    inf, _ = shape('1:1')
    colon_case = inf[1].codepoint if len(inf) == 3 else gid.get('colon', 0)
    with open(os.path.join(out, name + '.lay'), 'wb') as o:
        o.write(b'SBTKLAY1' + struct.pack('<III', len(pairs), len(tnum), colon_case))
        for cp, g in tnum:
            o.write(struct.pack('<II', cp, g))
        for k in sorted(pairs):
            o.write(struct.pack('<Ii', k, pairs[k]))
    print(f'{name}: {os.path.getsize(path)} bytes, {len(order)} glyphs, {len(pairs)} kern pairs, colon.case {order[colon_case]}')
