#!/bin/bash
# link3.sh FILELIST OUT: the xnu kernel link (MakeInc.kernel, non-LTO) with dxb-12377's saved arguments; reproduces the seq kernels
set -e
O=/Users/dev/project-starbite/build/dxb-12377/build/xnu.obj/DEVELOPMENT_ARM64_STARLTE
L=$O/kernel.development.starlte.link
export DEVELOPER_DIR=/Applications/Xcode_16.4.app/Contents/Developer
CXX=$DEVELOPER_DIR/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang++
cd $O
# shellcheck disable=SC2046
$CXX -nostdlib $(sed -e "s/-Wl,-rename_segment,__LASTDATA_CONST,__LAST//" -e "s/ -lcc_kext / /" $L/kernel.development.starlte.linkarguments) \
	-Wl,-alias_list,$O/all-alias.exp -Wl,-sectcreate,__LINKINFO,__symbolsets,$O/symbolsets.plist -Wl,-segprot,__LINKINFO,r--,r-- \
	-filelist "$1" $O/lastkerneldataconst.o $O/lastkernelconstructor.o $O/nonlto.o $O/version.o -o "$2" \
	-lcc_kext $(cat $L/kernel.development.starlte.linkarchives)
