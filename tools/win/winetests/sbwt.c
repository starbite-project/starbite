// SPDX-License-Identifier: GPL-2.0-only
//
// sbwt: run a Wine test suite built by tools/win/winetests/build.sh one test function per process.
//
//   sbwt.exe SUITE_test.exe SUITE LOG [TIMEOUT_S [FIRST [COUNT]]]
//
// 1. SUITE_test.exe SUITE with SBWT_LIST=1 prints "SBWT-TEST name" per test function.
// 2. Each one runs as its own process (SBWT_ONLY=name, WINETEST_NO_MT_D3D=1), output to LOG.out\name.txt, with a
//    timeout (default 120 s; the process is terminated). No error dialogs (the error mode is inherited).
// 3. One line per test in LOG:
//      SBWT SUITE name: PASS|FAIL|CRASH|TIMEOUT|NOSTART; exit 0x..; executed N, failures F, todo T, skipped S; X.X s
//    then a summary line. The first 12 "Test failed" lines of each failing test follow its line.
// FIRST/COUNT (0-based) run a slice, for splitting a long suite over boots.
//
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXT 2048
static char names[MAXT][96];
static unsigned ntests;
static FILE *lg;

static void say(const char *fmt, ...)
{
    va_list a;
    va_start(a, fmt);
    vprintf(fmt, a);
    va_end(a);
    fflush(stdout);
    if (lg) {
        va_start(a, fmt);
        vfprintf(lg, fmt, a);
        va_end(a);
        fflush(lg);
    }
}

// run "exe suite" with the environment set; stdout+stderr to path; returns 0 ran, 1 timeout, 2 no start
static int run(const char *exe, const char *suite, const char *path, DWORD timeout_ms, DWORD *code, double *secs)
{
    SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};
    HANDLE out = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (out == INVALID_HANDLE_VALUE)
        return 2;
    STARTUPINFOA si = {0};
    PROCESS_INFORMATION pi = {0};
    char cmd[600];
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = si.hStdError = out;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    snprintf(cmd, sizeof(cmd), "\"%s\" %s", exe, suite);
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    if (!CreateProcessA(NULL, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        CloseHandle(out);
        return 2;
    }
    int ret = 0;
    if (WaitForSingleObject(pi.hProcess, timeout_ms) != WAIT_OBJECT_0) {
        TerminateProcess(pi.hProcess, 0xdead);
        WaitForSingleObject(pi.hProcess, 5000);
        ret = 1;
    }
    QueryPerformanceCounter(&t1);
    *secs = (double)(t1.QuadPart - t0.QuadPart) / (double)f.QuadPart;
    *code = 0;
    GetExitCodeProcess(pi.hProcess, code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(out);
    return ret;
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        printf("usage: sbwt SUITE_test.exe SUITE LOG [TIMEOUT_S [FIRST [COUNT]]]\n");
        return 2;
    }
    const char *exe = argv[1], *suite = argv[2];
    DWORD timeout = (argc > 4 ? (DWORD)atoi(argv[4]) : 120) * 1000;
    unsigned first = argc > 5 ? (unsigned)atoi(argv[5]) : 0, count = argc > 6 ? (unsigned)atoi(argv[6]) : MAXT;
    char dir[400], path[520], line[1024];
    lg = fopen(argv[3], "a");
    snprintf(dir, sizeof(dir), "%s.out", argv[3]);
    CreateDirectoryA(dir, NULL);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    SetEnvironmentVariableA("WINETEST_NO_MT_D3D", "1");

    DWORD code;
    double secs;
    snprintf(path, sizeof(path), "%s\\_list.txt", dir);
    SetEnvironmentVariableA("SBWT_LIST", "1");
    int r = run(exe, suite, path, 60000, &code, &secs);
    SetEnvironmentVariableA("SBWT_LIST", NULL);
    FILE *f = fopen(path, "r");
    while (f && fgets(line, sizeof(line), f)) {
        if (!strncmp(line, "SBWT-TEST ", 10) && ntests < MAXT) {
            line[strcspn(line, "\r\n")] = 0;
            strncpy(names[ntests], line + 10, sizeof(names[0]) - 1);
            ntests++;
        }
    }
    if (f)
        fclose(f);
    say("SBWT %s: %u test functions listed (list run %d, exit 0x%lx, %.1f s); timeout %lu s; slice %u+%u\n", suite,
        ntests, r, code, secs, timeout / 1000, first, count);
    if (!ntests)
        return 1;

    unsigned pass = 0, fail = 0, crash = 0, tmo = 0, nostart = 0;
    for (unsigned i = first; i < ntests && i - first < count; i++) {
        snprintf(path, sizeof(path), "%s\\%s.txt", dir, names[i]);
        SetEnvironmentVariableA("SBWT_ONLY", names[i]);
        r = run(exe, suite, path, timeout, &code, &secs);
        unsigned executed = 0, failures = 0, todo = 0, skipped = 0, have = 0;
        char fails[12][300];
        unsigned nf = 0;
        f = fopen(path, "r");
        while (f && fgets(line, sizeof(line), f)) {
            char *p = strstr(line, " tests executed (");
            if (p) {
                // "<pid>:suite: N tests executed (T marked as todo, F failures), S skipped."
                char *q = p;
                while (q > line && q[-1] >= '0' && q[-1] <= '9')
                    q--;
                unsigned flaky = 0;
                // "failures" or "failure" (one); older Wine has no "as flaky" part
                if (sscanf(q, "%u tests executed (%u marked as todo, %u as flaky, %u %*[a-z]), %u skipped", &executed,
                           &todo, &flaky, &failures, &skipped) == 5 ||
                    sscanf(q, "%u tests executed (%u marked as todo, %u %*[a-z]), %u skipped", &executed, &todo,
                           &failures, &skipped) == 4)
                    have = 1;
            }
            if (nf < 12 && (strstr(line, "Test failed") || strstr(line, "unhandled exception") ||
                            strstr(line, "Test succeeded inside todo"))) {
                line[strcspn(line, "\r\n")] = 0;
                strncpy(fails[nf], line, sizeof(fails[0]) - 1);
                fails[nf][sizeof(fails[0]) - 1] = 0;
                nf++;
            }
        }
        if (f)
            fclose(f);
        const char *res;
        if (r == 2) {
            res = "NOSTART";
            nostart++;
        } else if (r == 1) {
            res = "TIMEOUT";
            tmo++;
        } else if (!have || code >= 0x80000000u) {
            res = "CRASH";
            crash++;
        } else if (failures || code) {
            res = "FAIL";
            fail++;
        } else {
            res = "PASS";
            pass++;
        }
        say("SBWT %s %s: %s; exit 0x%lx; executed %u, failures %u, todo %u, skipped %u; %.1f s\n", suite, names[i], res,
            code, executed, failures, todo, skipped, secs);
        for (unsigned k = 0; k < nf; k++)
            say("    %s\n", fails[k]);
    }
    say("SBWT %s SUMMARY: %u pass, %u fail, %u crash, %u timeout, %u nostart of %u run (%u listed)\n", suite, pass, fail,
        crash, tmo, nostart, pass + fail + crash + tmo + nostart, ntests);
    return 0;
}
