/* SPDX-License-Identifier: GPL-2.0-only
 * sblinkd portability layer (plat.h): Win32 and POSIX.
 */
#include "plat.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static plat_mutex log_lock;
static int log_init_done;
static int log_stderr = 1;
static char log_path[512];

#ifdef _WIN32
/* ------------------------------------------------------------------------------------------------ */

void plat_mutex_init(plat_mutex *m) { InitializeCriticalSection(m); }
void plat_mutex_lock(plat_mutex *m) { EnterCriticalSection(m); }
void plat_mutex_unlock(plat_mutex *m) { LeaveCriticalSection(m); }

plat_event plat_event_new(void) { return CreateEventW(NULL, FALSE, FALSE, NULL); }
void plat_event_set(plat_event e) { SetEvent(e); }
int plat_event_wait(plat_event e, int ms)
{
    return WaitForSingleObject(e, ms < 0 ? INFINITE : (DWORD)ms) == WAIT_OBJECT_0;
}

struct thr { void (*fn)(void *); void *arg; };
static DWORD WINAPI thr_main(LPVOID p)
{
    struct thr t = *(struct thr *)p;
    free(p);
    t.fn(t.arg);
    return 0;
}
int plat_thread(void (*fn)(void *), void *arg)
{
    struct thr *t = malloc(sizeof *t);
    if (!t) return -1;
    t->fn = fn; t->arg = arg;
    HANDLE h = CreateThread(NULL, 0, thr_main, t, 0, NULL);
    if (!h) { free(t); return -1; }
    CloseHandle(h);
    return 0;
}

uint64_t plat_now_us(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (uint64_t)(c.QuadPart / f.QuadPart) * 1000000u +
           (uint64_t)(c.QuadPart % f.QuadPart) * 1000000u / (uint64_t)f.QuadPart;
}
void plat_sleep_ms(int ms) { Sleep((DWORD)ms); }

static HANDLE log_h = INVALID_HANDLE_VALUE;

static HANDLE open_log_w(const wchar_t *path)
{
    /* keep one previous log: rotate at 8 MB */
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (GetFileAttributesExW(path, GetFileExInfoStandard, &fa) && (fa.nFileSizeHigh || fa.nFileSizeLow > (8u << 20))) {
        wchar_t old[600];
        _snwprintf(old, 599, L"%ls.old", path);
        old[599] = 0;
        MoveFileExW(path, old, MOVEFILE_REPLACE_EXISTING);
    }
    return CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                       OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
}

int plat_log_open(const char *path, int to_stderr)
{
    if (!log_init_done) { plat_mutex_init(&log_lock); log_init_done = 1; }
    log_stderr = to_stderr;
    wchar_t wp[600];
    if (path) {
        MultiByteToWideChar(CP_UTF8, 0, path, -1, wp, 600);
        log_h = open_log_w(wp);
    } else {
        CreateDirectoryW(L"C:\\starbite", NULL);
        CreateDirectoryW(L"C:\\starbite\\link", NULL);
        wcscpy(wp, L"C:\\starbite\\link\\sblinkd.log");
        log_h = open_log_w(wp);
        if (log_h == INVALID_HANDLE_VALUE) {
            wchar_t la[MAX_PATH];
            DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", la, MAX_PATH);
            if (n && n < MAX_PATH - 40) {
                _snwprintf(wp, 599, L"%ls\\Starbite", la); wp[599] = 0;
                CreateDirectoryW(wp, NULL);
                _snwprintf(wp, 599, L"%ls\\Starbite\\Link", la); wp[599] = 0;
                CreateDirectoryW(wp, NULL);
                _snwprintf(wp, 599, L"%ls\\Starbite\\Link\\sblinkd.log", la); wp[599] = 0;
                log_h = open_log_w(wp);
            }
        }
    }
    if (log_h == INVALID_HANDLE_VALUE) { log_path[0] = 0; return -1; }
    WideCharToMultiByte(CP_UTF8, 0, wp, -1, log_path, sizeof log_path, NULL, NULL);
    return 0;
}

static void log_write(const char *s, size_t n)
{
    DWORD w;
    if (log_h != INVALID_HANDLE_VALUE) WriteFile(log_h, s, (DWORD)n, &w, NULL);
    if (log_stderr) { fwrite(s, 1, n, stderr); fflush(stderr); }
}

static size_t log_stamp(char *b, size_t cap)
{
    SYSTEMTIME t;
    GetLocalTime(&t);
    return (size_t)snprintf(b, cap, "%04u-%02u-%02u %02u:%02u:%02u.%03u ", t.wYear, t.wMonth, t.wDay, t.wHour,
                            t.wMinute, t.wSecond, t.wMilliseconds);
}

#else
/* ------------------------------------------------------------------------------------------------ */
#include <sys/time.h>
#include <unistd.h>

struct plat_event_s { pthread_mutex_t m; pthread_cond_t c; int set; };

void plat_mutex_init(plat_mutex *m) { pthread_mutex_init(m, NULL); }
void plat_mutex_lock(plat_mutex *m) { pthread_mutex_lock(m); }
void plat_mutex_unlock(plat_mutex *m) { pthread_mutex_unlock(m); }

plat_event plat_event_new(void)
{
    plat_event e = calloc(1, sizeof *e);
    if (!e) return NULL;
    pthread_mutex_init(&e->m, NULL);
    pthread_cond_init(&e->c, NULL);
    return e;
}
void plat_event_set(plat_event e)
{
    pthread_mutex_lock(&e->m);
    e->set = 1;
    pthread_cond_signal(&e->c);
    pthread_mutex_unlock(&e->m);
}
int plat_event_wait(plat_event e, int ms)
{
    pthread_mutex_lock(&e->m);
    if (!e->set && ms != 0) {
        if (ms < 0) {
            while (!e->set) pthread_cond_wait(&e->c, &e->m);
        } else {
            struct timeval tv;
            gettimeofday(&tv, NULL);
            struct timespec ts;
            uint64_t ns = (uint64_t)tv.tv_usec * 1000u + (uint64_t)ms * 1000000u;
            ts.tv_sec = tv.tv_sec + (time_t)(ns / 1000000000u);
            ts.tv_nsec = (long)(ns % 1000000000u);
            while (!e->set) if (pthread_cond_timedwait(&e->c, &e->m, &ts)) break;
        }
    }
    int r = e->set;
    e->set = 0;
    pthread_mutex_unlock(&e->m);
    return r;
}

struct thr { void (*fn)(void *); void *arg; };
static void *thr_main(void *p)
{
    struct thr t = *(struct thr *)p;
    free(p);
    t.fn(t.arg);
    return NULL;
}
int plat_thread(void (*fn)(void *), void *arg)
{
    struct thr *t = malloc(sizeof *t);
    if (!t) return -1;
    t->fn = fn; t->arg = arg;
    pthread_t th;
    if (pthread_create(&th, NULL, thr_main, t)) { free(t); return -1; }
    pthread_detach(th);
    return 0;
}

uint64_t plat_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}
void plat_sleep_ms(int ms) { usleep((useconds_t)ms * 1000u); }

static FILE *log_f;
int plat_log_open(const char *path, int to_stderr)
{
    if (!log_init_done) { plat_mutex_init(&log_lock); log_init_done = 1; }
    log_stderr = to_stderr;
    if (!path) { log_path[0] = 0; return -1; }
    log_f = fopen(path, "a");
    if (!log_f) return -1;
    snprintf(log_path, sizeof log_path, "%s", path);
    return 0;
}
static void log_write(const char *s, size_t n)
{
    if (log_f) { fwrite(s, 1, n, log_f); fflush(log_f); }
    if (log_stderr) { fwrite(s, 1, n, stderr); fflush(stderr); }
}
static size_t log_stamp(char *b, size_t cap)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm tm;
    time_t t = tv.tv_sec;
    localtime_r(&t, &tm);
    return (size_t)snprintf(b, cap, "%04d-%02d-%02d %02d:%02d:%02d.%03d ", tm.tm_year + 1900, tm.tm_mon + 1,
                            tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, (int)(tv.tv_usec / 1000));
}
#endif

const char *plat_log_path(void) { return log_path; }

void sbl_log(const char *fmt, ...)
{
    if (!log_init_done) { plat_mutex_init(&log_lock); log_init_done = 1; }
    char b[1024];
    size_t n = log_stamp(b, sizeof b);
    va_list ap;
    va_start(ap, fmt);
    int k = vsnprintf(b + n, sizeof b - n - 2, fmt, ap);
    va_end(ap);
    if (k < 0) k = 0;
    n += (size_t)k < sizeof b - n - 2 ? (size_t)k : sizeof b - n - 3;
#ifdef _WIN32
    b[n++] = '\r';
#endif
    b[n++] = '\n';
    plat_mutex_lock(&log_lock);
    log_write(b, n);
    plat_mutex_unlock(&log_lock);
}
