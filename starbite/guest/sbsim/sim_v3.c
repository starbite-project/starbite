/* sbsim's stand-ins for the contract-v3 services (F_V3 = 128 in the manifest flags; included by sbsim.c, which has
 * puts_ / putu / now_ms / the guest log hook). Each service's file defines its sbg_os part and a tick:
 *   sim_net.c  sim_net_rx (+ a responder: ICMP echo, UDP echo, a DNS stub) and sim_net_tick
 *   sim_p9.c   SIM_FS (a struct sbg_fs * over a small in-memory tree, or NULL) and sim_p9_tick
 *   sim_snd.c  SIM_SND (a struct sbg_snd *, or NULL) and sim_snd_tick
 * The tests themselves run in the guest (a test root logs "v3-test: <svc>: PASS|FAIL ..." and "V3-TEST-DONE"). */
static uint32_t sim_unix_s(int32_t *tz_min)
{
    *tz_min = 120;                              /* a fixed clock for tests: 2026-11-12 12:00:00 UTC + uptime */
    return 1794484800U + (uint32_t)(now_ms() / 1000);
}
#include "sim_net.c"
#include "sim_p9.c"
#include "sim_snd.c"
static void sim_v3_tick(uint64_t t)
{
    sim_net_tick(t);
    sim_p9_tick(t);
    sim_snd_tick(t);
}
