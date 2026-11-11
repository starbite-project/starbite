/* ARM64X only (ARM64EC object). lld-link 20.1.2 /machine:arm64x writes one TLS directory, the native view's:
 * its AddressOfIndex is the ARM64 _tls_index. The loader therefore never writes the ARM64EC _tls_index, and
 * EC code would read index 0 (another module's TLS block). The CRT calls _pRawDllMain first thing in
 * _DllMainCRTStartup, before any CRT or Mesa code can touch a __declspec(thread) variable: copy the index there.
 * A no-op when the directory already names our _tls_index. Not put in .CRT$XI*: in lld 20 ARM64X images the
 * native and EC initializer tables interleave, so native _initterm would call an EC entry. */
#include <windows.h>

extern IMAGE_DOS_HEADER __ImageBase;
extern ULONG _tls_index;

static BOOL WINAPI
sb_raw_dllmain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
   (void)inst; (void)reserved;
   if (reason == DLL_PROCESS_ATTACH) {
      const BYTE *base = (const BYTE *)&__ImageBase;
      const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)(base + __ImageBase.e_lfanew);
      const IMAGE_DATA_DIRECTORY *d = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
      if (d->VirtualAddress != 0 && d->Size >= sizeof(IMAGE_TLS_DIRECTORY64)) {
         const IMAGE_TLS_DIRECTORY64 *t = (const IMAGE_TLS_DIRECTORY64 *)(base + d->VirtualAddress);
         const ULONG *idx = (const ULONG *)(ULONG_PTR)t->AddressOfIndex;
         if (idx != NULL && idx != &_tls_index)
            _tls_index = *idx;
      }
   }
   return TRUE;
}

BOOL (WINAPI *const _pRawDllMain)(HINSTANCE, DWORD, LPVOID) = sb_raw_dllmain;
