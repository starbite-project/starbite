/* sim_snd: sbsnd's stand-in (stub: no sound offered) */
#define SIM_SND ((const struct sbg_snd *)0)
static void sim_snd_tick(uint64_t t) { (void)t; }
