/* smptest: shim/starlte/glass_smp.c's phone code (PSCI CPU_ON, worker entry with the MMU on, the
 * claim word, stop with cache write-back and CPU_OFF) on QEMU virt -smp 4 (PSCI over HVC: built
 * with G7_SMP_QEMU). Prints to the PL011 at 0x09000000. */
#include <stdint.h>
#include "glass_int.h"
#include "mmu.h"

#define UART 0x09000000UL
static void putc_(char c) { *(volatile uint32_t *)UART = (uint32_t)c; }
static void puts_(const char *s) { while (*s) putc_(*s++); }
static void putu(uint64_t v) { char d[24]; int k = 0; do { d[k++] = (char)('0' + v % 10); v /= 10; } while (v); while (k) putc_(d[--k]); }
void star_pmsg(const char *s) { puts_("pmsg| "); puts_(s); }
uint64_t hw_ticks(void) { uint64_t v; __asm__ volatile("isb\n mrs %0, cntvct_el0" : "=r"(v)); return v; }
uint64_t hw_freq(void) { uint64_t v; __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v)); return v; }
void hw_wait_us(uint64_t us) { uint64_t t0 = hw_ticks(), n = hw_freq() * us / 1000000U; while (hw_ticks() - t0 < n) ; }
void g7_poll(void) {}

#define ROWS 3000
static volatile uint32_t OUT[ROWS];
static uint32_t salt;
static uint32_t percore[G7_NCORES][64];
static uint64_t rows_by[G7_NCORES];
static void fn(void *ctx, int y0, int y1, int core)
{
    (void)ctx;
    int c = g7_core();
    if (c != core)
        percore[core][63] = 0xbad;              /* the core index the claim gave must be ours */
    for (int y = y0; y < y1; y++) {
        OUT[y] = (uint32_t)y * 2654435761u ^ salt;
        for (volatile int k = 0; k < (y * 7 + (int)salt) % 23; k++)
            ;
        percore[core][y & 31]++;
    }
    rows_by[core] += (uint64_t)(y1 - y0);
}

static int round_(int n)
{
    int bad = 0;
    for (int it = 0; it < n; it++) {
        salt = (uint32_t)it * 40503u + 7u;
        int y0 = it % 50, y1 = ROWS - (it * 13) % 400, grain = 1 + it % 37;
        g7_par(fn, 0, y0, y1, grain);
        for (int y = y0; y < y1; y++)
            if (OUT[y] != ((uint32_t)y * 2654435761u ^ salt))
                bad++;
    }
    return bad;
}

void smptest_main(void);
void smptest_main(void)
{
    puts_("smptest: MMU on\n");
    star_mmu_enable();
    for (int pass = 0; pass < 3; pass++) {
        int n = g7_smp_start(4);
        puts_("smptest: pass ");
        putu((uint64_t)pass);
        puts_(" cores ");
        putu((uint64_t)n);
        puts_("\n");
        int bad = round_(3000);
        puts_("smptest: wrong rows ");
        putu((uint64_t)bad);
        puts_(", rows by core");
        for (int c = 0; c < G7_NCORES; c++) {
            puts_(" ");
            putu(rows_by[c]);
            rows_by[c] = 0;
        }
        int badcore = 0;
        for (int c = 0; c < G7_NCORES; c++)
            badcore |= percore[c][63] == 0xbad;
        puts_(badcore ? ", CORE INDEX MISMATCH" : ", core index ok");
        int off = g7_smp_stop();
        puts_(", stop -> ");
        putu((uint64_t)off);
        puts_(" report ");
        uint64_t r = g7_smp_report();
        char h[17];
        for (int k = 0; k < 16; k++)
            h[k] = "0123456789abcdef"[(r >> (60 - 4 * k)) & 15];
        h[16] = 0;
        puts_(h);
        puts_(bad || off || badcore || n != 4 ? "  FAIL\n" : "  PASS\n");
    }
    puts_("smptest: end\n");
}
