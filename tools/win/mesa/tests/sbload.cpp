// sbload.cpp (docs/WIN11-GPU-CAPS.md 5c): does Windows' loader accept a DLL in this process's architecture?
//   sbload.exe DLL EXPORT [EXPORT...]
// LoadLibraryExW(DLL, LOAD_WITH_ALTERED_SEARCH_PATH), GetProcAddress for each export, FreeLibrary. Prints the
// process architecture (native ARM64, x64 under emulation, ARM64EC, x86) so one log line says which view of an
// ARM64X file was loaded. Nothing is called: DllMain runs, the DDI entry points do not.
// Exit code: 0 all found; 1 load failed; 2 an export missing.
#include <windows.h>
#include <stdio.h>

static const char *machine_name(USHORT m)
{
    switch (m) {
    case IMAGE_FILE_MACHINE_ARM64: return "ARM64";
    case IMAGE_FILE_MACHINE_AMD64: return "x64";
    case IMAGE_FILE_MACHINE_I386: return "x86";
    case 0: return "(none)";
    default: return "other";
    }
}

int wmain(int argc, wchar_t **argv)
{
    if (argc < 3) { printf("usage: sbload DLL EXPORT...\n"); return 64; }
#if defined(_M_ARM64EC)
    const char *built = "ARM64EC";
#elif defined(_M_ARM64)
    const char *built = "ARM64";
#elif defined(_M_X64)
    const char *built = "x64";
#else
    const char *built = "x86";
#endif
    USHORT pm = 0, nm = 0;
    typedef BOOL (WINAPI *PFN)(HANDLE, USHORT *, USHORT *);
    PFN f = (PFN)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "IsWow64Process2");
    if (f) f(GetCurrentProcess(), &pm, &nm);
    printf("sbload: built %s, process machine %s on %s, loading %ls\n", built, machine_name(pm), machine_name(nm), argv[1]);
    DWORD t0 = GetTickCount();
    HMODULE h = LoadLibraryExW(argv[1], NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!h) { printf("sbload: FAIL LoadLibrary error %lu\n", GetLastError()); return 1; }
    printf("sbload: loaded at %p in %lu ms\n", (void *)h, GetTickCount() - t0);
    int missing = 0;
    for (int i = 2; i < argc; i++) {
        char name[256];
        WideCharToMultiByte(CP_ACP, 0, argv[i], -1, name, sizeof name, NULL, NULL);
        FARPROC p = GetProcAddress(h, name);
        printf("sbload: %s %s = %p\n", p ? "PASS" : "FAIL", name, (void *)p);
        if (!p) missing++;
    }
    FreeLibrary(h);
    printf("sbload: end, %d missing\n", missing);
    return missing ? 2 : 0;
}
