# Source me: Windows 11 ARM64 build environment on the Mac (tools/win/README.md).
#   . tools/win/env.sh
# Kit installed by tools/win/setup.sh into ${SBWIN:-$HOME/sbwin} (keep that path stable: the GPU, USB and
# Wi-Fi sessions build against it). Target: Windows 11 22H2/23H2 (22621/22631) ARM64, KMDF 1.33, UMDF 2.33.

export SBWIN=${SBWIN:-$HOME/sbwin}
export SBWIN_KITV=10.0.26100.0
export SBWIN_INC=$SBWIN/kit/Include/$SBWIN_KITV
export SBWIN_LIB=$SBWIN/kit/Lib/$SBWIN_KITV
export SBWIN_WDF_INC=$SBWIN/kit/Include/wdf
export SBWIN_WDF_LIB=$SBWIN/kit/Lib/wdf
export SBWIN_MSVC=$SBWIN/msvc

_llvm=$(brew --prefix llvm 2>/dev/null || echo /opt/homebrew/opt/llvm)
export SB_CLANG_CL=${SB_CLANG_CL:-$_llvm/bin/clang-cl}
export SB_LLD_LINK=${SB_LLD_LINK:-$(command -v lld-link || echo "$_llvm/bin/lld-link")}
export SB_LLVM_RC=${SB_LLVM_RC:-$_llvm/bin/llvm-rc}
export SB_LLVM_LIB=${SB_LLVM_LIB:-$_llvm/bin/llvm-lib}
export SB_READOBJ=${SB_READOBJ:-$_llvm/bin/llvm-readobj}
export SB_OBJDUMP=${SB_OBJDUMP:-$_llvm/bin/llvm-objdump}
unset _llvm

# What the drivers ask for. 22631 = Windows 11 23H2 (same kernel line as 22H2, NTDDI_WIN10_NI).
export SB_NTDDI=${SB_NTDDI:-0x0A00000C}
export SB_KMDF_MINOR=${SB_KMDF_MINOR:-33}
export SB_UMDF_MINOR=${SB_UMDF_MINOR:-33}

[ -f "$SBWIN/PINS" ] || echo "tools/win/env.sh: $SBWIN not set up yet (run tools/win/setup.sh)" >&2
