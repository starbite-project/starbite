// hello-exe: the L0 user-mode test program. Prints its build id, the real Windows version (RtlGetVersion,
// which does not lie like GetVersionEx), the CPU architecture and the local time, and appends the same line
// to C:\starbite\hello-exe.txt so it can be read back offline.
#include <windows.h>
#include <stdio.h>

#ifndef SB_BUILD_ID
#define SB_BUILD_ID "unknown"
#endif

typedef LONG(WINAPI *RtlGetVersion_t)(OSVERSIONINFOW *);

int wmain(int argc, wchar_t **argv)
{
    OSVERSIONINFOW v = { sizeof v };
    SYSTEM_INFO si;
    SYSTEMTIME t;
    char line[256];
    RtlGetVersion_t rgv = (RtlGetVersion_t)(void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
    (void)argc; (void)argv;

    if (rgv)
        rgv(&v);
    GetNativeSystemInfo(&si);
    GetLocalTime(&t);
    snprintf(line, sizeof line, "hello-exe build %s on Windows %lu.%lu.%lu %s, %lu CPUs, %04u-%02u-%02u %02u:%02u:%02u\r\n",
             SB_BUILD_ID, v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber,
             si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64 ? "arm64" : "not-arm64",
             si.dwNumberOfProcessors, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    fputs(line, stdout);

    CreateDirectoryW(L"C:\\starbite", NULL);
    HANDLE h = CreateFileW(L"C:\\starbite\\hello-exe.txt", FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD n;
        WriteFile(h, line, (DWORD)strlen(line), &n, NULL);
        CloseHandle(h);
    }
    return 0;
}
