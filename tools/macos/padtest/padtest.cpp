// Host test of the XNU touch driver's trackpad gestures (pending patch 0460) and the keyboard summon swipe:
// the driver's own code (pad_cut.inc, cut by extract.py) driven with touch samples; the HID events it sends
// are checked.   padtest SWEETPAD   (0 off, 1 three-finger swipes, 2 scrolling in points, 3 both)
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <string>
#include "sweet_kbd.h"
struct sweet_kbd_shared sweet_kbd;
typedef uint32_t UInt32; typedef int32_t SInt32; typedef uint64_t UInt64; typedef int32_t IOFixed; typedef uint32_t IOOptionBits;
typedef uint16_t IOHIDEventPhaseBits;
enum { kIOHIDEventOptionIsPixelUnits = 1 << 2, kIOHIDEventPhaseBegan = 1, kIOHIDEventPhaseChanged = 2, kIOHIDEventPhaseEnded = 4,
       kIOHIDEventEventOptionPhaseShift = 24, kIOHIDEventScrollMomentumContinue = 1, kIOHIDEventScrollMomentumStart = 2,
       kIOHIDEventScrollMomentumEnd = 4, kIOHIDEventScrollMomentumShift = 28 };
#define SWEET_TOUCH_FINGERS 10
#define NSEC_PER_MSEC 1000000ULL
static int verbose;
#define IOLog(...) do { if (verbose) printf(__VA_ARGS__); } while (0)
static uint64_t now_ns = 1000000000;
static uint64_t mach_absolute_time() { return now_ns; }
static void absolutetime_to_nanoseconds(uint64_t a, uint64_t *o) { *o = a; }
static int cfg_gestures;
#define POINTS (cfg_gestures & 2)
#define SWIPES (cfg_gestures & 1)
static bool PE_parse_boot_argn(const char *name, void *v, int) { if (!strcmp(name, "sweetpad")) { *(int *)v = cfg_gestures; return true; } return false; }
struct IOLock; static void IOLockLock(IOLock *) {} static void IOLockUnlock(IOLock *) {}
static void sweet_canvas(int *w, int *h) { *w = 1440; *h = 2960; }

struct Ev { char kind; int64_t x, y; uint32_t a, b; };  // 'p' pointer (x,y,buttons,old), 's' line scroll, 'f' fixed scroll (x,y 16.16, options), 'k' key (usage, down)
static std::vector<Ev> out;
static int phase_events;
struct IOHIDEvent {
	Ev e;
	static IOHIDEvent *mk(char k, int64_t x, int64_t y, uint32_t a, uint32_t b) { IOHIDEvent *v = new IOHIDEvent(); v->e = { k, x, y, a, b }; return v; }
	static IOHIDEvent *relativePointerEvent(UInt64, SInt32 dx, SInt32 dy, SInt32, UInt32 buttons, UInt32 old) { return mk('p', dx, dy, buttons, old); }
	static IOHIDEvent *scrollEvent(UInt64, SInt32 x, SInt32 y, SInt32, IOOptionBits o = 0) { return mk('s', x, y, o, 0); }
	static IOHIDEvent *scrollEventWithFixed(UInt64, IOFixed x, IOFixed y, IOFixed, IOOptionBits o = 0) { return mk('f', x, y, o, 0); }
	void release() { delete this; }
};
struct SweetHIDKeyboard { void key(UInt32 usage, bool down) { out.push_back({ 'k', 0, 0, usage, down }); } };
static SweetHIDKeyboard kb; static SweetHIDKeyboard *gSweetKeyboard = &kb;
struct Timer { int ms = 0; void setTimeoutMS(int m) { ms = m; } };
struct SweetHIDTouch {
	IOLock *fLock = nullptr; int fLogicalW = 1440, fPanelW = 1440; Timer steer, *fSteer = &steer;
	// 0465: whatever the mode, no scroll event may carry a gesture phase or a momentum flag (S6i: AppKit aborts the app)
	void dispatchEvent(IOHIDEvent *e) { if ((e->e.kind == 'f' || e->e.kind == 's') && (e->e.a & 0xff000300u)) phase_events++; out.push_back(e->e); }
	void padTouch(int finger, bool down, int x, int y);
	void padTick(void);
};
#include "pad_cut.inc"

static SweetHIDTouch T;
static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __func__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
static void ms(int n) { now_ns += (uint64_t)n * NSEC_PER_MSEC; }
static void touch(int f, bool d, int x, int y) { T.padTouch(f, d, x, y); }
// run the timer as the driver would: while it is armed
static int ticks(int limit_ms) {
	int n = 0, t = 0;
	while (T.steer.ms > 0 && t < limit_ms) { int w = T.steer.ms; T.steer.ms = 0; ms(w); t += w; T.padTick(); n++; }
	return n;
}
static int count(char k) { int n = 0; for (auto &e : out) n += e.kind == k; return n; }
static bool has_opt(uint32_t bit) { for (auto &e : out) if (e.kind == 'f' && (e.a & bit)) return true; return false; }
static const uint32_t BEGAN = 1u << 24, CHANGED = 2u << 24, ENDED = 4u << 24, MSTART = 2u << 28, MCONT = 1u << 28, MEND = 4u << 28;
static void reset() { out.clear(); T.steer.ms = 0; ms(2000); }

static void scroll2(int px, int step_ms, int pause_ms) {
	touch(0, true, 500, 1000); ms(10); touch(1, true, 800, 1000);
	for (int i = 1; i <= px / 20; i++) { ms(step_ms); touch(0, true, 500, 1000 + 20 * i); touch(1, true, 800, 1000 + 20 * i); }
	ms(pause_ms);
	touch(1, false, 800, 1000 + px); ms(5); touch(0, false, 500, 1000 + px);
}

static void test_scroll() {
	reset(); scroll2(600, 10, 5);
	if (!POINTS) {
		CHECK(count('s') > 10 && count('f') == 0, "line scrolling: %d line events, %d fixed", count('s'), count('f'));
		int64_t y = 0; for (auto &e : out) if (e.kind == 's') y += e.y;
		CHECK(y == -(600 / 24), "lines %lld", (long long)y);
		CHECK(count('p') == 0, "pointer events %d", count('p'));
		return;
	}
	CHECK(count('s') == 0 && count('p') == 0, "line %d pointer %d", count('s'), count('p'));
	int64_t y = 0, x = 0;
	for (size_t i = 0; i < out.size(); i++) { auto &e = out[i]; if (e.kind != 'f') continue;
		y += e.y; x += e.x;
		// 0465: pixel units and nothing else: a gesture phase or momentum flag makes AppKit abort the app (S6i)
		CHECK(e.a == 4, "scroll event options %#x (must be pixel units only)", e.a); }
	// 600 panel px = 600 canvas px = 300 points, fingers down => negative y
	CHECK(y == -300 * 65536 && x == 0, "scrolled %lld/65536 points, x %lld", (long long)(y / 65536), (long long)x);
	CHECK(T.steer.ms == 16, "momentum timer %d", T.steer.ms);
	size_t before = out.size(); int n = ticks(5000);
	CHECK(n > 20 && n < 200, "momentum ticks %d", n);
	int64_t my = 0; bool plain = true;
	for (size_t i = before; i < out.size(); i++) { auto &e = out[i]; if (e.kind != 'f' || e.a != 4) { plain = false; continue; }
		my += e.y; }
	CHECK(plain, "momentum events are not plain pixel scroll events");
	// 2 points/ms at the lift: the coast must go on in the same direction, between 200 and 1500 points
	CHECK(my < -200 * 65536 && my > -1500 * 65536, "momentum distance %lld points", (long long)(my / 65536));
	printf("  scroll: 300 points by hand, then %lld points of momentum in %d ticks\n", (long long)(-my / 65536), n);
	// slow end: no momentum
	reset(); scroll2(600, 10, 200);
	// (0468: the first finger's long-press timer may still be armed; it must do nothing: the next check)
	CHECK(T.steer.ms == 0 || T.steer.ms >= 500, "slow lift: momentum timer %d", T.steer.ms);
	{ size_t b2 = out.size(); ticks(1000); CHECK(out.size() == b2, "slow lift: %zu events after the lift", out.size() - b2); }
	// a finger stops the momentum
	reset(); scroll2(600, 10, 5); T.steer.ms = 0; ms(16); T.padTick(); ms(16); T.padTick();
	out.clear(); touch(0, true, 700, 700);
	CHECK(out.size() == 0, "touch during momentum: %zu events", out.size());
	T.steer.ms = 0; ms(16); T.padTick();
	CHECK(count('f') == 0, "momentum went on after the touch");
	ms(400); touch(0, false, 700, 700); ticks(1000);
}

static void test_two_finger_tap() {
	reset(); touch(0, true, 500, 1000); ms(8); touch(1, true, 800, 1010); ms(60);
	touch(0, true, 503, 1002); ms(10); touch(1, false, 800, 1010); ms(8); touch(0, false, 503, 1002);
	ticks(1000);
	int down = 0, up = 0; for (auto &e : out) if (e.kind == 'p') { down += e.a == 2 && e.b == 0; up += e.a == 0 && e.b == 2; }
	CHECK(down == 1 && up == 1 && count('f') == 0 && count('s') == 0, "right click down %d up %d, scroll events %d", down, up, count('f') + count('s'));
}

static void test_one_finger() {
	reset(); touch(0, true, 400, 400);
	for (int i = 1; i <= 20; i++) { ms(10); touch(0, true, 400 + 10 * i, 400 + 5 * i); }
	ms(300); touch(0, false, 600, 500); ticks(1000);
	int64_t x = 0, y = 0; for (auto &e : out) if (e.kind == 'p') { x += e.x; y += e.y; CHECK(e.a == 0, "a button during a move"); }
	CHECK(x == 200 && y == 100 && count('f') == 0 && count('k') == 0, "pointer moved %lld,%lld", (long long)x, (long long)y);
	reset(); touch(0, true, 400, 400); ms(60); touch(0, false, 400, 400); ticks(1000);
	int down = 0, up = 0; for (auto &e : out) if (e.kind == 'p') { down += e.a == 1 && e.b == 0; up += e.a == 0 && e.b == 1; }
	CHECK(down == 1 && up == 1, "tap: click down %d up %d", down, up);
}

static void test_swipe3(int dx, int dy, uint32_t key) {
	reset(); touch(0, true, 400, 1500); ms(5); touch(1, true, 700, 1500); ms(5); touch(2, true, 1000, 1500);
	for (int i = 1; i <= 15; i++) { ms(10); for (int f = 0; f < 3; f++) touch(f, true, 400 + 300 * f + dx * i / 15, 1500 + dy * i / 15); }
	ms(10); for (int f = 2; f >= 0; f--) { touch(f, false, 400 + 300 * f + dx, 1500 + dy); ms(4); }
	ticks(1000);
	if (!SWIPES) { CHECK(count('k') == 0, "keys without the swipes"); return; }
	std::vector<Ev> k; for (auto &e : out) if (e.kind == 'k') k.push_back(e);
	CHECK(k.size() == 4 && k[0].a == 0xe0 && k[0].b == 1 && k[1].a == key && k[1].b == 1 && k[2].a == key && k[2].b == 0 &&
	    k[3].a == 0xe0 && k[3].b == 0, "swipe %d,%d: %zu key events, key 0x%x", dx, dy, k.size(), k.size() > 1 ? k[1].a : 0);
	CHECK(count('p') == 0 && count('f') == 0 && count('s') == 0, "swipe also sent pointer %d / scroll %d events", count('p'), count('f') + count('s'));
}

static void test_leftover_finger() {
	// scroll, lift the primary finger first, move the other: no pointer jump (and with gestures, no movement)
	reset(); touch(0, true, 500, 1000); ms(10); touch(1, true, 900, 1400);
	for (int i = 1; i <= 10; i++) { ms(10); touch(0, true, 500, 1000 + 20 * i); touch(1, true, 900, 1400 + 20 * i); }
	ms(200); touch(0, false, 500, 1200); out.clear();
	for (int i = 1; i <= 5; i++) { ms(10); touch(1, true, 900 + 4 * i, 1600); }
	int64_t x = 0, y = 0; for (auto &e : out) if (e.kind == 'p') { x += e.x; y += e.y; }
	if (cfg_gestures) CHECK(count('p') == 0, "leftover finger moved the pointer %lld,%lld", (long long)x, (long long)y);
	else CHECK(x == 20 - 4 && y == 0, "leftover finger: pointer %lld,%lld (a jump?)", (long long)x, (long long)y);   // its first sample only sets the start
	ms(10); touch(1, false, 920, 1600); ticks(1000);
	// the next single touch moves the pointer normally
	reset(); touch(0, true, 300, 300);
	for (int i = 1; i <= 3; i++) { ms(10); touch(0, true, 300 + 10 * i, 300); }     // (steps under 20 px: no acceleration)
	ms(300); touch(0, false, 330, 300); ticks(1000);
	x = 0; for (auto &e : out) if (e.kind == 'p') x += e.x;
	CHECK(x == 30, "after the gesture a finger moves the pointer by %lld", (long long)x);
}

static void test_kbd_swipe() {
	int rx = 0, ry = 0;
	sweet_kbd.enabled = 1; sweet_kbd.panel_w = 1440; sweet_kbd.panel_h = 2960; sweet_kbd.visible = 0;
	memset(sweet_kbd_fingers, 0, sizeof(sweet_kbd_fingers));
	// S5y: lands at 876,2801 and settles 42 px lower 24 ms later, then swipes up
	CHECK(sweet_kbd_touch(0, true, 876, 2801, &rx, &ry) == 1, "start not taken");
	ms(24); CHECK(sweet_kbd_touch(0, true, 877, 2843, &rx, &ry) == 1, "the settling finger was handed to the trackpad");
	for (int i = 1; i <= 10; i++) { ms(15); sweet_kbd_touch(0, true, 880, 2843 - 30 * i, &rx, &ry); }
	CHECK(sweet_kbd.visible == 1, "the keyboard did not show");
	CHECK(sweet_kbd_touch(0, false, 880, 2543, &rx, &ry) == 1, "lift");
	// a tap in the bottom zone goes to the trackpad from its first point
	sweet_kbd.visible = 0; ms(500);
	CHECK(sweet_kbd_touch(0, true, 700, 2900, &rx, &ry) == 1, "tap start");
	ms(60); CHECK(sweet_kbd_touch(0, false, 702, 2901, &rx, &ry) == 2 && rx == 700 && ry == 2900, "tap not handed over");
	CHECK(sweet_kbd_touch(0, true, 700, 2900, &rx, &ry) == 0 && sweet_kbd_touch(0, false, 702, 2901, &rx, &ry) == 0, "replay not passed through");
	// a sideways move in the zone is the trackpad's
	CHECK(sweet_kbd_touch(0, true, 300, 2900, &rx, &ry) == 1, "side start");
	ms(30); CHECK(sweet_kbd_touch(0, true, 500, 2890, &rx, &ry) == 2, "sideways not handed over");
	sweet_kbd_touch(0, false, 500, 2890, &rx, &ry);
	// a touch above the zone is never the keyboard's
	CHECK(sweet_kbd_touch(1, true, 700, 2700, &rx, &ry) == 0, "above the zone");
}

// 0468: a resting finger is a right click (the context menu), sent while the finger is still down
static int rights() { int n = 0; for (auto &e : out) n += e.kind == 'p' && e.a == 2 && e.b == 0; return n; }
static int lefts() { int n = 0; for (auto &e : out) n += e.kind == 'p' && e.a == 1 && e.b == 0; return n; }
static void test_long_press() {
	// still finger: one right click after 550 ms, nothing at the lift
	reset(); touch(0, true, 400, 400); ms(300); T.padTick();
	CHECK(rights() == 0, "a right click after 300 ms");
	ticks(1000);
	CHECK(rights() == 1 && lefts() == 0, "long press: right clicks %d, left %d", rights(), lefts());
	ms(200); touch(0, false, 400, 400); ticks(1000);
	CHECK(rights() == 1 && lefts() == 0 && count('p') == 2, "after the lift: right %d left %d pointer events %d", rights(), lefts(), count('p'));
	// a finger that trembles a few pixels still counts
	reset(); touch(0, true, 400, 400); ms(100); touch(0, true, 404, 397); ms(100); touch(0, true, 398, 402); ticks(1000);
	CHECK(rights() == 1, "trembling finger: right clicks %d", rights());
	touch(0, false, 398, 402); ticks(1000);
	// after the menu is up the same finger moves the pointer (no button held)
	reset(); touch(0, true, 400, 400); ticks(1000); out.clear();
	for (int i = 1; i <= 10; i++) { ms(10); touch(0, true, 400, 400 + 10 * i); }
	int64_t y = 0; for (auto &e : out) if (e.kind == 'p') { y += e.y; CHECK(e.a == 0, "a button while moving after the long press"); }
	CHECK(y == 100, "pointer moved %lld after the long press", (long long)y);
	touch(0, false, 400, 500); ticks(1000);
	CHECK(lefts() == 0 && rights() == 0, "a click at the lift after the long press");
	// a finger that moved away and then rests is not a long press
	reset(); touch(0, true, 400, 400);
	for (int i = 1; i <= 10; i++) { ms(10); touch(0, true, 400 + 10 * i, 400); }
	ticks(2000);
	CHECK(rights() == 0, "moved then rested: right clicks %d", rights());
	touch(0, false, 500, 400); ticks(1000);
	// tap, then touch and rest: that is the drag gesture's start, never a right click
	reset(); touch(0, true, 400, 400); ms(60); touch(0, false, 400, 400); ms(100); touch(0, true, 400, 400); ticks(1500);
	CHECK(rights() == 0, "tap then hold: right clicks %d", rights());
	touch(0, false, 400, 400); ticks(1000);
	// a quick tap stays a left click
	reset(); touch(0, true, 400, 400); ms(60); touch(0, false, 400, 400); ticks(1000);
	CHECK(lefts() == 1 && rights() == 0, "tap: left %d right %d", lefts(), rights());
	// two fingers resting: no long press (their tap is the right click, a rest is nothing)
	reset(); touch(0, true, 400, 400); ms(20); touch(1, true, 700, 400); ticks(1500);
	CHECK(rights() == 0, "two fingers resting: right clicks %d", rights());
	touch(1, false, 700, 400); touch(0, false, 400, 400); ticks(1000);
}

// 0468: a four-finger tap shows / hides the on-screen keyboard; four fingers never click or scroll
static void test_four_finger_tap() {
	sweet_kbd.enabled = 1; sweet_kbd.visible = 0;
	reset();
	for (int f = 0; f < 4; f++) { touch(f, true, 300 + 200 * f, 1000); ms(6); }
	ms(80);
	for (int f = 3; f >= 0; f--) { touch(f, false, 300 + 200 * f, 1000); ms(5); }
	ticks(1000);
	CHECK(sweet_kbd.visible == 1 && sweet_kbd.full == 1, "four-finger tap: visible %u", (unsigned)sweet_kbd.visible);
	CHECK(count('p') == 0 && count('f') == 0 && count('s') == 0 && count('k') == 0, "four-finger tap sent %zu events", out.size());
	reset();
	for (int f = 0; f < 4; f++) { touch(f, true, 300 + 200 * f, 1000); ms(6); }
	ms(80);
	for (int f = 0; f < 4; f++) { touch(f, false, 300 + 200 * f, 1000); ms(5); }
	ticks(1000);
	CHECK(sweet_kbd.visible == 0, "second four-finger tap: visible %u", (unsigned)sweet_kbd.visible);
	// four fingers resting for a second: nothing
	reset();
	for (int f = 0; f < 4; f++) { touch(f, true, 300 + 200 * f, 1000); ms(6); }
	ticks(1500); ms(1000);
	for (int f = 0; f < 4; f++) { touch(f, false, 300 + 200 * f, 1000); ms(5); }
	ticks(1000);
	CHECK(sweet_kbd.visible == 0 && out.size() == 0, "four fingers resting: visible %u, %zu events", (unsigned)sweet_kbd.visible, out.size());
}

int main(int argc, char **argv) {
	cfg_gestures = argc > 1 ? atoi(argv[1]) : 1; verbose = argc > 2;
	// 0465: whatever the mode, no event may carry a gesture phase or a momentum flag
	test_one_finger(); test_two_finger_tap(); test_scroll();
	test_swipe3(0, -300, 0x52); test_swipe3(0, 300, 0x51); test_swipe3(-300, 0, 0x4f); test_swipe3(300, 20, 0x50);
	test_leftover_finger(); test_one_finger(); test_long_press(); test_one_finger(); test_two_finger_tap(); test_four_finger_tap(); test_one_finger(); test_kbd_swipe();
	if (phase_events) { printf("FAIL: %d scroll events with phase or momentum bits\n", phase_events); fails++; }
	printf("%s: sweetpad=%d, %d failures\n", fails ? "FAILED" : "ok", cfg_gestures, fails);
	return fails != 0;
}
