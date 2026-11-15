#!/usr/bin/env python3
"""Cut the present path out of SweetFramebuffer.cpp for the host test (fbtest.cpp includes the result)."""
import sys
s = open(sys.argv[1]).read()
def cut(a, b, incl=True):
    i = s.index(a); j = s.index(b, i)
    return s[i:j + (len(b) if incl else 0)]
out = []
out.append(cut("static uint8_t *sweet_fb_prev;", "#if defined(ARM64_BOARD_CONFIG_A5XELTE)\n#define SWEET_FB_DOT_PITCH", False).rsplit("#endif", 1)[0])
out.append(cut("static uint32_t\nget32(", "static uint64_t sweet_fb_swap_count;"))
out.append("static uint32_t sweet_fb_merged;\nstatic uint32_t sweet_fb_rec_size;\n")
out.append(cut("static bool sweet_fb_blend;", "\treturn copied;\n}\n"))
open(sys.argv[2], 'w').write("\n".join(out))
