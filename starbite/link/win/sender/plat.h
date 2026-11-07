/* SPDX-License-Identifier: GPL-2.0-only
 * sblinkd portability layer: mutex, auto-reset event, detached thread, monotonic clock, log.
 * Win32 on the phone, POSIX for the Mac host build (synthetic capture + tests). plat.c has both.
 */
#ifndef SBL_PLAT_H
#define SBL_PLAT_H

#include <stdint.h>
#include <stddef.h>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <windows.h>
typedef CRITICAL_SECTION plat_mutex;
typedef HANDLE plat_event;
#else
#  include <pthread.h>
typedef pthread_mutex_t plat_mutex;
typedef struct plat_event_s *plat_event;
#endif

void plat_mutex_init(plat_mutex *m);
void plat_mutex_lock(plat_mutex *m);
void plat_mutex_unlock(plat_mutex *m);

plat_event plat_event_new(void);               /* auto-reset, initially clear */
void plat_event_set(plat_event e);
int  plat_event_wait(plat_event e, int ms);    /* 1 signalled, 0 timeout */

int  plat_thread(void (*fn)(void *), void *arg);   /* detached; 0 ok */
uint64_t plat_now_us(void);                    /* monotonic */
void plat_sleep_ms(int ms);

/* Log: path NULL = the default (Windows: C:\starbite\link\sblinkd.log, else %LOCALAPPDATA%\Starbite\Link;
 * host: none). to_stderr: also print. Returns 0 if the file is open. */
int  plat_log_open(const char *path, int to_stderr);
const char *plat_log_path(void);
void sbl_log(const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;

#endif
