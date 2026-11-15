#!/usr/bin/env python3
"""Cut the trackpad gesture code and the keyboard's touch code out of SweetHID.cpp for the host test."""
import sys
s = open(sys.argv[1]).read()
def cut(a, b):
    i = s.index(a); j = s.index(b, i)
    return s[i:j]
kbd = cut("#define SWEET_KBD_EDGE", "bool\nSweetHIDTouch::start(")
i = s.index("static struct {\n\tuint32_t downMask;")
pad = s[i:s.index("void\nSweetHIDTouch::mouseTouch(", i)]
open(sys.argv[2], 'w').write(kbd + "\n" + pad)
