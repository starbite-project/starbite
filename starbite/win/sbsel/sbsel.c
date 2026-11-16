/* SPDX-License-Identifier: MIT
 *
 * sbsel.dll: which Starbite D3D user-mode driver this boot uses (docs/WIN11-GPU.md 6.108).
 *
 * The user wants the driver with the GPU fixes (sbumd 0167: flip-model windows, Geometry Dash at 60) as its own
 * entry on the Starbite OS list, next to the Windows entries they have, which must keep booting the driver they
 * have (T61's sbumd.dll, byte for byte). Both entries start the same Windows install and the same firmware image,
 * so Windows cannot tell them apart by hardware. What differs is the catalog row's `wdt`: the loader arms the
 * watchdog with it, and sbwdog.sys (boot start) records the reload value it found in
 *   HKLM\SYSTEM\CurrentControlSet\Services\sbwdog\State\WtDat
 * before it takes the watchdog over (82 s = 0xFE27 for the existing rows). The new row has wdt = 80.
 *
 * The display adapter's UserModeDriverName is this DLL. The D3D runtime calls OpenAdapter10 / OpenAdapter10_2
 * here; the first call loads %SystemRoot%\System32\sbumd-ng.dll when WtDat says "80 s", else (any other value,
 * no value, no access, or sbumd-ng.dll missing or failing to load) %SystemRoot%\System32\sbumd.dll, and every call
 * goes to the loaded driver's function of the same name. No state besides the loaded module, no C++ (an ARM64X
 * image must have no C++ initializers, 6.106). The real driver stays loaded for the life of the process.
 *
 * Only for tests: when the file C:\ProgramData\Starbite\sbsellog exists, each process writes one line to its own
 * file C:\ProgramData\Starbite\umdlog\sbsel-<pid>.log (one file per process: a file one account created is
 * read-only to the others): its exe, pid, the WtDat it read and the driver it loaded. That is how a test shows
 * what DWM (another account) loaded. Without the switch file nothing is written.
 *
 * Known difference to loading the driver directly: the runtime's FreeLibrary of its user-mode driver now unloads
 * only this DLL; the real driver stays, with its globals, until the process ends.
 *
 * The watchdog counts at 793.457 Hz (sbwdog.c: prescaler 255, DIV128): N s = N * 793.457 counts.
 * 80 s = 63476 or 63477 (0xF7F4/5); 82 s = 65063 (0xFE27). Accepted for the new row: 63400..63560 (79.9..80.1 s).
 */
#include <windows.h>

#define SB_NG_LO 63400u
#define SB_NG_HI 63560u

typedef HRESULT (APIENTRY *SB_OPEN)(void *);

static PVOID volatile sb_mod;   /* the driver all calls go to; set once, the first loader wins */

/* 1: this boot came from the new-GPU row. *dat: the value read; *err: 0, or the Win32 error that hid it. */
static int
sb_new_gpu_row(DWORD *dat, LONG *err)
{
   DWORD v = 0, n = sizeof(v), type = 0;
   HKEY k;
   int ng = 0;
   *dat = 0;
   *err = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\sbwdog\\State", 0,
                        KEY_QUERY_VALUE, &k);
   if (*err != ERROR_SUCCESS)
      return 0;
   *err = RegQueryValueExW(k, L"WtDat", NULL, &type, (BYTE *)&v, &n);
   if (*err == ERROR_SUCCESS && type == REG_DWORD && n == sizeof(v)) {
      *dat = v;
      ng = v >= SB_NG_LO && v <= SB_NG_HI;
   }
   RegCloseKey(k);
   return ng;
}

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

/* the test log (see the top of the file); every failure here is ignored */
static void
sb_log(DWORD dat, LONG err, int ng, HMODULE m)
{
   char exe[MAX_PATH], mod[MAX_PATH], line[400], name[80], *p = line, *end = line + sizeof(line) - 2, *q;
   const char *e = exe, *d = mod;
   DWORD n, w;
   HANDLE f;
   if (GetFileAttributesW(L"C:\\ProgramData\\Starbite\\sbsellog") == INVALID_FILE_ATTRIBUTES)
      return;
   n = GetModuleFileNameA(NULL, exe, sizeof(exe));
   exe[n < sizeof(exe) ? n : sizeof(exe) - 1] = 0;
   for (const char *c = exe; *c; c++)
      if (*c == '\\')
         e = c + 1;
   n = m ? GetModuleFileNameA(m, mod, sizeof(mod)) : 0;
   mod[n < sizeof(mod) ? n : sizeof(mod) - 1] = 0;
   for (const char *c = mod; *c; c++)
      if (*c == '\\')
         d = c + 1;
   p = sb_put(p, end, "sbsel: ");
   p = sb_put(p, end, e);
   p = sb_put(p, end, " pid ");
   p = sb_put_u(p, end, GetCurrentProcessId());
#if defined(_M_ARM64EC)
   p = sb_put(p, end, " (x64)");
#else
   p = sb_put(p, end, " (arm64)");
#endif
   p = sb_put(p, end, " WtDat ");
   p = sb_put_u(p, end, dat);
   if (err) {
      p = sb_put(p, end, " (read error ");
      p = sb_put_u(p, end, (unsigned long)err);
      p = sb_put(p, end, ")");
   }
   p = sb_put(p, end, ng ? " row NEW-GPU -> " : " row standard -> ");
   p = sb_put(p, end, m ? d : "NO DRIVER LOADED");
   *p++ = '\r';
   *p++ = '\n';
   q = sb_put(name, name + sizeof(name) - 16, "C:\\ProgramData\\Starbite\\umdlog\\sbsel-");
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

static HMODULE
sb_load(const wchar_t *name)
{
   wchar_t path[MAX_PATH + 32];
   UINT n = GetSystemDirectoryW(path, MAX_PATH);
   UINT i = 0;
   if (n == 0 || n >= MAX_PATH)
      return NULL;
   path[n++] = L'\\';
   while (name[i] && i < 24)
      path[n++] = name[i++];
   path[n] = 0;
   return LoadLibraryW(path);
}

static HMODULE
sb_driver(void)
{
   HMODULE m = (HMODULE)sb_mod, won;
   DWORD dat;
   LONG err;
   int ng;
   if (m)
      return m;
   ng = sb_new_gpu_row(&dat, &err);
   if (ng)
      m = sb_load(L"sbumd-ng.dll");
   if (!m)
      m = sb_load(L"sbumd.dll");
   if (!m) {
      sb_log(dat, err, ng, NULL);
      return NULL;
   }
   /* two threads at once: both use the module the first one stored, the other reference is dropped */
   won = (HMODULE)InterlockedCompareExchangePointer(&sb_mod, m, NULL);
   if (won) {
      FreeLibrary(m);
      return won;
   }
   sb_log(dat, err, ng, m);
   return m;
}

static HRESULT
sb_forward(const char *fn, void *arg)
{
   HMODULE m = sb_driver();
   SB_OPEN f = m ? (SB_OPEN)(void *)GetProcAddress(m, fn) : NULL;
   return f ? f(arg) : E_FAIL;
}

HRESULT APIENTRY
OpenAdapter10(void *pOpenData)
{
   return sb_forward("OpenAdapter10", pOpenData);
}

HRESULT APIENTRY
OpenAdapter10_2(void *pOpenData)
{
   return sb_forward("OpenAdapter10_2", pOpenData);
}

/* For a test program: 1 the new-GPU row's driver is the one loaded here, 0 the standard one, -1 none. */
int APIENTRY
SbSelWhich(void)
{
   wchar_t p[MAX_PATH];
   HMODULE m = sb_driver();
   DWORD n = m ? GetModuleFileNameW(m, p, MAX_PATH) : 0;
   if (!n || n >= MAX_PATH)
      return -1;
   /* ...\sbumd-ng.dll ends in "-ng.dll" */
   return n > 7 && p[n - 7] == L'-' && (p[n - 6] | 32) == L'n' && (p[n - 5] | 32) == L'g';
}

BOOL WINAPI
DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
   (void)inst;
   (void)reason;
   (void)reserved;
   return TRUE;
}
