#!/usr/bin/env python3
"""Cut StarbiteMali.cpp's buffer cache (0459) out for the host test."""
import sys
s = open(sys.argv[1]).read()
i = s.index("#define SBM_CACHE_SLOTS")
j = s.index("class StarbiteMali : public IOService {", i)
open(sys.argv[2], 'w').write(s[i:j])
