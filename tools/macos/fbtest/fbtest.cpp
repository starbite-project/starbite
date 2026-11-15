// Host test of SweetFramebuffer.cpp's present path (0457): the kernel's own code (fb_cut.inc, cut by extract.py)
// against random frames, pointer moves, pointer hides and keyboard shows/hides. After every present the panel
// must equal an independent rendering: the frame (nearest scaling) with the pointer blended over it.
//   fbtest PANEL_W PANEL_H CANVAS_W CANVAS_H STEPS SEED
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <map>
#include <thread>
#define ARM64_BOARD_CONFIG_STARLTE 1
#define SWEET_KBD_WANT_ALPHA 1
#include "sweet_kbd.h"
struct sweet_kbd_shared sweet_kbd;
#define NSEC_PER_SEC 1000000000ULL
#define NSEC_PER_MSEC 1000000ULL
static int quiet = 1;
#define IOLog(...) do { if (!quiet) printf(__VA_ARGS__); } while (0)
static uint64_t fake_ns = 1;
static uint64_t mach_absolute_time() { return fake_ns; }
static void absolutetime_to_nanoseconds(uint64_t a, uint64_t *o) { *o = a; }
static bool PE_parse_boot_argn(const char *, void *, int) { return false; }
static void *IOMallocData(size_t n) { return malloc(n); }
static void IOFreeData(void *p, size_t) { free(p); }
int sweet_console_muted;
uint64_t sweet_fb_first_present;
#define SWEET_FB_SWAP_REC_MAX 0x600
#define SWEET_FB_LAYERS 4
#define OSSafeReleaseNULL(x) do { if (x) { (x)->release(); (x) = NULL; } } while (0)

struct Mem { size_t len; size_t getLength() { return len; } };
struct SweetSurface {
	uint32_t id, width, height, pixelFormat, bytesPerRow, bytesPerElement, elementWidth, elementHeight, offset;
	Mem mem, *memory;
	std::vector<uint8_t> data;
	uintptr_t kernelAddress() { return (uintptr_t)data.data(); }
	void release() {}
};
static std::map<uint32_t, SweetSurface *> surfaces;
static SweetSurface *sweet_surface_copy(uint32_t id) { auto it = surfaces.find(id); return it == surfaces.end() ? NULL : it->second; }
static SweetSurface *mksurface(uint32_t id, uint32_t w, uint32_t h, uint32_t pad)
{
	SweetSurface *s = new SweetSurface();
	s->id = id; s->width = w; s->height = h; s->pixelFormat = 0x42475241; s->bytesPerRow = w * 4 + pad;
	s->bytesPerElement = 4; s->elementWidth = s->elementHeight = 1; s->offset = 0;
	s->data.assign((size_t)s->bytesPerRow * h, 0);
	s->mem.len = s->data.size(); s->memory = &s->mem;
	surfaces[id] = s;
	return s;
}

struct AppleCLCD {
	uint32_t fWidth, fHeight, fRowBytes, fLogicalW, fLogicalH;
	int32_t fOffX, fOffY;
	bool fScaled, fGpu, fConsolePaused;
	uintptr_t fPixels;
	SweetSurface *fSurface;
	uint64_t present(const uint8_t *rec);
	void sweetSaveUnderRect(int32_t dx, int32_t dy, int32_t w, int32_t h);
	void sweetSaveUnder(bool save);
	uint64_t scaledCopy(uintptr_t src, uint32_t srcRowBytes, int32_t dx, int32_t dy, int32_t w, int32_t h);
	void kbdService(void);
	void kbdDrawKey(uint32_t page, uint32_t i);
	void kbdRepaint(int32_t py0, int32_t py1);
	void pointerRect(int32_t dx, int32_t dy, int32_t w, int32_t h, int32_t *x0, int32_t *y0, int32_t *x1, int32_t *y1);
	void pointerCompose(int32_t x0, int32_t y0, int32_t x1, int32_t y1);
};
// the kernel's helper threads: four bands at once
static void sweet_fb_run_bands(void (*fn)(void *, uint32_t, uint32_t), void *ctx)
{
	std::thread t[3];
	for (int i = 0; i < 3; i++) t[i] = std::thread(fn, ctx, i + 1, 4);
	fn(ctx, 0, 4);
	for (int i = 0; i < 3; i++) t[i].join();
}

#include "fb_cut.inc"

static uint32_t rnd_state;
static uint32_t rnd() { rnd_state = rnd_state * 1664525u + 1013904223u; return rnd_state >> 8; }
static void put32(uint8_t *p, uint32_t off, uint32_t v) { memcpy(p + off, &v, 4); }

int main(int argc, char **argv)
{
	if (argc < 7) { fprintf(stderr, "usage\n"); return 2; }
	uint32_t PW = atoi(argv[1]), PH = atoi(argv[2]), LW = atoi(argv[3]), LH = atoi(argv[4]);
	int steps = atoi(argv[5]); rnd_state = atoi(argv[6]);
	const uint32_t PTR = argc > 7 ? atoi(argv[7]) : 128;
	AppleCLCD fb{};
	fb.fWidth = PW; fb.fHeight = PH; fb.fRowBytes = PW * 4 + 64; fb.fLogicalW = LW; fb.fLogicalH = LH;
	fb.fScaled = true; fb.fConsolePaused = true;
	std::vector<uint8_t> panel((size_t)fb.fRowBytes * PH, 0x55);
	fb.fPixels = (uintptr_t)panel.data();
	sweet_kbd.enabled = 1; sweet_kbd.panel_w = PW; sweet_kbd.panel_h = PH;
	SweetSurface *fr[2] = { mksurface(3, LW, LH, 128), mksurface(4, LW, LH, 128) };
	SweetSurface *ps[2] = { mksurface(6, PTR, PTR, 0), mksurface(7, PTR, PTR, 0) };
	for (int k = 0; k < 2; k++) {
		for (uint32_t y = 0; y < PTR; y++) {
			for (uint32_t x = 0; x < PTR; x++) {
				uint32_t a = (x + y + k) % 5 == 0 ? 0 : ((x * y + k) % 7 == 0 ? 255 : (rnd() & 0xff));
				uint32_t c = rnd() & 0xff;
				c = c * a / 255;        // premultiplied
				uint32_t v = (a << 24) | (c << 16) | ((c / 2) << 8) | (c / 3);
				memcpy(&ps[k]->data[(size_t)y * ps[k]->bytesPerRow + x * 4], &v, 4);
			}
		}
	}
	sweet_fb_rec_size = 0x514;
	const uint32_t sh = 0x48;
	int cur = 0;                    // which frame surface was shown last
	bool shown = false;             // a frame has been presented
	bool ptrOn = false; uint32_t ptrId = 0; int32_t pdx = 0, pdy = 0;
	// geometry as the driver's
	int32_t outW = (int32_t)((int64_t)LW * PH / LH), outH = PH;
	if (outW > (int32_t)PW) { outW = PW; outH = (int32_t)((int64_t)LH * PW / LW); }
	int32_t barX = ((int32_t)PW - outW) / 2, barY = ((int32_t)PH - outH) / 2;
	uint32_t swapid = 0; long checks = 0;
	for (int step = 0; step < steps; step++) {
		uint8_t rec[SWEET_FB_SWAP_REC_MAX]; memset(rec, 0, sizeof rec);
		uint32_t mask = 0;
		uint32_t op = step == 0 ? 0 : rnd() % 100;
		bool frame = op < 35 || (op >= 85 && op < 92), ptr = op >= 35 && op < 92, hide = op >= 92 && op < 95, kbd = op >= 95;
		if (!shown) { frame = true; ptr = hide = kbd = false; }
		if (kbd) {
			sweet_kbd.visible = !sweet_kbd.visible;
			fb.kbdService();
		} else {
			if (frame) {
				int nxt = cur ^ (shown ? 1 : 0);
				if (shown) fr[nxt]->data = fr[cur]->data;
				uint32_t kind = shown ? rnd() % 6 : 5;
				if (kind == 5) {
					for (size_t i = 0; i + 4 <= fr[nxt]->data.size(); i += 4) { uint32_t v = 0xff000000u | rnd(); memcpy(&fr[nxt]->data[i], &v, 4); }
				} else if (kind != 4) {          // (4: an unchanged frame)
					int n = 1 + rnd() % 4;
					for (int k = 0; k < n; k++) {
						// a rectangle, often around the pointer
						int32_t rw = 1 + rnd() % (LW / 2), rh = 1 + rnd() % 60;
						int32_t rx = rnd() % LW, ry = rnd() % LH;
						if (ptrOn && rnd() % 2) { rx = pdx - 20 + (int32_t)(rnd() % 60); ry = pdy - 20 + (int32_t)(rnd() % 60); }
						for (int32_t y = ry < 0 ? 0 : ry; y < ry + rh && y < (int32_t)LH; y++)
							for (int32_t x = rx < 0 ? 0 : rx; x < rx + rw && x < (int32_t)LW; x++) {
								uint32_t v = 0xff000000u | rnd();
								memcpy(&fr[nxt]->data[(size_t)y * fr[nxt]->bytesPerRow + x * 4], &v, 4);
							}
					}
				}
				cur = nxt; shown = true;
				mask |= 1;
				put32(rec, 0x54 + sh, fr[cur]->id);
				put32(rec, 0x64 + sh + 8, LW); put32(rec, 0x64 + sh + 12, LH);
				put32(rec, 0xc4 + sh + 8, LW); put32(rec, 0xc4 + sh + 12, LH);
			}
			if (ptr) {
				ptrOn = true; ptrId = ps[rnd() % 8 == 0]->id;
				pdx = rnd() % (LW + 10); pdy = rnd() % (LH + 10);      // may hang over the right/bottom edge
				if (rnd() % 3 == 0 && step > 2) { pdx += (int32_t)(rnd() % 9) - 4; pdy += (int32_t)(rnd() % 9) - 4; }
				if (pdx < 0) pdx = 0;
				if (pdy < 0) pdy = 0;
				if (pdx >= (int32_t)LW) pdx = LW - 1;
				if (pdy >= (int32_t)LH) pdy = LH - 1;
				mask |= 2;
				put32(rec, 0x58 + sh, ptrId);
				put32(rec, 0x64 + sh + 16 + 8, PTR); put32(rec, 0x64 + sh + 16 + 12, PTR);
				put32(rec, 0xc4 + sh + 16, pdx); put32(rec, 0xc4 + sh + 16 + 4, pdy);
				put32(rec, 0xc4 + sh + 16 + 8, PTR); put32(rec, 0xc4 + sh + 16 + 12, PTR);
			}
			if (hide) {
				ptrOn = false;
				mask |= 2;      // layer 1 updated to no surface
			}
			put32(rec, 0x50 + sh, ++swapid);
			put32(rec, 0x104 + sh, mask);
			fb.present(rec);
			fake_ns += 16000000;
		}
		// the reference picture
		int32_t kbdTop = sweet_kbd.visible ? (int32_t)PH - sweet_kbd_height(PW) : 0x7fffffff;
		int32_t pw = PTR, ph = PTR;
		if (pw > (int32_t)LW - pdx) pw = LW - pdx;
		if (ph > (int32_t)LH - pdy) ph = LH - pdy;
		int32_t x0 = barX + (int32_t)((int64_t)pdx * outW / LW), x1 = barX + (int32_t)(((int64_t)(pdx + pw) * outW + LW - 1) / LW);
		int32_t y0 = barY + (int32_t)((int64_t)pdy * outH / LH), y1 = barY + (int32_t)(((int64_t)(pdy + ph) * outH + LH - 1) / LH);
		if (x1 > barX + outW) x1 = barX + outW;
		if (y1 > barY + outH) y1 = barY + outH;
		SweetSurface *p = ptrOn ? surfaces[ptrId] : NULL;
		for (int32_t py = barY; py < barY + outH && py < kbdTop; py++) {
			int32_t r = (int32_t)((int64_t)(py - barY) * LH / outH);
			for (int32_t px = barX; px < barX + outW; px++) {
				int32_t c = (int32_t)((int64_t)(px - barX) * LW / outW);
				uint32_t want; memcpy(&want, &fr[cur]->data[(size_t)r * fr[cur]->bytesPerRow + c * 4], 4);
				if (p && px >= x0 && px < x1 && py >= y0 && py < y1) {
					int32_t pc = c - pdx, pr = r - pdy;
					pc = pc < 0 ? 0 : (pc >= pw ? pw - 1 : pc);
					pr = pr < 0 ? 0 : (pr >= ph ? ph - 1 : pr);
					uint32_t v; memcpy(&v, &p->data[(size_t)pr * p->bytesPerRow + pc * 4], 4);
					uint32_t a = v >> 24;
					if (a == 255) want = v;
					else if (a) {
						uint32_t k = 255 - a, res = 0xff000000u;
						for (uint32_t s2 = 0; s2 < 24; s2 += 8) {
							uint32_t ch = ((v >> s2) & 0xff) + (((want >> s2) & 0xff) * k + 127) / 255;
							res |= (ch > 255 ? 255 : ch) << s2;
						}
						want = res;
					}
				}
				uint32_t got; memcpy(&got, &panel[(size_t)py * fb.fRowBytes + px * 4], 4);
				if (got != want) {
					printf("FAIL step %d op %u (frame %d ptr %d hide %d kbd %d): panel %d,%d got %08x want %08x; pointer %s at %d,%d rect %d,%d-%d,%d kbdTop %d\n",
					    step, op, frame, ptr, hide, kbd, px, py, got, want, ptrOn ? "on" : "off", pdx, pdy, x0, y0, x1, y1, kbdTop);
					return 1;
				}
				checks++;
			}
		}
	}
	printf("ok: %ux%u canvas on %ux%u, %d steps, %ld pixels checked; rows same %u new %u\n", LW, LH, PW, PH, steps, checks,
	    sweet_fb_rows_same, sweet_fb_rows_new);
	return 0;
}
