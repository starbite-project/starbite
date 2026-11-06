#!/usr/bin/env python3
"""patch-extract-resume.py UN1CA_DIR : make UN1CA's scripts/extract_fw.sh resumable after the super
image has been unpacked (a marker .super_unpacked in the firmware dir), so a failure in a later
unpack step does not repeat the hour-long super.img extraction on a slow disk. Idempotent."""
import sys

p = sys.argv[1] + '/scripts/extract_fw.sh'
s = open(p).read()
if '.super_unpacked' in s:
    print('already patched'); sys.exit(0)
META = '[ -f "$FW_DIR/${MODEL}_${CSC}/os_partitions_metadata.txt" ] && rm -f "$FW_DIR/${MODEL}_${CSC}/os_partitions_metadata.txt"'
old1 = ('    ' + META + '\n\n'
        '    if FILE_EXISTS_IN_TAR "$AP_TAR" "super.img" || FILE_EXISTS_IN_TAR "$AP_TAR" "super.img.lz4"; then')
new1 = ('    # Starbite: resume after an interrupted extraction (super image already unpacked: .super_unpacked)\n'
        '    if [ -f "$FW_DIR/${MODEL}_${CSC}/.super_unpacked" ] && [ -f "$FW_DIR/${MODEL}_${CSC}/os_partitions_metadata.txt" ]; then\n'
        '        LOG "- super.img already unpacked (resume)"\n'
        '    else\n' + old1)
SUPER_RM = '        rm -f "$FW_DIR/${MODEL}_${CSC}/super.img"\n    else'
old2 = SUPER_RM
new2 = ('        rm -f "$FW_DIR/${MODEL}_${CSC}/super.img"\n'
        '        touch "$FW_DIR/${MODEL}_${CSC}/.super_unpacked"\n    else')
old3 = '    fi\n\n    local PARTITION\n    for f in $FILES; do'
new3 = '    fi\n    fi\n\n    local PARTITION\n    for f in $FILES; do'
for o in (old1, old2, old3):
    assert s.count(o) == 1, o[:80]
s = s.replace(old1, new1).replace(old2, new2).replace(old3, new3)
open(p, 'w').write(s)
print('patched', p)
