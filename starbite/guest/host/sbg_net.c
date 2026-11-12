/* SPDX-License-Identifier: GPL-2.0-only */
/* sbg_net: the host side of the v3 service (net). Stub until implemented (sbghost_int.h has the rules). */
#include "sbghost_int.h"

void sbg_net_load(void)
{
}

void sbg_net_poll(void)
{
}

int sbg_net_send(const uint8_t *pkt, unsigned len)
{
    (void)pkt;
    (void)len;
    return 0;
}

void sbg_net_config(int link, int uplink, const uint8_t guest_ip[4], const uint8_t gw_ip[4], const uint8_t dns_ip[4],
                    unsigned prefix)
{
    (void)link, (void)uplink, (void)guest_ip, (void)gw_ip, (void)dns_ip, (void)prefix;
}
