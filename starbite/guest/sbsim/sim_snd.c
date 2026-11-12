/* sim_snd: sbsnd's stand-in for Starbite's mixer (host/sbg_snd.h). While the guest plays, it pulls the playback at
 * real time (48 frames per ms of now_ms, the way a speaker DMA would) and analyses it: per 100-ms block of 4800
 * frames a Goertzel at 440 Hz against the block's energy (integer only: sbsim is built -mgeneral-regs-only), the RMS,
 * and runs of >= 1 ms of exact silence between sound (what an underrun inserts). While the guest captures, it pushes a
 * 1 kHz sine (amplitude 16384, both channels) at real time. When the playback stops it prints one line:
 *   sbsim: snd: playback ... 440 Hz in N of M blocks (PASS|FAIL) ...
 * (sim_snd_report() prints it on demand too). PASS: at least 25 full blocks (2.5 s) and 90 % of them 440 Hz. */
#include "../host/sbg_snd.h"

static void sim_snd_hint(int playing, int capturing) { (void)playing, (void)capturing; }
static const struct sbg_snd sim_snd_plat = {
    .caps = SBG_SND_PLAY | SBG_SND_CAPTURE,
    .block_frames = 96,                         /* sbsim's loop runs every 2 ms */
    .latency_frames = 0,
    .active = sim_snd_hint,
};
#define SIM_SND (&sim_snd_plat)

#define SIM_SND_BLOCK 4800U                     /* 100 ms; 440 Hz is bin 44 exactly */
#define SIM_SND_G440 130855                     /* 2 cos(2 pi 440 / 48000), Q16 */
static const int16_t sim_snd_sin1k[48] = {      /* 1 kHz at 48 kHz: one period, amplitude 16384 */
    0, 2139, 4240, 6270, 8192, 9974, 11585, 12998, 14189, 15137, 15826, 16244,
    16384, 16244, 15826, 15137, 14189, 12998, 11585, 9974, 8192, 6270, 4240, 2139,
    0, -2139, -4240, -6270, -8192, -9974, -11585, -12998, -14189, -15137, -15826, -16244,
    -16384, -16244, -15826, -15137, -14189, -12998, -11585, -9974, -8192, -6270, -4240, -2139,
};
static struct {
    int pulling, pushing, reported;
    uint64_t p_t0, p_done, c_t0, c_done;        /* frames due since t0 */
    uint64_t p_guest, c_taken, c_offered;       /* frames: the guest's in the pull, taken by the guest's ring */
    uint32_t stalls;                            /* times sbsim fell > 100 ms behind (skipped) */
    /* the current block */
    int64_t s1, s2, energy;
    uint32_t n;
    /* results */
    uint32_t blocks, tone, rms_sum;
    uint32_t zrun, gaps, seen_sound;
    uint64_t fill_sum, fill_n;
    uint32_t c_phase;
} sim_ss;
static int16_t sim_snd_buf[2 * 960];

static void sim_snd_block_end(void)
{
    int64_t p = sim_ss.s1 * sim_ss.s1 + sim_ss.s2 * sim_ss.s2 - ((SIM_SND_G440 * sim_ss.s1) >> 16) * sim_ss.s2;
    int64_t ne = (int64_t)SIM_SND_BLOCK * sim_ss.energy;
    uint32_t ratio = ne > 0 ? (uint32_t)(200 * p / ne) : 0;     /* % of the block's energy at 440 Hz */
    uint64_t ms = (uint64_t)sim_ss.energy / SIM_SND_BLOCK, r = 0;    /* mean square -> rms (integer sqrt) */
    for (uint64_t b = 1ULL << 30; b; b >>= 1)
        if ((r + b) * (r + b) <= ms)
            r += b;
    sim_ss.blocks++;
    if (ratio >= 90) {
        sim_ss.tone++;
        sim_ss.rms_sum += (uint32_t)r;
    }
    sim_ss.s1 = sim_ss.s2 = sim_ss.energy = 0;
    sim_ss.n = 0;
}

static void sim_snd_analyse(const int16_t *lr, unsigned n)
{
    for (unsigned i = 0; i < n; ++i) {
        int64_t m = ((int32_t)lr[2 * i] + lr[2 * i + 1]) / 2;
        int64_t s0 = m + ((SIM_SND_G440 * sim_ss.s1) >> 16) - sim_ss.s2;
        sim_ss.s2 = sim_ss.s1;
        sim_ss.s1 = s0;
        sim_ss.energy += m * m;
        if (lr[2 * i] == 0 && lr[2 * i + 1] == 0) {
            sim_ss.zrun++;
        } else {
            if (sim_ss.seen_sound && sim_ss.zrun >= 48)     /* >= 1 ms of nothing between sound: a gap */
                sim_ss.gaps++;
            sim_ss.zrun = 0;
            sim_ss.seen_sound = 1;
        }
        if (++sim_ss.n == SIM_SND_BLOCK)
            sim_snd_block_end();
    }
}

static void sim_snd_ms(uint64_t frames)       /* frames -> "N.N ms" */
{
    putu(frames / 48);
    puts_(".");
    putu(frames % 48 * 10 / 48);
    puts_(" ms");
}

static void sim_snd_report(void)
{
    const struct sbg_snd_stats *s = sbg_snd_stats();
    int pass = sim_ss.blocks >= 25 && sim_ss.tone * 10 >= sim_ss.blocks * 9;
    puts_("sbsim: snd: playback ");
    putu(sim_ss.p_done / 48);
    puts_(" ms pulled (");
    putu(sim_ss.p_guest);
    puts_(" guest frames): 440 Hz in ");
    putu(sim_ss.tone);
    puts_(" of ");
    putu(sim_ss.blocks);
    puts_(" 100-ms blocks (");
    puts_(pass ? "PASS" : "FAIL");
    puts_("), rms ");
    putu(sim_ss.tone ? sim_ss.rms_sum / sim_ss.tone : 0);
    puts_(", silent gaps ");
    putu(sim_ss.gaps);
    puts_(", underruns ");
    putu(s->underruns);
    puts_(" (");
    putu(s->underrun_frames);
    puts_(" frames), ring fill avg ");
    sim_snd_ms(sim_ss.fill_n ? sim_ss.fill_sum / sim_ss.fill_n : 0);
    puts_(" max ");
    sim_snd_ms(s->fill_max);
    puts_(", host stalls ");
    putu(sim_ss.stalls);
    puts_("; capture: ");
    putu(sim_ss.c_offered / 48);
    puts_(" ms of 1 kHz offered, ");
    putu(sim_ss.c_taken);
    puts_(" frames taken, ");
    putu(s->overrun_frames);
    puts_(" dropped; starts play ");
    putu(s->play_starts);
    puts_(" capture ");
    putu(s->cap_starts);
    puts_("\n");
}

static void sim_snd_tick(uint64_t t)
{
    int p, c;
    sbg_snd_active(&p, &c);
    if (p && !sim_ss.pulling) {
        sim_ss.pulling = 1;
        sim_ss.p_t0 = t;
        sim_ss.p_done = 0;
    }
    if (sim_ss.pulling) {
        uint64_t due = (t - sim_ss.p_t0) * 48;
        if (due > sim_ss.p_done + 4800) {           /* sbsim itself fell behind: do not burst */
            sim_ss.stalls++;
            sim_ss.p_done = due - 960;
        }
        while (sim_ss.p_done < due) {
            unsigned n = due - sim_ss.p_done > 960 ? 960U : (unsigned)(due - sim_ss.p_done);
            sim_ss.p_guest += sbg_snd_pull(sim_snd_buf, n);
            sim_ss.fill_sum += sbg_snd_stats()->fill_frames;
            sim_ss.fill_n++;
            sim_snd_analyse(sim_snd_buf, n);
            sim_ss.p_done += n;
        }
        if (!p) {                               /* the guest's stream ended (and the ring is empty) */
            sim_ss.pulling = 0;
            if (sim_ss.p_guest && !sim_ss.reported) {
                sim_ss.reported = 1;
                sim_snd_report();
            }
        }
    }
    if (c && !sim_ss.pushing) {
        sim_ss.pushing = 1;
        sim_ss.c_t0 = t;
        sim_ss.c_done = 0;
    }
    if (sim_ss.pushing) {
        uint64_t due = (t - sim_ss.c_t0) * 48;
        if (due > sim_ss.c_done + 4800)
            sim_ss.c_done = due - 960;
        while (sim_ss.c_done < due) {
            unsigned n = due - sim_ss.c_done > 960 ? 960U : (unsigned)(due - sim_ss.c_done);
            for (unsigned i = 0; i < n; ++i) {
                sim_snd_buf[2 * i] = sim_snd_buf[2 * i + 1] = sim_snd_sin1k[sim_ss.c_phase];
                sim_ss.c_phase = sim_ss.c_phase == 47 ? 0 : sim_ss.c_phase + 1;
            }
            sim_ss.c_taken += sbg_snd_push(sim_snd_buf, n);
            sim_ss.c_offered += n;
            sim_ss.c_done += n;
        }
        if (!c)
            sim_ss.pushing = 0;
    }
}
