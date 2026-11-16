// sbtouchlat.exe LOG: how late does Windows hand a TOUCH to a program that only understands the mouse?
// (Track K, docs/WIN11-GPU.md 6.109.) Geometry Dash and most games read WM_LBUTTONDOWN / WM_LBUTTONUP; a touch
// reaches them only after Windows has "promoted" it to a mouse click. This program injects touches
// (InjectTouchInput: the same path a digitizer's reports take from win32k on) into its own window and writes down
// when each message arrives, relative to the injected touch-down:
//    WM_POINTERDOWN   the touch itself, as Windows delivers it to any window
//    WM_LBUTTONDOWN   the promoted click a mouse-only program acts on
//    WM_LBUTTONUP, WM_RBUTTONDOWN (press-and-hold), WM_GESTURE, WM_TOUCH
// Patterns: tap 60 ms, hold 400 ms, hold 1500 ms, drag (150 px over 300 ms). Window kinds:
//    plain      nothing registered (what a game window is)
//    nohold     the tablet window property + WM_TABLET_QUERYSYSTEMGESTURESTATUS answer that disable press-and-hold,
//               flicks and tap feedback for this window
//    nogesture  as plain, but every gesture blocked with SetGestureConfig at WM_GESTURENOTIFY
//    touchwin   RegisterTouchWindow (the window asks for raw WM_TOUCH)
// If WM_LBUTTONDOWN comes at the release (+60 / +400 / +1500 ms) while WM_POINTERDOWN comes at once, Windows is
// holding the click back, and a program (or our GL driver inside the game) that turns WM_POINTERDOWN into the click
// itself would react that much sooner.
#include <windows.h>
#include <stdio.h>
#include <string.h>
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

#ifndef WM_TABLET_QUERYSYSTEMGESTURESTATUS
#define WM_TABLET_QUERYSYSTEMGESTURESTATUS 0x02CC
#endif
#define SB_TABLET_FLAGS (0x00000001 /* no press-and-hold */ | 0x00000008 /* no pen tap feedback */ | \
                         0x00000010 /* no barrel feedback */ | 0x00000100 /* no touch UI force on */ | \
                         0x00010000 /* no flicks */)

enum { K_POINTERDOWN, K_POINTERUP, K_LDOWN, K_LUP, K_RDOWN, K_MOVE, K_GESTURE, K_TOUCH, K_N };
static const char *kname[K_N] = {"pointerdown", "pointerup", "lbuttondown", "lbuttonup", "rbuttondown", "mousemove",
                                 "gesture", "wm_touch"};
enum { W_PLAIN, W_NOHOLD, W_NOGESTURE, W_TOUCHWIN, W_N };
static const char *wname[W_N] = {"plain", "nohold", "nogesture", "touchwin"};

static LARGE_INTEGER qf;
static volatile LONGLONG t0;              // QPC of the injected touch-down of the running trial (0: none)
static volatile LONG first_us[K_N];       // first arrival of each message kind, microseconds after t0 (-1: none)
static volatile LONG promoted;            // 1: the mouse messages carried the touch signature in their extra info
static int wkind;
static HWND hwnd;
static FILE *lf;

static LONGLONG qpc(void) { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }

static void seen(int k)
{
   LONGLONG s = t0;
   if (!s)
      return;
   LONG us = (LONG)((qpc() - s) * 1000000 / qf.QuadPart);
   InterlockedCompareExchange(&first_us[k], us, -1);
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
   switch (m) {
   case WM_POINTERDOWN: seen(K_POINTERDOWN); break;
   case WM_POINTERUP: seen(K_POINTERUP); break;
   case WM_LBUTTONDOWN:
      seen(K_LDOWN);
      if ((GetMessageExtraInfo() & 0xFFFFFF00) == 0xFF515700)
         promoted = 1;
      return 0;
   case WM_LBUTTONUP: seen(K_LUP); return 0;
   case WM_RBUTTONDOWN: seen(K_RDOWN); return 0;
   case WM_MOUSEMOVE: seen(K_MOVE); return 0;
   case WM_GESTURE: seen(K_GESTURE); break;
   case WM_TOUCH: seen(K_TOUCH); CloseTouchInputHandle((HTOUCHINPUT)l); return 0;
   case WM_GESTURENOTIFY:
      if (wkind == W_NOGESTURE) {
         GESTURECONFIG gc = {0, 0, GC_ALLGESTURES};
         SetGestureConfig(h, 0, 1, &gc, sizeof(gc));
      }
      break;
   case WM_TABLET_QUERYSYSTEMGESTURESTATUS:
      if (wkind == W_NOHOLD)
         return SB_TABLET_FLAGS;
      break;
   case WM_ERASEBKGND: {
      RECT r;
      GetClientRect(h, &r);
      HBRUSH b = CreateSolidBrush(RGB(20, 60, 120));
      FillRect((HDC)w, &r, b);
      DeleteObject(b);
      return 1;
   }
   case WM_DESTROY: PostQuitMessage(0); return 0;
   }
   return DefWindowProcW(h, m, w, l);
}

static DWORD WINAPI window_thread(void *ready)
{
   WNDCLASSW wc = {};
   wc.lpfnWndProc = wndproc;
   wc.hInstance = GetModuleHandleW(NULL);
   wc.lpszClassName = L"sbtouchlat";
   wc.hCursor = LoadCursor(NULL, IDC_ARROW);
   RegisterClassW(&wc);
   int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
   hwnd = CreateWindowExW(WS_EX_TOPMOST, L"sbtouchlat", L"sbtouchlat", WS_POPUP | WS_VISIBLE, sw / 8, sh / 8, sw * 3 / 4,
                          sh * 3 / 4, NULL, NULL, wc.hInstance, NULL);
   if (wkind == W_NOHOLD)
      SetPropW(hwnd, L"MicrosoftTabletPenServiceProperty", (HANDLE)(ULONG_PTR)SB_TABLET_FLAGS);
   if (wkind == W_TOUCHWIN)
      RegisterTouchWindow(hwnd, TWF_WANTPALM);
   SetForegroundWindow(hwnd);
   SetEvent((HANDLE)ready);
   MSG msg;
   while (GetMessageW(&msg, NULL, 0, 0) > 0) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
   }
   UnregisterClassW(L"sbtouchlat", wc.hInstance);
   return 0;
}

static BOOL inject(int x, int y, DWORD flags)
{
   POINTER_TOUCH_INFO c = {};
   c.pointerInfo.pointerType = PT_TOUCH;
   c.pointerInfo.pointerId = 0;
   c.pointerInfo.ptPixelLocation.x = x;
   c.pointerInfo.ptPixelLocation.y = y;
   c.pointerInfo.pointerFlags = flags;
   c.touchFlags = TOUCH_FLAG_NONE;
   c.touchMask = TOUCH_MASK_CONTACTAREA | TOUCH_MASK_ORIENTATION | TOUCH_MASK_PRESSURE;
   c.orientation = 90;
   c.pressure = 32000;
   c.rcContact.left = x - 2;
   c.rcContact.right = x + 2;
   c.rcContact.top = y - 2;
   c.rcContact.bottom = y + 2;
   return InjectTouchInput(1, &c);
}

// one touch: down at (x, y), held hold_ms (updates every 8 ms, moved by dx over the hold), up. Returns 0 on failure.
static int trial(const char *pattern, int n, int x, int y, int hold_ms, int dx)
{
   for (int k = 0; k < K_N; k++)
      first_us[k] = -1;
   promoted = 0;
   LONGLONG start = qpc();
   t0 = start;
   if (!inject(x, y, POINTER_FLAG_DOWN | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT)) {
      fprintf(lf, "TOUCHLAT %s %s #%d: InjectTouchInput(down) failed, error %lu\n", wname[wkind], pattern, n, GetLastError());
      t0 = 0;
      return 0;
   }
   LONG down_us = (LONG)((qpc() - start) * 1000000 / qf.QuadPart);
   for (;;) {
      Sleep(8);
      LONG ms = (LONG)((qpc() - start) * 1000 / qf.QuadPart);
      if (ms >= hold_ms)
         break;
      inject(x + dx * ms / hold_ms, y, POINTER_FLAG_UPDATE | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT);
   }
   LONG up_us = (LONG)((qpc() - start) * 1000000 / qf.QuadPart);
   if (!inject(x + dx, y, POINTER_FLAG_UP))
      fprintf(lf, "TOUCHLAT %s %s #%d: InjectTouchInput(up) failed, error %lu\n", wname[wkind], pattern, n, GetLastError());
   Sleep(900);      // late messages (a click sent after the release, a press-and-hold right click)
   fprintf(lf, "TOUCHLAT %s %s #%d: inject down took %.1f ms, release at %.1f ms |", wname[wkind], pattern, n,
           down_us / 1000.0, up_us / 1000.0);
   for (int k = 0; k < K_N; k++)
      if (first_us[k] >= 0)
         fprintf(lf, " %s +%.1f", kname[k], first_us[k] / 1000.0);
   LONG ld = first_us[K_LDOWN];
   fprintf(lf, " | click %s%s\n",
           ld < 0 ? "NEVER" : ld >= up_us - 2000 ? "AT OR AFTER THE RELEASE" : ld < 30000 ? "at once" : "during the hold",
           promoted ? " (promoted from touch)" : "");
   fflush(lf);
   t0 = 0;
   return 1;
}

int main(int argc, char **argv)
{
   lf = argc > 1 ? fopen(argv[1], "a") : stdout;
   if (!lf)
      return 2;
   SetProcessDPIAware();
   QueryPerformanceFrequency(&qf);
   int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
   fprintf(lf, "TOUCHLAT start: screen %dx%d, digitizer status 0x%x, max touches %d\n", sw, sh,
           GetSystemMetrics(SM_DIGITIZER), GetSystemMetrics(SM_MAXIMUMTOUCHES));
   if (!InitializeTouchInjection(2, TOUCH_FEEDBACK_NONE)) {
      fprintf(lf, "TOUCHLAT InitializeTouchInjection failed, error %lu\n", GetLastError());
      fclose(lf);
      return 1;
   }
   for (wkind = 0; wkind < W_N; wkind++) {
      HANDLE ready = CreateEventW(NULL, TRUE, FALSE, NULL);
      HANDLE th = CreateThread(NULL, 0, window_thread, ready, 0, NULL);
      WaitForSingleObject(ready, 5000);
      Sleep(700);
      int x = sw / 2 - 100, y = sh / 2, ok = 1;
      for (int i = 0; ok && i < 5; i++)
         ok = trial("tap60", i, x, y + 10 * i, 60, 0);
      for (int i = 0; ok && i < 3; i++)
         ok = trial("hold400", i, x, y - 10 * i, 400, 0);
      for (int i = 0; ok && i < 2; i++)
         ok = trial("hold1500", i, x + 40, y, 1500, 0);
      for (int i = 0; ok && i < 3; i++)
         ok = trial("drag300", i, x - 60, y + 40, 300, 150);
      PostMessageW(hwnd, WM_CLOSE, 0, 0);
      WaitForSingleObject(th, 5000);
      CloseHandle(th);
      CloseHandle(ready);
      Sleep(300);
   }
   fprintf(lf, "TOUCHLAT end\n");
   fclose(lf);
   return 0;
}
