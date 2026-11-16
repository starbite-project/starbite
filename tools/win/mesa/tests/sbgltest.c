/* sbgltest.c: OpenGL through the SYSTEM opengl32.dll (build/win/patchwork/glsys-report.md): is the Starbite
 * ICD (sbglicd.dll -> sbgl-arm64.dll / sbgl-x64.dll) what an ordinary program gets, and does it draw?
 *
 *   sbgltest.exe [hw|ms|any] [LOG] [TIMED_FRAMES]
 *     hw  (default)  PASS needs the hardware driver: the system opengl32.dll, an ICD pixel format, a renderer
 *                    that is neither Microsoft's "GDI Generic" nor a Mesa software one, GLSL frames read back right
 *     ms             PASS needs Microsoft's own OpenGL ("GDI Generic"): the checks for the switch files
 *                    (glsys\off, off-<exe>, only) and for a process that cannot reach the Mali (MALIKM_WDDM=0);
 *                    it says which of the ICD's DLLs were loaded all the same
 *     any            PASS needs only the frames to read back right
 *   LOG defaults to sbgltest.log (appended); TIMED_FRAMES to 120.
 *
 * No app-local DLL: put NO opengl32.dll or libgallium_wgl.dll next to this program (it says so and fails "hw").
 * What it does and logs, with times:
 *   1 which opengl32.dll the process has; what the system names as the ICD for the window's adapter
 *     (D3DKMTQueryAdapterInfo KMTQAITYPE_UMOPENGLINFO: the registry's OpenGLDriverName / Version / Flags);
 *   2 the pixel formats (how many from the ICD, how many Microsoft generic), the one chosen;
 *   3 a context: GL_VENDOR / GL_RENDERER / GL_VERSION / GLSL version; the best core-profile context offered;
 *   4 30 checked frames: clear + a quad drawn by a GLSL program (GL 2.0+; fixed function on GL 1.x), colour
 *     changing per frame, glReadPixels of the centre (the quad) and a corner (the clear), SwapBuffers;
 *   5 TIMED_FRAMES frames without read-back: SwapBuffers pace;
 *   6 which driver DLLs ended up loaded, and the verdict.
 * Exit code: 0 PASS, 1 FAIL, 100 + step for a setup failure. Plain C; builds for ARM64, ARM64EC and x64.
 */
#include <windows.h>
#include <GL/gl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "opengl32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

typedef char GLchar;
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_SHADING_LANGUAGE_VERSION 0x8B8C
#define WGL_CONTEXT_MAJOR_VERSION_ARB 0x2091
#define WGL_CONTEXT_MINOR_VERSION_ARB 0x2092
#define WGL_CONTEXT_PROFILE_MASK_ARB 0x9126
#define WGL_CONTEXT_CORE_PROFILE_BIT_ARB 0x0001

#define FNS(X) \
   X(GLuint, glCreateShader, (GLenum)) X(void, glShaderSource, (GLuint, GLsizei, const GLchar *const *, const GLint *)) \
   X(void, glCompileShader, (GLuint)) X(void, glGetShaderiv, (GLuint, GLenum, GLint *)) \
   X(void, glGetShaderInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *)) X(GLuint, glCreateProgram, (void)) \
   X(void, glAttachShader, (GLuint, GLuint)) X(void, glLinkProgram, (GLuint)) \
   X(void, glGetProgramiv, (GLuint, GLenum, GLint *)) X(void, glGetProgramInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *)) \
   X(void, glUseProgram, (GLuint)) X(void, glBindAttribLocation, (GLuint, GLuint, const GLchar *)) \
   X(GLint, glGetUniformLocation, (GLuint, const GLchar *)) X(void, glUniform4f, (GLint, GLfloat, GLfloat, GLfloat, GLfloat)) \
   X(void, glEnableVertexAttribArray, (GLuint)) \
   X(void, glVertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *))
#define DECL(r, n, a) typedef r (APIENTRY *PFN_##n) a; static PFN_##n p_##n;
FNS(DECL)

typedef HGLRC (WINAPI *PFN_wglCreateContextAttribsARB)(HDC, HGLRC, const int *);

/* gdi32's D3DKMT calls, declared here (d3dkmthk.h: D3DKMT_OPENADAPTERFROMHDC, D3DKMT_QUERYADAPTERINFO,
 * D3DKMT_OPENGLINFO, KMTQAITYPE_UMOPENGLINFO = 2) */
typedef struct { HDC hDc; UINT hAdapter; LUID AdapterLuid; UINT VidPnSourceId; } SB_OPENADAPTERFROMHDC;
typedef struct { UINT hAdapter; UINT Type; void *pPrivateDriverData; UINT PrivateDriverDataSize; } SB_QUERYADAPTERINFO;
typedef struct { WCHAR UmdOpenGlIcdFileName[MAX_PATH]; ULONG Version; ULONG Flags; } SB_OPENGLINFO;
typedef struct { UINT hAdapter; } SB_CLOSEADAPTER;
typedef LONG (APIENTRY *SB_KMT)(void *);

#if defined(_M_ARM64EC)
#define SB_BUILD "ARM64EC (runs as an x64 process)"
#elif defined(_M_ARM64)
#define SB_BUILD "ARM64"
#elif defined(_M_X64)
#define SB_BUILD "x64 (emulated on this phone)"
#else
#define SB_BUILD "other"
#endif

static FILE *logfile;
static LARGE_INTEGER qpf;

static void
say(const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   vprintf(fmt, ap);
   va_end(ap);
   fflush(stdout);
   if (logfile) {
      va_start(ap, fmt);
      vfprintf(logfile, fmt, ap);
      va_end(ap);
      fflush(logfile);
   }
}

static double
now_ms(void)
{
   LARGE_INTEGER t;
   QueryPerformanceCounter(&t);
   return (double)t.QuadPart * 1000.0 / (double)qpf.QuadPart;
}

static int
bail(int step, const char *what)
{
   say("SETUP FAILURE at step %d: %s (Win32 error %lu)\nsbgltest: FAIL (exit %d)\n", step, what,
       (unsigned long)GetLastError(), 100 + step);
   return 100 + step;
}

/* the file a loaded module came from; "" when it is not loaded */
static const char *
module_path(const char *name, char *buf, DWORD size)
{
   HMODULE m = GetModuleHandleA(name);
   DWORD n = m ? GetModuleFileNameA(m, buf, size) : 0;
   buf[n < size ? n : size - 1] = 0;
   return buf;
}

static LRESULT CALLBACK
wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
   if (m == WM_CLOSE) {
      PostQuitMessage(0);
      return 0;
   }
   return DefWindowProcA(h, m, w, l);
}

static int
pump(void)
{
   MSG msg;
   while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT)
         return 0;
      TranslateMessage(&msg);
      DispatchMessageA(&msg);
   }
   return 1;
}

/* what the system says the adapter's OpenGL ICD is (the registry values, through dxgkrnl) */
static void
log_registered_icd(HDC dc)
{
   HMODULE g = GetModuleHandleA("gdi32.dll");
   SB_KMT open = g ? (SB_KMT)(void *)GetProcAddress(g, "D3DKMTOpenAdapterFromHdc") : NULL;
   SB_KMT query = g ? (SB_KMT)(void *)GetProcAddress(g, "D3DKMTQueryAdapterInfo") : NULL;
   SB_KMT close = g ? (SB_KMT)(void *)GetProcAddress(g, "D3DKMTCloseAdapter") : NULL;
   SB_OPENADAPTERFROMHDC oa;
   SB_QUERYADAPTERINFO qa;
   SB_OPENGLINFO info;
   SB_CLOSEADAPTER ca;
   LONG st;
   HDC screen = NULL;

   if (!open || !query || !close) {
      say("registered ICD: gdi32 has no D3DKMT calls here\n");
      return;
   }
   memset(&oa, 0, sizeof(oa));
   oa.hDc = dc;
   st = open(&oa);
   if (st < 0) {   /* a window DC was not accepted: the display's own DC */
      screen = CreateDCA("DISPLAY", NULL, NULL, NULL);
      memset(&oa, 0, sizeof(oa));
      oa.hDc = screen;
      st = screen ? open(&oa) : st;
   }
   if (st < 0) {
      say("registered ICD: D3DKMTOpenAdapterFromHdc failed (0x%08lx)\n", (unsigned long)st);
   } else {
      memset(&info, 0, sizeof(info));
      memset(&qa, 0, sizeof(qa));
      qa.hAdapter = oa.hAdapter;
      qa.Type = 2; /* KMTQAITYPE_UMOPENGLINFO */
      qa.pPrivateDriverData = &info;
      qa.PrivateDriverDataSize = sizeof(info);
      st = query(&qa);
      if (st < 0)
         say("registered ICD: KMTQAITYPE_UMOPENGLINFO failed (0x%08lx): no OpenGLDriverName for this adapter?\n",
             (unsigned long)st);
      else
         say("registered ICD (adapter luid %08lx:%08lx): name \"%ls\", version %lu, flags 0x%lx%s\n",
             (unsigned long)oa.AdapterLuid.HighPart, (unsigned long)oa.AdapterLuid.LowPart,
             info.UmdOpenGlIcdFileName, (unsigned long)info.Version, (unsigned long)info.Flags,
             info.UmdOpenGlIcdFileName[0] ? "" : "  (empty: no ICD registered)");
      ca.hAdapter = oa.hAdapter;
      close(&ca);
   }
   if (screen)
      DeleteDC(screen);
}

static GLuint
shader(GLenum type, const char *src)
{
   GLuint s = p_glCreateShader(type);
   GLint ok = 0;
   p_glShaderSource(s, 1, &src, NULL);
   p_glCompileShader(s);
   p_glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
   if (!ok) {
      char log[512] = "";
      p_glGetShaderInfoLog(s, sizeof(log), NULL, log);
      say("shader compile failed: %s\n", log);
      return 0;
   }
   return s;
}

static int
near255(int got, float want)
{
   int w = (int)(want * 255.0f + 0.5f);
   return got >= w - 3 && got <= w + 3;
}

/* the colour of frame i's quad: always far from the clear colour */
static void
frame_colour(int i, float *c)
{
   c[0] = (float)((i * 53) % 200 + 55) / 255.0f;
   c[1] = (float)((i * 97) % 156 + 99) / 255.0f;
   c[2] = (float)((i * 29) % 100) / 255.0f;
}

static const float clear_c[3] = {0.10f, 0.20f, 0.60f};
static const float quad[8] = {-0.5f, -0.5f, 0.5f, -0.5f, 0.5f, 0.5f, -0.5f, 0.5f};

static void
draw(int i, int glsl, GLint ucol, int w, int h)
{
   float c[3];
   frame_colour(i, c);
   glViewport(0, 0, w, h);
   glClearColor(clear_c[0], clear_c[1], clear_c[2], 1.0f);
   glClear(GL_COLOR_BUFFER_BIT);
   if (glsl) {
      p_glUniform4f(ucol, c[0], c[1], c[2], 1.0f);
      glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
   } else {
      glColor3f(c[0], c[1], c[2]);
      glBegin(GL_QUADS);
      for (int v = 0; v < 4; v++)
         glVertex2f(quad[2 * v], quad[2 * v + 1]);
      glEnd();
   }
}

int
main(int argc, char **argv)
{
   const char *expect = argc > 1 ? argv[1] : "hw";
   int timed = argc > 3 ? atoi(argv[3]) : 120;
   char path[MAX_PATH], sysdir[MAX_PATH], exe[MAX_PATH];
   int fails = 0;
   double t0, t1;

   QueryPerformanceFrequency(&qpf);
   logfile = fopen(argc > 2 ? argv[2] : "sbgltest.log", "a");
   if (strcmp(expect, "hw") && strcmp(expect, "ms") && strcmp(expect, "any")) {
      say("usage: sbgltest.exe [hw|ms|any] [LOG] [TIMED_FRAMES]\n");
      return 100;
   }
   if (timed < 1 || timed > 100000)
      timed = 120;
   GetModuleFileNameA(NULL, exe, sizeof(exe));
   GetSystemDirectoryA(sysdir, sizeof(sysdir));
   {
      SYSTEMTIME st;
      USHORT pm = 0, nm = 0;
      BOOL (WINAPI *wow2)(HANDLE, USHORT *, USHORT *) =
         (BOOL (WINAPI *)(HANDLE, USHORT *, USHORT *))(void *)GetProcAddress(GetModuleHandleA("kernel32.dll"),
                                                                             "IsWow64Process2");
      GetLocalTime(&st);
      if (wow2)
         wow2(GetCurrentProcess(), &pm, &nm);
      say("==== sbgltest %02u:%02u:%02u pid %lu, built for %s, expecting \"%s\"\n", st.wHour, st.wMinute, st.wSecond,
          (unsigned long)GetCurrentProcessId(), SB_BUILD, expect);
      say("exe %s; native machine 0x%04x, WOW64 machine 0x%04x\n", exe, nm, pm);
   }

   /* 1: the opengl32.dll this process has */
   module_path("opengl32.dll", path, sizeof(path));
   int sys_gl = _strnicmp(path, sysdir, strlen(sysdir)) == 0;
   say("opengl32.dll: %s%s\n", path, sys_gl ? " (the system's)" : "  ** NOT the system's: an app-local copy **");

   /* a window */
   WNDCLASSA wc;
   memset(&wc, 0, sizeof(wc));
   wc.style = CS_OWNDC;
   wc.lpfnWndProc = wndproc;
   wc.hInstance = GetModuleHandleA(NULL);
   wc.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
   wc.lpszClassName = "sbgltest";
   if (!RegisterClassA(&wc))
      return bail(1, "RegisterClass");
   RECT r = {0, 0, 640, 480};
   AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
   HWND wnd = CreateWindowA("sbgltest", "sbgltest: system OpenGL", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 40, 40,
                            r.right - r.left, r.bottom - r.top, NULL, NULL, wc.hInstance, NULL);
   if (!wnd)
      return bail(1, "CreateWindow");
   HDC dc = GetDC(wnd);
   pump();
   log_registered_icd(dc);

   /* 2: pixel formats. The first call loads the ICD and makes its screen: the cost every GL-probing program pays */
   PIXELFORMATDESCRIPTOR pfd, got;
   t0 = now_ms();
   int count = DescribePixelFormat(dc, 1, sizeof(got), &got);
   t1 = now_ms();
   int icd = 0, generic = 0;
   for (int i = 1; i <= count; i++) {
      if (DescribePixelFormat(dc, i, sizeof(got), &got) && (got.dwFlags & PFD_SUPPORT_OPENGL)) {
         if (got.dwFlags & PFD_GENERIC_FORMAT)
            generic++;
         else
            icd++;
      }
   }
   say("pixel formats: %d (%d from an ICD, %d Microsoft generic); first DescribePixelFormat %.1f ms\n", count, icd,
       generic, t1 - t0);
   memset(&pfd, 0, sizeof(pfd));
   pfd.nSize = sizeof(pfd);
   pfd.nVersion = 1;
   pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
   pfd.iPixelType = PFD_TYPE_RGBA;
   pfd.cColorBits = 32;
   pfd.cDepthBits = 24;
   pfd.cStencilBits = 8;
   int pf = ChoosePixelFormat(dc, &pfd);
   if (!pf)
      return bail(2, "ChoosePixelFormat");
   DescribePixelFormat(dc, pf, sizeof(got), &got);
   int icd_format = !(got.dwFlags & PFD_GENERIC_FORMAT);
   say("chosen format %d: flags 0x%lx (%s%s%s), colour %d (r%d g%d b%d a%d), depth %d, stencil %d\n", pf,
       (unsigned long)got.dwFlags, icd_format ? "ICD" : "Microsoft generic",
       (got.dwFlags & PFD_GENERIC_ACCELERATED) ? ", MCD" : "", (got.dwFlags & PFD_SUPPORT_COMPOSITION) ? ", composition" : "",
       got.cColorBits, got.cRedBits, got.cGreenBits, got.cBlueBits, got.cAlphaBits, got.cDepthBits, got.cStencilBits);
   if (!SetPixelFormat(dc, pf, &got))
      return bail(2, "SetPixelFormat");

   /* 3: a context */
   t0 = now_ms();
   HGLRC rc = wglCreateContext(dc);
   if (!rc)
      return bail(3, "wglCreateContext");
   t1 = now_ms();
   if (!wglMakeCurrent(dc, rc))
      return bail(3, "wglMakeCurrent");
   double t2 = now_ms();
   const char *vendor = (const char *)glGetString(GL_VENDOR), *renderer = (const char *)glGetString(GL_RENDERER);
   const char *version = (const char *)glGetString(GL_VERSION);
   if (!vendor || !renderer || !version)
      return bail(3, "glGetString");
   int major = 0, minor = 0;
   sscanf(version, "%d.%d", &major, &minor);
   say("wglCreateContext %.1f ms, wglMakeCurrent %.1f ms\n", t1 - t0, t2 - t1);
   say("GL_VENDOR   %s\nGL_RENDERER %s\nGL_VERSION  %s\n", vendor, renderer, version);
   if (major >= 2) {
      const char *sl = (const char *)glGetString(GL_SHADING_LANGUAGE_VERSION);
      say("GLSL        %s\n", sl ? sl : "(none)");
   }
   int ms_gl = strstr(renderer, "GDI Generic") != NULL;
   int soft = ms_gl || strstr(renderer, "softpipe") || strstr(renderer, "llvmpipe");
   say("renderer kind: %s\n", ms_gl ? "Microsoft's software OpenGL" : soft ? "Mesa software" : "hardware driver");

   /* the best core-profile context the driver offers (information only) */
   {
      PFN_wglCreateContextAttribsARB create =
         (PFN_wglCreateContextAttribsARB)(void *)wglGetProcAddress("wglCreateContextAttribsARB");
      static const int tries[][2] = {{4, 6}, {4, 5}, {4, 3}, {4, 0}, {3, 3}, {3, 2}, {3, 1}};
      if (!create) {
         say("core profile: no WGL_ARB_create_context\n");
      } else {
         int found = 0;
         for (unsigned i = 0; i < sizeof(tries) / sizeof(tries[0]) && !found; i++) {
            int attribs[] = {WGL_CONTEXT_MAJOR_VERSION_ARB, tries[i][0], WGL_CONTEXT_MINOR_VERSION_ARB, tries[i][1],
                             WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB, 0};
            HGLRC core = create(dc, NULL, attribs);
            if (core) {
               found = 1;
               if (wglMakeCurrent(dc, core))
                  say("core profile: %d.%d asked and given: GL_VERSION %s\n", tries[i][0], tries[i][1],
                      (const char *)glGetString(GL_VERSION));
               wglMakeCurrent(dc, rc);
               wglDeleteContext(core);
            }
         }
         if (!found)
            say("core profile: none of 4.6 .. 3.1 given\n");
      }
   }

   /* the GLSL program (GL 2.0+), else fixed function */
   int glsl = major >= 2;
   GLint ucol = -1;
   if (glsl) {
      int missing = 0;
#define LOAD(r, n, a) if (!(p_##n = (PFN_##n)(void *)wglGetProcAddress(#n))) { say("missing %s\n", #n); missing++; }
      FNS(LOAD)
      if (missing)
         return bail(4, "GL 2.0 entry points");
      t0 = now_ms();
      GLuint vs = shader(GL_VERTEX_SHADER, "attribute vec2 pos;\nvoid main() { gl_Position = vec4(pos, 0.0, 1.0); }\n");
      GLuint fs = shader(GL_FRAGMENT_SHADER, "uniform vec4 col;\nvoid main() { gl_FragColor = col; }\n");
      if (!vs || !fs)
         return bail(4, "shader compile");
      GLuint prog = p_glCreateProgram();
      GLint ok = 0;
      p_glAttachShader(prog, vs);
      p_glAttachShader(prog, fs);
      p_glBindAttribLocation(prog, 0, "pos");
      p_glLinkProgram(prog);
      p_glGetProgramiv(prog, GL_LINK_STATUS, &ok);
      if (!ok) {
         char log[512] = "";
         p_glGetProgramInfoLog(prog, sizeof(log), NULL, log);
         say("link failed: %s\n", log);
         return bail(4, "program link");
      }
      p_glUseProgram(prog);
      ucol = p_glGetUniformLocation(prog, "col");
      p_glEnableVertexAttribArray(0);
      p_glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
      say("GLSL program compiled and linked in %.1f ms\n", now_ms() - t0);
   } else {
      say("GL %d.%d: no shaders, fixed-function quad\n", major, minor);
   }

   /* 4: checked frames */
   glDisable(GL_DITHER);
   RECT cr;
   GetClientRect(wnd, &cr);
   int w = cr.right > 0 ? cr.right : 1, h = cr.bottom > 0 ? cr.bottom : 1;
   int bad = 0, alive = 1;
   double sum = 0, worst = 0, first = 0;
   const int checked = 30;
   for (int i = 0; i < checked && alive; i++) {
      unsigned char mid[4] = {0}, corner[4] = {0};
      float c[3];
      t0 = now_ms();
      draw(i, glsl, ucol, w, h);
      glReadPixels(w / 2, h / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, mid);
      glReadPixels(2, 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, corner);
      SwapBuffers(dc);
      t1 = now_ms() - t0;
      frame_colour(i, c);
      int ok = near255(mid[0], c[0]) && near255(mid[1], c[1]) && near255(mid[2], c[2]) &&
               near255(corner[0], clear_c[0]) && near255(corner[1], clear_c[1]) && near255(corner[2], clear_c[2]);
      if (!ok) {
         bad++;
         if (bad <= 5)
            say("frame %d WRONG: centre %d,%d,%d (want %.0f,%.0f,%.0f), corner %d,%d,%d (want %.0f,%.0f,%.0f)\n", i,
                mid[0], mid[1], mid[2], c[0] * 255, c[1] * 255, c[2] * 255, corner[0], corner[1], corner[2],
                clear_c[0] * 255, clear_c[1] * 255, clear_c[2] * 255);
      }
      if (i == 0)
         first = t1;
      else {
         sum += t1;
         if (t1 > worst)
            worst = t1;
      }
      alive = pump();
   }
   GLenum err = glGetError();
   say("checked frames: %d of %d read back right (%dx%d); first frame %.1f ms, then avg %.1f / max %.1f ms; glGetError 0x%x\n",
       checked - bad, checked, w, h, first, sum / (checked - 1), worst, err);
   if (bad || err || !alive)
      fails++;

   /* 5: timed frames, no read-back */
   {
      double start = now_ms(), last = start, wmax = 0;
      int n = 0, over50 = 0;
      for (int i = 0; i < timed && alive; i++) {
         draw(checked + i, glsl, ucol, w, h);
         SwapBuffers(dc);
         double t = now_ms();
         if (t - last > wmax)
            wmax = t - last;
         if (t - last > 50.0)
            over50++;
         last = t;
         n++;
         alive = pump();
      }
      glFinish();
      double total = now_ms() - start;
      say("timed frames: %d in %.0f ms = %.1f fps (avg %.1f ms, longest %.1f ms, %d over 50 ms)\n", n, total,
          n ? n * 1000.0 / total : 0.0, n ? total / n : 0.0, wmax, over50);
   }

   /* 6: what is loaded, the verdict */
   {
      static const char *const mods[] = {"sbglicd.dll", "sbgl-arm64.dll", "sbgl-x64.dll", "libgallium_wgl.dll"};
      for (unsigned i = 0; i < sizeof(mods) / sizeof(mods[0]); i++) {
         module_path(mods[i], path, sizeof(path));
         say("loaded %-18s %s\n", mods[i], path[0] ? path : "-");
      }
   }
   int front = GetModuleHandleA("sbglicd.dll") != NULL;
   int impl = GetModuleHandleA("sbgl-arm64.dll") != NULL || GetModuleHandleA("sbgl-x64.dll") != NULL;
   if (!strcmp(expect, "hw")) {
      if (!sys_gl) { say("FAIL: opengl32.dll is not the system's\n"); fails++; }
      if (!icd_format) { say("FAIL: the pixel format is Microsoft's generic one, not the ICD's\n"); fails++; }
      if (soft) { say("FAIL: software renderer \"%s\"\n", renderer); fails++; }
      if (!glsl) { say("FAIL: GL %d.%d has no shaders\n", major, minor); fails++; }
      if (!front || !impl) { say("FAIL: sbglicd.dll + sbgl-*.dll are not both loaded\n"); fails++; }
   } else if (!strcmp(expect, "ms")) {
      if (!ms_gl) { say("FAIL: expected Microsoft's \"GDI Generic\", got \"%s\"\n", renderer); fails++; }
      /* a switch file: sbglicd.dll alone is loaded; no Mali in the process (MALIKM_WDDM=0): the driver DLL is
       * loaded too but offered no pixel formats */
      say("Microsoft's OpenGL with %s\n", impl ? "the driver DLL loaded (it offered no pixel formats)"
                                               : front ? "only the ICD front loaded (a switch file)" : "no ICD loaded at all");
   }
   wglMakeCurrent(NULL, NULL);
   wglDeleteContext(rc);
   ReleaseDC(wnd, dc);
   DestroyWindow(wnd);
   say("sbgltest: %s (expect %s; renderer \"%s\", GL %d.%d)\n", fails ? "FAIL" : "PASS", expect, renderer, major, minor);
   if (logfile)
      fclose(logfile);
   return fails ? 1 : 0;
}
