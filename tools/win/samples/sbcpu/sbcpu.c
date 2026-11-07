/*
 * sbcpu: what Windows on the S9 tells a program about its processors (docs/WIN11-24H2-A55.md, "Test program").
 *
 * Built three times by build-sbcpu.sh: ARM64 (native), x64 (runs under xtajit64.dll) and x86 (WoW64, xtajit.dll).
 * Freestanding: no CRT, only kernel32 + advapi32, so an emulated build that reaches main() proves the emulator
 * itself started. Writes one report to stdout and appends it to the file named by the first argument, if any.
 *
 *   sbcpu.exe [LOGFILE]
 *
 * Reports: GetSystemInfo, GetActiveProcessorCount, every GetLogicalProcessorInformationEx record (processor cores
 * with efficiency class, packages, caches with level/ways/line/size/mask, groups), IsProcessorFeaturePresent 0..63,
 * QueryPerformanceFrequency, per-CPU registry ID values (HARDWARE\DESCRIPTION\System\CentralProcessor\N), and on x86/x64
 * the emulated CPUID leaves (0, 1, 4, 7, 0x80000000.., 0x80000005/6) and an RDTSC rate. Then it runs a short
 * busy loop pinned to every processor in turn and reports how long each took (all 8 must answer).
 *
 * Copyright (c) 2026, Starbite. SPDX-License-Identifier: GPL-2.0-only
 */
#include <windows.h>

#if defined(_M_X64) || defined(_M_IX86)
#define SB_X86 1
#endif

static HANDLE gOut, gLog;
static char gBuf[256];
static int gLen;

void *memset (void *d, int c, size_t n) { unsigned char *p = d; while (n--) *p++ = (unsigned char)c; return d; }
void *memcpy (void *d, const void *s, size_t n) { unsigned char *p = d; const unsigned char *q = s; while (n--) *p++ = *q++; return d; }
#ifdef _M_IX86
int _fltused = 0;
#endif

static void Flush (void)
{
    DWORD w;
    if (gLen) {
        WriteFile (gOut, gBuf, gLen, &w, NULL);
        if (gLog != INVALID_HANDLE_VALUE) WriteFile (gLog, gBuf, gLen, &w, NULL);
        gLen = 0;
    }
}
// 64-bit divide without compiler helpers (x86 would need __aulldiv from a CRT)
static unsigned long long Div (unsigned long long n, unsigned long long d, unsigned long long *rem)
{
    unsigned long long q = 0, r = 0;
    int i;
    if (d == 0) { if (rem) *rem = 0; return 0; }
    for (i = 63; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1);
        if (r >= d) { r -= d; q |= 1ull << i; }
    }
    if (rem) *rem = r;
    return q;
}
static void C (char c) { if (gLen == sizeof (gBuf)) Flush (); gBuf[gLen++] = c; if (c == '\n') Flush (); }
static void S (const char *s) { while (*s) C (*s++); }
static void U (unsigned long long v)
{
    char t[24]; int n = 0;
    do { unsigned long long r; v = Div (v, 10, &r); t[n++] = (char)('0' + r); } while (v);
    while (n) C (t[--n]);
}
static void X (unsigned long long v, int digits)
{
    int i;
    S ("0x");
    for (i = digits - 1; i >= 0; i--) C ("0123456789abcdef"[(v >> (4 * i)) & 15]);
}

#ifdef SB_X86
static void Cpuid (unsigned leaf, unsigned sub, unsigned r[4])
{
    unsigned a, b, c, d;
    __asm__ volatile ("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(leaf), "c"(sub));
    r[0] = a; r[1] = b; r[2] = c; r[3] = d;
}
static void Leaf (unsigned leaf, unsigned sub)
{
    unsigned r[4];
    Cpuid (leaf, sub, r);
    S ("cpuid "); X (leaf, 8); S ("."); U (sub);
    S (": eax "); X (r[0], 8); S (" ebx "); X (r[1], 8); S (" ecx "); X (r[2], 8); S (" edx "); X (r[3], 8); S ("\n");
}
#endif

static void Registry (void)
{
    static const char *Names[] = { "CP 4000", "CP 4020", "CP 4021", "CP 4028", "CP 4030", "CP 4031", "CP 4038", "CP 4039", "CP 4001" };
    char key[80] = "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\";
    int base = (int)sizeof ("HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\") - 1, cpu, i;
    for (cpu = 0; cpu < 16; cpu++) {
        HKEY k; char name[96]; DWORD sz, type; unsigned long long v; DWORD mhz;
        int n = base;
        if (cpu >= 10) key[n++] = (char)('0' + cpu / 10);
        key[n++] = (char)('0' + cpu % 10); key[n] = 0;
        if (RegOpenKeyExA (HKEY_LOCAL_MACHINE, key, 0, KEY_READ, &k) != ERROR_SUCCESS) break;
        S ("reg cpu "); U (cpu); S (":");
        sz = sizeof (name);
        if (RegQueryValueExA (k, "ProcessorNameString", NULL, &type, (BYTE *)name, &sz) == ERROR_SUCCESS && type == REG_SZ) {
            name[sizeof (name) - 1] = 0; S (" \""); S (name); S ("\"");
        }
        sz = sizeof (mhz);
        if (RegQueryValueExA (k, "~MHz", NULL, &type, (BYTE *)&mhz, &sz) == ERROR_SUCCESS) { S (" MHz "); U (mhz); }
        for (i = 0; i < (int)(sizeof (Names) / sizeof (Names[0])); i++) {
            sz = sizeof (v); v = 0;
            if (RegQueryValueExA (k, Names[i], NULL, &type, (BYTE *)&v, &sz) == ERROR_SUCCESS) {
                S (" ["); S (Names[i] + 3); S ("]="); X (v, 16);
            }
        }
        S ("\n");
        RegCloseKey (k);
    }
}

static void Topology (void)
{
    static unsigned char buf[16384];
    DWORD len = sizeof (buf), off = 0;
    if (!GetLogicalProcessorInformationEx (RelationAll, (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buf, &len)) {
        S ("GetLogicalProcessorInformationEx failed, error "); U (GetLastError ()); S ("\n");
        return;
    }
    while (off < len) {
        PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX r = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)(buf + off);
        switch (r->Relationship) {
        case RelationProcessorCore:
        case RelationProcessorPackage:
            S (r->Relationship == RelationProcessorCore ? "core: eff " : "package: eff ");
            U (r->Processor.EfficiencyClass); S (" flags "); U (r->Processor.Flags);
            S (" mask "); X (r->Processor.GroupMask[0].Mask, 4); S ("\n");
            break;
        case RelationCache:
            S ("cache: L"); U (r->Cache.Level); S (" type "); U (r->Cache.Type); S (" ways "); U (r->Cache.Associativity);
            S (" line "); U (r->Cache.LineSize); S (" size "); U (r->Cache.CacheSize); S (" mask "); X (r->Cache.GroupMask.Mask, 4);
            S ("\n");
            break;
        case RelationGroup:
            S ("group: active "); U (r->Group.GroupInfo[0].ActiveProcessorCount); S (" max "); U (r->Group.GroupInfo[0].MaximumProcessorCount);
            S (" mask "); X (r->Group.GroupInfo[0].ActiveProcessorMask, 4); S ("\n");
            break;
        case RelationNumaNode:
            S ("numa node "); U (r->NumaNode.NodeNumber); S (" mask "); X (r->NumaNode.GroupMask.Mask, 4); S ("\n");
            break;
        default:
            S ("relation "); U (r->Relationship); S ("\n");
        }
        if (r->Size == 0) break;
        off += r->Size;
    }
}

static volatile unsigned long long gSink;

static void PerCpu (void)
{
    DWORD_PTR proc, sys, bit;
    int cpu;
    LARGE_INTEGER f, a, b;
    QueryPerformanceFrequency (&f);
    if (!GetProcessAffinityMask (GetCurrentProcess (), &proc, &sys)) return;
    for (cpu = 0, bit = 1; cpu < (int)(8 * sizeof (DWORD_PTR)) && bit <= sys && bit; cpu++, bit <<= 1) {
        unsigned long long i, x = 1;
        if (!(sys & bit)) continue;
        if (!SetThreadAffinityMask (GetCurrentThread (), bit)) { S ("cpu "); U (cpu); S (": affinity refused\n"); continue; }
        Sleep (1);
        QueryPerformanceCounter (&a);
        for (i = 0; i < 20000000ull; i++) x = x * 2862933555777941757ull + 3037000493ull;
        QueryPerformanceCounter (&b);
        gSink = x;
        S ("cpu "); U (cpu); S (": now on "); U (GetCurrentProcessorNumber ()); S (", 20M mul-add in ");
        U (Div ((unsigned long long)(b.QuadPart - a.QuadPart) * 1000000ull, (unsigned long long)f.QuadPart, 0));
        S (" us\n");
    }
    SetThreadAffinityMask (GetCurrentThread (), sys);
}

void __cdecl start (void)
{
    SYSTEM_INFO si;
    LARGE_INTEGER f;
    int i;
    char *cmd = GetCommandLineA ();

    gOut = GetStdHandle (STD_OUTPUT_HANDLE);
    gLog = INVALID_HANDLE_VALUE;
    // the first argument after the program name, if any, is the log file
    if (*cmd == '"') { cmd++; while (*cmd && *cmd != '"') cmd++; if (*cmd) cmd++; }
    else { while (*cmd && *cmd != ' ') cmd++; }
    while (*cmd == ' ') cmd++;
    if (*cmd) {
        gLog = CreateFileA (cmd, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    }

#if defined(_M_X64)
    S ("sbcpu x64 (xtajit64)");
#elif defined(_M_IX86)
    S ("sbcpu x86 (WoW64, xtajit)");
#else
    S ("sbcpu arm64");
#endif
    S (" build " SB_BUILD_ID "\n");
    GetNativeSystemInfo (&si);
    S ("native: arch "); U (si.wProcessorArchitecture); S (" cpus "); U (si.dwNumberOfProcessors); S (" active mask "); X (si.dwActiveProcessorMask, 4);
    S (" level "); U (si.wProcessorLevel); S (" rev "); X (si.wProcessorRevision, 4); S (" page "); U (si.dwPageSize); S ("\n");
    GetSystemInfo (&si);
    S ("process view: arch "); U (si.wProcessorArchitecture); S (" cpus "); U (si.dwNumberOfProcessors); S ("\n");
    S ("active processors "); U (GetActiveProcessorCount (ALL_PROCESSOR_GROUPS)); S (", maximum "); U (GetMaximumProcessorCount (ALL_PROCESSOR_GROUPS)); S ("\n");
    QueryPerformanceFrequency (&f);
    S ("QueryPerformanceFrequency "); U ((unsigned long long)f.QuadPart); S ("\n");
    S ("IsProcessorFeaturePresent set:");
    for (i = 0; i < 64; i++) if (IsProcessorFeaturePresent (i)) { S (" "); U (i); }
    S ("\n");
    Topology ();
    Registry ();
#ifdef SB_X86
    {
        unsigned r[4], max, xmax, sub;
        unsigned long long t0, t1;
        LARGE_INTEGER a, b;
        Cpuid (0, 0, r); max = r[0];
        Leaf (0, 0); Leaf (1, 0);
        for (sub = 0; sub < 6 && max >= 4; sub++) { Cpuid (4, sub, r); Leaf (4, sub); if ((r[0] & 31) == 0) break; }
        if (max >= 7) Leaf (7, 0);
        if (max >= 0x15) Leaf (0x15, 0);
        if (max >= 0x16) Leaf (0x16, 0);
        Cpuid (0x80000000u, 0, r); xmax = r[0];
        Leaf (0x80000000u, 0);
        if (xmax >= 0x80000005u) Leaf (0x80000005u, 0);
        if (xmax >= 0x80000006u) Leaf (0x80000006u, 0);
        QueryPerformanceCounter (&a); t0 = __builtin_ia32_rdtsc ();
        Sleep (200);
        QueryPerformanceCounter (&b); t1 = __builtin_ia32_rdtsc ();
        S ("rdtsc: "); U (t1 - t0); S (" ticks in "); U (Div ((unsigned long long)(b.QuadPart - a.QuadPart) * 1000000ull, (unsigned long long)f.QuadPart, 0));
        S (" us\n");
    }
#endif
    PerCpu ();
#if defined(_M_ARM64)
    {
        // Last, after everything else is flushed: direct EL0 reads. CNTFRQ_EL0 is 0 on this phone (S-Boot never sets
        // it; the woa firmware patches the kernel's copy to 26 MHz) - code that divides by the register itself
        // (an emulator's TSC scaling, say) would divide by zero. (CTR_EL0 is not read: Windows keeps SCTLR_EL1.UCT
        // off here, so an EL0 read is an illegal instruction, as the first run on the phone showed.)
        unsigned long long frq, v0, v1;
        __asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(frq));
        S ("CNTFRQ_EL0 (EL0 read) "); U (frq); S ("\n"); Flush ();
        __asm__ volatile ("isb; mrs %0, cntvct_el0" : "=r"(v0));
        Sleep (200);
        __asm__ volatile ("isb; mrs %0, cntvct_el0" : "=r"(v1));
        S ("CNTVCT_EL0 advanced "); U (v1 - v0); S (" in 200 ms\n");
    }
#endif
    S ("sbcpu done\n");
    Flush ();
    if (gLog != INVALID_HANDLE_VALUE) CloseHandle (gLog);
    ExitProcess (0);
}
