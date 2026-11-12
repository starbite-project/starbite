/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS: media playback through Calliope (abox_media.c). On this phone ABOX's DMAs do not move in "bypass"
 * (abox_pcm.c: four probe runs on 2026-11-12, RDMA0 STATUS 0x80000000 forever, 1004 settings tried), so media goes
 * the way One UI sends it: the AP boots Calliope (abox.c), maps RDMA0's ring at its IOVA in ABOX's SysMMU, sets the
 * mixer routes and asks Calliope over IPC to run RDMA0:
 *
 *   RDMA0 (ring in abox_rmem, IOVA 0x91000000) -> SPUS chain 0 (ASRC off) -> SIFS0 (MIXP) -> UAIF1 SPK (the amps)
 *                                                                                      or UAIF0 SPK (the codec)
 *
 * Sources (facts): Samsung's GPL-2.0 driver (sound/soc/samsung/abox/abox_rdma.c: open / hw_params / prepare /
 * trigger / hw_free / close and their IPC_PCMPLAYBACK messages, the pointer from PCM_PLTDAI_POINTER or RDMA STATUS,
 * the 40 ms disable barrier and the 600-1200 us FIFO flush; abox.c: abox_try_to_asrc_off, the SPUS / UAIF SPK mux
 * texts, abox_flush_mixp, abox_request_dram_on; abox.h: IOVA_RDMA_BUFFER(x), RDMA STATUS 0x30), and One UI's own
 * boot on this phone (build/test-boots/20261108-091233-android-logged/dmesg-final.txt: RDMA0 15360 bytes = 4 x 3840,
 * 48000 / 16 / 2, SIFS0, abox_flush_mixp, uaif_trigger then rdma_trigger; Calliope "CRL0" 64 ms after the CPU).
 *
 * Seen on the phone: nothing yet (the probe below is its first run).
 *
 * Use (single-threaded, no locks; lines start "cal: "):
 *   cal_hal_set(&hal)                     the same HAL abox.c has (abox_hal_set)
 *   cal_map()                             before abox_boot(): RDMA0's ring window in ABOX's SysMMU (abox_map_extra)
 *   abox_boot()                           (abox.c; shared with calls: it says "already runs" when it does)
 *   cal_play_start(uaif, period, periods) routes, UAIF, PCM OPEN / SET_BUFFER / HW_PARAMS / PREPARE, flush, TRIGGER
 *                                         (refused unless abox_boot() put the ring in ABOX's SysMMU tables)
 *   cal_play_buf() / cal_play_pos()       the ring (CPU view, non-cacheable) and where RDMA0 reads now
 *   abox_poll()                           every 10 ms while playing (abox.c: Calliope's messages, pointer included)
 *   cal_play_stop()                       TRIGGER 0, barrier, UAIF off, HW_FREE, CLOSE, routes back
 *   cal_unmap()                           after abox_off() */
#ifndef SBOS_ABOX_MEDIA_H
#define SBOS_ABOX_MEDIA_H
#include "abox.h"

#define CAL_RATE         48000U
#define CAL_FRAME_BYTES  4U                 /* 2 x S16 */
#define CAL_BYTES_PER_S  (CAL_RATE * CAL_FRAME_BYTES)   /* 192000 */
#define CAL_ID           0U                 /* RDMA0 */
#define CAL_IOVA         0x91000000U        /* ABOX_IOVA_RDMA(0) */
/* the ring's window in abox_rmem: the same 128 KiB abox_pcm.c uses for its playback ring (APCM_RM_PLAY; never both:
 * apcm refuses while Calliope owns ABOX), in the free part of abox.c's page-table section. The ring is at most 64 KiB;
 * the rest stays zero, so a DMA that reads past the ring's end reads silence. */
#define CAL_RM_PLAY      0x1580000U
#define CAL_RM_WINDOW    0x20000U
#define CAL_RING_MAX     0x10000U
/* One UI's RDMA0 geometry on this phone (the probe), and snd.c's (the engine) */
#define CAL_ONEUI_PERIOD  3840U
#define CAL_ONEUI_PERIODS 4U
#define CAL_SND_PERIOD    2048U
#define CAL_SND_PERIODS   32U

#define CAL_UAIF0        0                  /* the codec */
#define CAL_UAIF1        1                  /* the amplifiers */
#define CAL_NOPOS        0xFFFFFFFFU

int cal_hal_set(const struct abox_hal *h);  /* 0, or ABOX_ESTATE when a required hook is missing */
int cal_map(void);                          /* 0 or < 0 (abox_map_extra) */
void cal_unmap(void);                       /* counted: the window goes when its last user unmaps */
void cal_sync(void);                        /* after CPU stores into the ring */
int cal_play_start(int uaif, uint32_t period_bytes, uint32_t periods);  /* 0 or < 0; Calliope must be READY / CALL */
uint8_t *cal_play_buf(void);                /* NULL when not playing */
uint32_t cal_ring_bytes(void);
uint32_t cal_play_pos(uint32_t *status);    /* byte RDMA0 reads (from its STATUS), CAL_NOPOS when not running */
uint32_t cal_play_ptr(uint32_t *count);     /* Calliope's last POINTER as a ring offset, CAL_NOPOS none */
void cal_play_stop(void);                   /* best effort, safe at any point; leaves the call's routes alone */
int cal_playing(void);
const char *cal_why(void);
/* diagnostics: cal_trace(1) logs every PCM message sent ("cal: ipc -> ..." with its fields and whether Calliope took
 * it); cal_dump(tag) logs RDMA0's registers, SYSPOWER, ROUTE0, SPUS0 and the UAIF in one line, only while Calliope runs
 * (never inside a powered-off AUD domain) */
void cal_trace(int on);
void cal_dump(const char *tag);

/* ---- the driver probe (drvprobe.h) ------------------------------------------------------------------------------
 * level 0: reads only: the AUD domain and ABOX CPU state (PMU), IP_INDEX / SysMMU CTRL when the domain is on, and
 *          whether Calliope's three files are on VENDOR, with their sizes (read into abox_rmem, which nothing uses
 *          while ABOX's CPU is off; skipped when it is on).
 * level 1: silent: Calliope booted (amps off, the codec left in reset), 300 ms for its post-boot messages, RDMA0
 *          plays zeroes to UAIF1 through Calliope, 1 s measured in One UI's geometry, then 0.5 s in snd.c's if it moved,
 *          else 0.5 s to UAIF0 (One UI's boot route); every PCM message, Calliope's replies (raw) and its own log lines
 *          after the stream's start are logged; stopped; abox_off() (the AUD domain ends off, as Android's runtime
 *          suspend leaves it: by design, not restored).
 * level 2: attended: the same with a 1 kHz tone at -24 dBFS on the earpiece amp at its lowest level, 2 s.
 * Calliope's boot is one call of up to ~3.5 s (abox_boot() waits for BOOT_DONE: ~64 ms on Android, 3 s allowed). */
int drv_calliope_probe(int level, unsigned *wait_ms);
void drv_calliope_abort(void);
#define DRV_CALLIOPE_BUDGET_MS 12000U

/* the probe's outside parts (the kernel's by default; the host test installs fakes). file: a VENDOR file into dst,
 * its size or < 0 (as abox_hal.file). amp_*: amp.c's; only level 2 uses them. */
struct cal_probe_ops {
    long (*file)(void *ctx, const char *path, void *dst, uint32_t cap);
    int (*amp_probe)(void);
    int (*amp_receiver)(int on, int level);
    void (*amp_off)(void);
};
void cal_probe_setup(const struct abox_hal *h, const struct cal_probe_ops *ops);   /* host test: before the probe */

#endif
