/* sim_net: sbnet's stand-in (stub: drops what the guest sends) */
static void sim_net_rx(const uint8_t *pkt, unsigned len) { (void)pkt; (void)len; }
static void sim_net_tick(uint64_t t) { (void)t; }
