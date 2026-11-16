/* SPDX-License-Identifier: MIT
 *
 * sbglicd.dll: the Starbite OpenGL installable client driver as the system sees it (build/win/patchwork/
 * glsys-report.md).
 *
 * The display adapter's OpenGLDriverName names this DLL. The system opengl32.dll loads it into any program that
 * asks for OpenGL on a window of our adapter, native ARM64 or x64-on-ARM, so the file in System32 must be ARM64X
 * (an ARM64 and an ARM64EC half in one file). Mesa's GL driver cannot be linked that way by us: in our
 * hand-linked ARM64X images both halves' C++ initializer tables are merged and both start-ups run both
 * (docs/WIN11-GPU.md 6.106, T77's crash), and the GL driver has a C++ dynamic initializer with a destructor
 * (the GLSL compiler's `builtins` object; more in the archives). So the driver stays two ordinary DLLs, each
 * linked by meson exactly as the game kit's is:
 *     System32\sbgl-arm64.dll   Mesa's libgallium_wgl.dll, ARM64     (native processes)
 *     System32\sbgl-x64.dll     Mesa's libgallium_wgl.dll, ARM64EC   (x64 and ARM64EC processes)
 * and this file, C only with no initializer tables, is the ARM64X front: its ARM64 half runs in native
 * processes and loads the first, its ARM64EC half runs in x64 processes and loads the second, by full System32
 * path, on the first Drv* call (never in DllMain), once per process. Every Drv* call then goes to the function
 * of the same name there. The driver stays loaded for the life of the process.
 *
 * When the driver must not or cannot be used, every entry point fails (DrvValidateVersion FALSE,
 * DrvDescribePixelFormat 0, no contexts): the system opengl32 then goes on with Microsoft's own implementation,
 * which is what the program had before the ICD was registered. That happens when:
 *   - C:\ProgramData\Starbite\glsys\off exists                      (the switch for everything, no hive edit);
 *   - C:\ProgramData\Starbite\glsys\off-<exe name> exists           (one program, e.g. off-javaw.exe);
 *   - C:\ProgramData\Starbite\glsys\only exists and
 *     C:\ProgramData\Starbite\glsys\on-<exe name> does not          (allow list: a first test boot);
 *   - the implementation DLL is missing, does not load, or lacks one of the entry points.
 * The files are looked at once per process, at the first Drv* call.
 *
 * Only for tests: with C:\ProgramData\Starbite\glsys\log each process writes one line to its own file
 * C:\ProgramData\Starbite\umdlog\sbglicd-<pid>.log (exe, pid, half, the decision, the DLL or the error).
 *
 * No CRT state, no C++, no threads, no window or GDI calls of its own.
 */
#include <windows.h>

#if defined(_M_ARM64EC)
#define SB_IMPL L"sbgl-x64.dll"
#define SB_HALF " (x64)"
#else
#define SB_IMPL L"sbgl-arm64.dll"
#define SB_HALF " (arm64)"
#endif

#define SB_DIR "C:\\ProgramData\\Starbite\\glsys\\"

/* the entry points Mesa's libgallium_wgl.dll exports for opengl32 (src/gallium/targets/wgl/gallium_wgl.def.in,
 * prototypes: src/gallium/frontends/wgl/gldrv.h); opaque types are pointers or handles here */
#define SB_FNS(X) \
   X(BOOL, FALSE, DrvCopyContext, (HANDLE a, HANDLE b, UINT c), (a, b, c)) \
   X(HANDLE, NULL, DrvCreateContext, (HDC a), (a)) \
   X(HANDLE, NULL, DrvCreateLayerContext, (HDC a, int b), (a, b)) \
   X(BOOL, FALSE, DrvDeleteContext, (HANDLE a), (a)) \
   X(BOOL, FALSE, DrvDescribeLayerPlane, (HDC a, INT b, INT c, UINT d, void *e), (a, b, c, d, e)) \
   X(LONG, 0, DrvDescribePixelFormat, (HDC a, INT b, ULONG c, void *d), (a, b, c, d)) \
   X(INT, 0, DrvGetLayerPaletteEntries, (HDC a, INT b, INT c, INT d, void *e), (a, b, c, d, e)) \
   X(PROC, NULL, DrvGetProcAddress, (LPCSTR a), (a)) \
   X(BOOL, FALSE, DrvPresentBuffers, (HDC a, void *b), (a, b)) \
   X(BOOL, FALSE, DrvRealizeLayerPalette, (HDC a, INT b, BOOL c), (a, b, c)) \
   X(BOOL, FALSE, DrvReleaseContext, (HANDLE a), (a)) \
   X(void *, NULL, DrvSetContext, (HDC a, HANDLE b, void *c), (a, b, c)) \
   X(INT, 0, DrvSetLayerPaletteEntries, (HDC a, INT b, INT c, INT d, const void *e), (a, b, c, d, e)) \
   X(BOOL, FALSE, DrvSetPixelFormat, (HDC a, LONG b), (a, b)) \
   X(BOOL, FALSE, DrvShareLists, (HANDLE a, HANDLE b), (a, b)) \
   X(BOOL, FALSE, DrvSwapBuffers, (HDC a), (a)) \
   X(BOOL, FALSE, DrvSwapLayerBuffers, (HDC a, UINT b), (a, b)) \
   X(BOOL, FALSE, DrvValidateVersion, (ULONG a), (a))

typedef void (APIENTRY *SB_SETCB)(INT, PROC *);

static struct {
#define X(ret, fail, name, args, call) ret (APIENTRY *name) args;
   SB_FNS(X)
#undef X
   SB_SETCB DrvSetCallbackProcs;
} sb_fn;

static SRWLOCK sb_lock = SRWLOCK_INIT;
static LONG volatile sb_state;   /* 0: not decided, 1: sb_fn is filled, -1: no driver in this process */

static char *
sb_put(char *p, char *end, const char *s)
{
   while (*s && p < end)
      *p++ = *s++;
   return p;
}

static char *
sb_put_u(char *p, char *end, unsigned long v)
{
   char t[12];
   int i = 0;
   do {
      t[i++] = (char)('0' + v % 10);
      v /= 10;
   } while (v && i < 11);
   while (i && p < end)
      *p++ = t[--i];
   return p;
}

/* the program's file name (no folder) */
static const char *
sb_exe(char *buf, DWORD size)
{
   const char *e = buf;
   DWORD n = GetModuleFileNameA(NULL, buf, size);
   buf[n < size ? n : size - 1] = 0;
   for (const char *c = buf; *c; c++)
      if (*c == '\\')
         e = c + 1;
   return e;
}

/* C:\ProgramData\Starbite\glsys\<a><b> exists */
static int
sb_file(const char *a, const char *b)
{
   char path[MAX_PATH + 64], *p = path, *end = path + sizeof(path) - 1;
   p = sb_put(p, end, SB_DIR);
   p = sb_put(p, end, a);
   p = sb_put(p, end, b);
   *p = 0;
   return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

/* the test log (see the top of the file); every failure here is ignored */
static void
sb_log(const char *exe, const char *what, const char *detail, unsigned long err)
{
   char line[500], name[80], *p = line, *end = line + sizeof(line) - 2, *q;
   DWORD w;
   HANDLE f;
   if (!sb_file("log", ""))
      return;
   p = sb_put(p, end, "sbglicd: ");
   p = sb_put(p, end, exe);
   p = sb_put(p, end, " pid ");
   p = sb_put_u(p, end, GetCurrentProcessId());
   p = sb_put(p, end, SB_HALF ": ");
   p = sb_put(p, end, what);
   if (detail) {
      p = sb_put(p, end, " ");
      p = sb_put(p, end, detail);
   }
   if (err) {
      p = sb_put(p, end, " (error ");
      p = sb_put_u(p, end, err);
      p = sb_put(p, end, ")");
   }
   *p++ = '\r';
   *p++ = '\n';
   q = sb_put(name, name + sizeof(name) - 16, "C:\\ProgramData\\Starbite\\umdlog\\sbglicd-");
   q = sb_put_u(q, name + sizeof(name) - 6, GetCurrentProcessId());
   q = sb_put(q, name + sizeof(name) - 1, ".log");
   *q = 0;
   f = CreateFileA(name, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_ALWAYS,
                   FILE_ATTRIBUTE_NORMAL, NULL);
   if (f == INVALID_HANDLE_VALUE)
      return;
   WriteFile(f, line, (DWORD)(p - line), &w, NULL);
   CloseHandle(f);
}

/* Decides once per process and fills sb_fn. Called with sb_lock held; returns the new state (1 or -1). */
static LONG
sb_decide(void)
{
   char exebuf[MAX_PATH], narrow[MAX_PATH + 32];
   const char *exe = sb_exe(exebuf, sizeof(exebuf)), *missing = NULL;
   wchar_t path[MAX_PATH + 32];
   HMODULE m;
   UINT n, i;

   if (sb_file("off", "")) {
      sb_log(exe, "OFF for every program (glsys\\off): Microsoft's OpenGL", NULL, 0);
      return -1;
   }
   if (sb_file("off-", exe)) {
      sb_log(exe, "OFF for this program (glsys\\off-<exe>): Microsoft's OpenGL", NULL, 0);
      return -1;
   }
   if (sb_file("only", "") && !sb_file("on-", exe)) {
      sb_log(exe, "OFF: allow list (glsys\\only) without glsys\\on-<exe>: Microsoft's OpenGL", NULL, 0);
      return -1;
   }

   n = GetSystemDirectoryW(path, MAX_PATH);
   if (n == 0 || n >= MAX_PATH) {
      sb_log(exe, "NO DRIVER: no system directory", NULL, GetLastError());
      return -1;
   }
   path[n++] = L'\\';
   for (i = 0; SB_IMPL[i] && i < 24; i++)
      path[n++] = SB_IMPL[i];
   path[n] = 0;
   for (i = 0; i <= n; i++)
      narrow[i] = (char)path[i];   /* for the log: the system directory is ASCII on our install */

   m = LoadLibraryW(path);
   if (!m) {
      sb_log(exe, "NO DRIVER: cannot load", narrow, GetLastError());
      return -1;
   }
#define X(ret, fail, name, args, call) \
   sb_fn.name = (ret (APIENTRY *) args)(void *)GetProcAddress(m, #name); \
   if (!sb_fn.name && !missing) \
      missing = #name;
   SB_FNS(X)
#undef X
   sb_fn.DrvSetCallbackProcs = (SB_SETCB)(void *)GetProcAddress(m, "DrvSetCallbackProcs");
   if (!sb_fn.DrvSetCallbackProcs && !missing)
      missing = "DrvSetCallbackProcs";
   if (missing) {
      sb_log(exe, "NO DRIVER: entry point missing in the driver:", missing, 0);
      FreeLibrary(m);
      return -1;
   }
   sb_log(exe, "driver", narrow, 0);
   return 1;
}

/* 1: sb_fn is usable; 0: this process gets no driver */
static int
sb_ready(void)
{
   LONG s = sb_state;
   if (s == 0) {
      AcquireSRWLockExclusive(&sb_lock);
      s = sb_state;
      if (s == 0) {
         s = sb_decide();
         InterlockedExchange(&sb_state, s);   /* after sb_fn is complete */
      }
      ReleaseSRWLockExclusive(&sb_lock);
   }
   return s > 0;
}

#define X(ret, fail, name, args, call) \
   ret APIENTRY name args \
   { \
      if (!sb_ready()) \
         return fail; \
      return sb_fn.name call; \
   }
SB_FNS(X)
#undef X

void APIENTRY
DrvSetCallbackProcs(INT nProcs, PROC *pProcs)
{
   if (sb_ready())
      sb_fn.DrvSetCallbackProcs(nProcs, pProcs);
}

/* For a test program: 1 the driver is loaded in this process, 0 this process gets Microsoft's OpenGL
 * (a switch file, or the driver did not load). Decides if nothing has yet. */
int APIENTRY
SbGlIcdState(void)
{
   return sb_ready();
}

BOOL WINAPI
DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
   (void)inst;
   (void)reason;
   (void)reserved;
   return TRUE;
}
