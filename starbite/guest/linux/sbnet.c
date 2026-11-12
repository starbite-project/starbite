// SPDX-License-Identifier: GPL-2.0-only
/*
 * sbnet: the guest's IP link to Starbite OS (contract v3, ../sbshm-net.h; the rules for services in sbg_core.h).
 *
 * One net_device, "sbnet0": a layer-3 point-to-point link like tun (ARPHRD_NONE, IFF_POINTOPOINT | IFF_NOARP, no
 * link-layer header). Starbite OS's NAT (Track G) is the other end; the guest is 10.77.0.2/30 behind gateway and DNS
 * 10.77.0.1 by default, whatever the host's config says (user space applies it: rootfs/sbnet-up).
 *
 *   tx: no offloads (no checksum, SG or GSO features: Track G's NAT needs complete checksums; a CHECKSUM_PARTIAL
 *       packet forwarded from elsewhere is completed with skb_checksum_help); MTU 1400 from the host (the NAT drops
 *       non-first fragments). ndo_start_xmit copies the packet (IPv4 or IPv6, skb->protocol) into slot tx_head: data, len,
 *       dmb, tx_head + 1; the host's doorbell once per batch (xmit_more). The host never rings when it consumes, so
 *       the queue stops when the ring is full (backpressure instead of drops: socket buffers hold the rest) and a
 *       1-jiffy timer wakes it once the host's tx_tail has made room.
 *   rx: NAPI. The doorbell interrupt (sbnet_kick, hard irq) schedules it; the poll copies each slot from rx_tail
 *       into an skb (protocol from the IP version nibble), then dmb, rx_tail. Polled mode (no doorbell interrupt, or
 *       db_to_guest / db_to_host 0): the timer schedules NAPI every jiffy; otherwise it is a 1 s safety net.
 *   config: the host's struct under cfg_seq (seqlock). A changed cfg_seq (seen by the kick, NAPI or the timer)
 *       queues a work item: it copies the config, sets the carrier from `link`, max_mtu from `mtu`, logs the change,
 *       wakes pollers of /sys/class/net/sbnet0/sbnet/seq and sends a KOBJ_CHANGE uevent (SBNET_SEQ, SBNET_LINK,
 *       SBNET_UPLINK in the environment). Read-only attributes in /sys/class/net/sbnet0/sbnet/: guest_ip, gw_ip,
 *       dns_ip, prefix, link (0/1), uplink (none/wifi/cell), mtu, seq, host_drops (the host's rx_drop), tx_full
 *       (times the tx ring was found full), ring_broken.
 *
 * link 0: the carrier goes off; the address and routes stay (sockets stay open); what is sent meanwhile is dropped
 * by the kernel's link watch (noop qdisc), as the host would drop it. link 1: the carrier comes back, traffic flows.
 */
#include <linux/delay.h>
#include <linux/if_arp.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/jiffies.h>
#include <linux/kobject.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <linux/spinlock.h>
#include <linux/timer.h>
#include <linux/workqueue.h>

#include "sbg_core.h"
#include "../sbshm-net.h"

#define SBNET_PKT_MAX	(SBSHM_NET_SLOT - 8U)
#define SBNET_MIN_MTU	68U
#define SBNET_WAKE_FREE	32U		/* free tx slots before a stopped queue starts again */
#define SBNET_MAP_LEN	(SBSHM_NET_RX_OFF + SBSHM_NET_SLOTS * SBSHM_NET_SLOT)

struct sbnet_cfg {
	u32 seq, link, uplink, mtu, prefix;
	u8 guest_ip[4], gw_ip[4], dns_ip[4];
};

static struct sbshm_net *sbn;			/* write-combine vmap: the struct, then the two rings */
static struct sbshm_net_slot *sbn_txs, *sbn_rxs;
static struct net_device *sbn_dev;
static struct napi_struct sbn_napi;
static struct timer_list sbn_timer;
static struct work_struct sbn_cfg_work;
static DEFINE_SPINLOCK(sbn_cfg_lock);		/* sbn_cfg (sysfs readers vs the work item) */
static struct sbnet_cfg sbn_cfg;
static u32 sbn_tx_head, sbn_rx_tail;		/* this side's own indices (never read back from the region) */
static u32 sbn_seen_seq;
static atomic_t sbn_tx_full = ATOMIC_INIT(0);
static bool sbn_ready, sbn_polled, sbn_broken;

static bool sbnet_cfg_read(struct sbnet_cfg *c)
{
	int i;

	for (i = 0; i < 200; i++) {
		u32 s = READ_ONCE(sbn->cfg_seq);

		if (!(s & 1)) {
			sbshm_dmb();
			c->link = READ_ONCE(sbn->link);
			c->uplink = READ_ONCE(sbn->uplink);
			c->mtu = READ_ONCE(sbn->mtu);
			c->prefix = READ_ONCE(sbn->prefix);
			memcpy(c->guest_ip, sbn->guest_ip, 4);
			memcpy(c->gw_ip, sbn->gw_ip, 4);
			memcpy(c->dns_ip, sbn->dns_ip, 4);
			sbshm_dmb();
			if (READ_ONCE(sbn->cfg_seq) == s) {
				c->seq = s;
				return true;
			}
		}
		if (in_task())
			usleep_range(20, 50);
		else
			udelay(5);
	}
	return false;
}

static u32 sbnet_mtu_of(const struct sbnet_cfg *c)
{
	return clamp_t(u32, c->mtu ? c->mtu : SBSHM_NET_MTU, SBNET_MIN_MTU, SBNET_PKT_MAX);
}

static const char *sbnet_uplink_name(u32 u)
{
	return u == SBSHM_NET_UPLINK_WIFI ? "wifi" : u == SBSHM_NET_UPLINK_CELL ? "cell" : "none";
}

static bool sbnet_free_slots(u32 want)
{
	return SBSHM_NET_SLOTS - (sbn_tx_head - READ_ONCE(sbn->tx_tail)) >= want;
}

/* a stopped queue starts again once the host has made room (from NAPI, the timer) */
static void sbnet_tx_wake(void)
{
	if (netif_queue_stopped(sbn_dev) && sbnet_free_slots(SBNET_WAKE_FREE))
		netif_wake_queue(sbn_dev);
}

static void sbnet_check(void)
{
	if (READ_ONCE(sbn->cfg_seq) != READ_ONCE(sbn_seen_seq))
		schedule_work(&sbn_cfg_work);
	napi_schedule(&sbn_napi);		/* a no-op while the interface is down */
}

void sbnet_kick(void)
{
	if (READ_ONCE(sbn_ready))
		sbnet_check();
}

static void sbnet_timer_fn(struct timer_list *t)
{
	sbnet_check();
	if (netif_running(sbn_dev) && netif_queue_stopped(sbn_dev))
		sbnet_tx_wake();
	mod_timer(&sbn_timer, jiffies + ((sbn_polled || (netif_running(sbn_dev) && netif_queue_stopped(sbn_dev))) ?
					 1 : HZ));
}

static void sbnet_cfg_fn(struct work_struct *w)
{
	struct net_device *dev = sbn_dev;
	struct sbnet_cfg c, old;
	char e_seq[24], e_link[16], e_up[24];
	char *envp[] = { e_seq, e_link, e_up, NULL };

	if (!sbnet_cfg_read(&c))
		return;			/* the host is mid-write for long: the next kick or tick tries again */
	spin_lock_bh(&sbn_cfg_lock);
	old = sbn_cfg;
	sbn_cfg = c;
	spin_unlock_bh(&sbn_cfg_lock);
	WRITE_ONCE(sbn_seen_seq, c.seq);
	WRITE_ONCE(dev->max_mtu, sbnet_mtu_of(&c));
	if (c.link) {
		netif_carrier_on(dev);
		if (netif_running(dev))
			sbnet_tx_wake();
	} else {
		netif_carrier_off(dev);
	}
	if (memcmp(&old.link, &c.link, sizeof(c) - offsetof(struct sbnet_cfg, link)))
		netdev_info(dev, "host config %u: link %u (%s), %pI4/%u, gateway %pI4, DNS %pI4, mtu %u\n", c.seq,
			    c.link, sbnet_uplink_name(c.uplink), c.guest_ip, c.prefix, c.gw_ip, c.dns_ip, c.mtu);
	sysfs_notify(&dev->dev.kobj, "sbnet", "seq");
	snprintf(e_seq, sizeof(e_seq), "SBNET_SEQ=%u", c.seq);
	snprintf(e_link, sizeof(e_link), "SBNET_LINK=%u", c.link);
	snprintf(e_up, sizeof(e_up), "SBNET_UPLINK=%s", sbnet_uplink_name(c.uplink));
	kobject_uevent_env(&dev->dev.kobj, KOBJ_CHANGE, envp);
}

static int sbnet_poll(struct napi_struct *napi, int budget)
{
	struct net_device *dev = sbn_dev;
	u32 head = READ_ONCE(sbn->rx_head);
	int n = 0;

	if (head - sbn_rx_tail > SBSHM_NET_SLOTS) {
		if (!sbn_broken)
			netdev_err(dev, "the rx ring is broken (rx_head %u, rx_tail %u): receive stopped\n", head,
				   sbn_rx_tail);
		sbn_broken = true;
		napi_complete_done(napi, 0);
		return 0;
	}
	while (n < budget && sbn_rx_tail != head) {
		struct sbshm_net_slot *s = &sbn_rxs[sbn_rx_tail & (SBSHM_NET_SLOTS - 1)];
		struct sk_buff *skb = NULL;
		unsigned int len;

		sbshm_dmb();
		len = READ_ONCE(s->len);
		if (len && len <= SBNET_PKT_MAX)
			skb = napi_alloc_skb(napi, len);
		if (skb) {
			u8 v;

			memcpy(skb_put(skb, len), s->data, len);
			v = skb->data[0] >> 4;
			if (v == 4 && len >= sizeof(struct iphdr)) {
				skb->protocol = htons(ETH_P_IP);
			} else if (v == 6 && len >= sizeof(struct ipv6hdr)) {
				skb->protocol = htons(ETH_P_IPV6);
			} else {
				dev_kfree_skb_any(skb);
				skb = NULL;
			}
		}
		if (skb) {
			skb->dev = dev;
			skb_reset_mac_header(skb);
			skb_reset_network_header(skb);
			skb->pkt_type = PACKET_HOST;
			dev_dstats_rx_add(dev, len);
			napi_gro_receive(napi, skb);
		} else {
			dev_dstats_rx_dropped(dev);
		}
		sbn_rx_tail++;
		n++;
		if (sbn_rx_tail == head) {
			u32 h = READ_ONCE(sbn->rx_head);

			if (h - sbn_rx_tail <= SBSHM_NET_SLOTS)
				head = h;
		}
	}
	if (n) {
		sbshm_dmb();			/* the copies, then the slots back to the host */
		WRITE_ONCE(sbn->rx_tail, sbn_rx_tail);
	}
	sbnet_tx_wake();
	if (READ_ONCE(sbn->cfg_seq) != READ_ONCE(sbn_seen_seq))
		schedule_work(&sbn_cfg_work);
	if (n < budget && napi_complete_done(napi, n) && READ_ONCE(sbn->rx_head) != sbn_rx_tail)
		napi_schedule(napi);
	return n;
}

static netdev_tx_t sbnet_xmit(struct sk_buff *skb, struct net_device *dev)
{
	unsigned int len = skb->len;
	struct sbshm_net_slot *s;

	if ((skb->protocol != htons(ETH_P_IP) && skb->protocol != htons(ETH_P_IPV6)) || !len ||
	    len > SBNET_PKT_MAX || sbn_broken)
		goto drop;
	if (!sbnet_free_slots(1)) {		/* stopped below before it fills: only a host gone wrong */
		atomic_inc(&sbn_tx_full);
		netif_stop_queue(dev);
		mod_timer(&sbn_timer, jiffies + 1);
		return NETDEV_TX_BUSY;
	}
	/* the host's NAT rewrites checksums incrementally: it needs whole ones. sbnet0 advertises no checksum offload,
	 * so only a packet forwarded from another device can still be partial here */
	if (skb->ip_summed == CHECKSUM_PARTIAL && skb_checksum_help(skb))
		goto drop;
	s = &sbn_txs[sbn_tx_head & (SBSHM_NET_SLOTS - 1)];
	if (skb_copy_bits(skb, 0, s->data, len))
		goto drop;
	WRITE_ONCE(s->len, len);
	WRITE_ONCE(s->flags, 0);
	sbshm_dmb();				/* the slot, then the index */
	WRITE_ONCE(sbn->tx_head, ++sbn_tx_head);
	dev_dstats_tx_add(dev, len);
	dev_consume_skb_any(skb);
	if (!sbnet_free_slots(1)) {
		atomic_inc(&sbn_tx_full);
		netif_stop_queue(dev);
		if (sbnet_free_slots(SBNET_WAKE_FREE))	/* the host drained meanwhile */
			netif_start_queue(dev);
		else
			mod_timer(&sbn_timer, jiffies + 1);
	}
	if (!netdev_xmit_more() || netif_queue_stopped(dev))
		sbg_ring_host();
	return NETDEV_TX_OK;
drop:
	WRITE_ONCE(sbn->tx_drop, READ_ONCE(sbn->tx_drop) + 1);
	dev_dstats_tx_dropped(dev);
	dev_kfree_skb_any(skb);
	return NETDEV_TX_OK;
}

static int sbnet_open(struct net_device *dev)
{
	napi_enable(&sbn_napi);
	netif_start_queue(dev);
	napi_schedule(&sbn_napi);		/* whatever came while it was down */
	return 0;
}

static int sbnet_stop(struct net_device *dev)
{
	netif_stop_queue(dev);
	napi_disable(&sbn_napi);
	return 0;
}

static void sbnet_tx_timeout(struct net_device *dev, unsigned int txq)
{
	netdev_warn(dev, "tx stalled: tx_head %u, the host's tx_tail %u\n", sbn_tx_head, READ_ONCE(sbn->tx_tail));
	sbg_ring_host();
}

static const struct net_device_ops sbnet_ops = {
	.ndo_open = sbnet_open,
	.ndo_stop = sbnet_stop,
	.ndo_start_xmit = sbnet_xmit,
	.ndo_tx_timeout = sbnet_tx_timeout,
};

/* ---- /sys/class/net/sbnet0/sbnet/ ---------------------------------------------------------------------- */
static struct sbnet_cfg sbnet_cfg_now(void)
{
	struct sbnet_cfg c;

	spin_lock_bh(&sbn_cfg_lock);
	c = sbn_cfg;
	spin_unlock_bh(&sbn_cfg_lock);
	return c;
}

#define SBNET_IP_ATTR(_f)									\
static ssize_t _f##_show(struct device *d, struct device_attribute *a, char *buf)		\
{												\
	struct sbnet_cfg c = sbnet_cfg_now();							\
	return sysfs_emit(buf, "%pI4\n", c._f);							\
}												\
static DEVICE_ATTR_RO(_f)
#define SBNET_U32_ATTR(_f)									\
static ssize_t _f##_show(struct device *d, struct device_attribute *a, char *buf)		\
{												\
	struct sbnet_cfg c = sbnet_cfg_now();							\
	return sysfs_emit(buf, "%u\n", c._f);							\
}												\
static DEVICE_ATTR_RO(_f)

SBNET_IP_ATTR(guest_ip);
SBNET_IP_ATTR(gw_ip);
SBNET_IP_ATTR(dns_ip);
SBNET_U32_ATTR(prefix);
SBNET_U32_ATTR(link);
SBNET_U32_ATTR(mtu);
SBNET_U32_ATTR(seq);

static ssize_t uplink_show(struct device *d, struct device_attribute *a, char *buf)
{
	return sysfs_emit(buf, "%s\n", sbnet_uplink_name(sbnet_cfg_now().uplink));
}
static DEVICE_ATTR_RO(uplink);

static ssize_t host_drops_show(struct device *d, struct device_attribute *a, char *buf)
{
	return sysfs_emit(buf, "%u\n", READ_ONCE(sbn->rx_drop));
}
static DEVICE_ATTR_RO(host_drops);

static ssize_t tx_full_show(struct device *d, struct device_attribute *a, char *buf)
{
	return sysfs_emit(buf, "%u\n", atomic_read(&sbn_tx_full));
}
static DEVICE_ATTR_RO(tx_full);

static ssize_t ring_broken_show(struct device *d, struct device_attribute *a, char *buf)
{
	return sysfs_emit(buf, "%u\n", READ_ONCE(sbn_broken) ? 1 : 0);
}
static DEVICE_ATTR_RO(ring_broken);

static struct attribute *sbnet_attrs[] = {
	&dev_attr_guest_ip.attr, &dev_attr_gw_ip.attr, &dev_attr_dns_ip.attr, &dev_attr_prefix.attr,
	&dev_attr_link.attr, &dev_attr_uplink.attr, &dev_attr_mtu.attr, &dev_attr_seq.attr,
	&dev_attr_host_drops.attr, &dev_attr_tx_full.attr, &dev_attr_ring_broken.attr, NULL,
};

static const struct attribute_group sbnet_group = {
	.name = "sbnet",
	.attrs = sbnet_attrs,
};

static void sbnet_setup(struct net_device *dev)
{
	dev->netdev_ops = &sbnet_ops;
	dev->type = ARPHRD_NONE;
	dev->hard_header_len = 0;
	dev->addr_len = 0;
	dev->flags = IFF_POINTOPOINT | IFF_NOARP | IFF_MULTICAST;
	dev->tx_queue_len = 1000;		/* a qdisc (not IFF_NO_QUEUE): it holds packets while the ring is full */
	dev->mtu = SBSHM_NET_MTU;
	dev->min_mtu = SBNET_MIN_MTU;
	dev->max_mtu = SBNET_PKT_MAX;
	dev->watchdog_timeo = 5 * HZ;
	dev->pcpu_stat_type = NETDEV_PCPU_STAT_DSTATS;
}

void sbnet_init(struct device *dev, bool irq_ok)
{
	struct net_device *nd;
	struct sbnet_cfg c = {};
	int ret;

	if (!sbg_svc(SBSHM_SVC_NET))
		return;
	BUILD_BUG_ON(sizeof(struct sbshm_net) != 320);
	BUILD_BUG_ON(offsetof(struct sbshm_net, tx_head) != 64 || offsetof(struct sbshm_net, rx_tail) != 256);
	BUILD_BUG_ON(sizeof(struct sbshm_net_slot) != SBSHM_NET_SLOT);
	BUILD_BUG_ON(sizeof(struct sbshm_net) > SBSHM_NET_TX_OFF);
	BUILD_BUG_ON(SBSHM_NET_OFF + SBNET_MAP_LEN > SBSHM_P9_OFF);
	if (shm_size < SBSHM_NET_OFF + (u64)SBNET_MAP_LEN) {
		dev_warn(dev, "sbnet: the shared region is too small for the packet rings\n");
		return;
	}
	sbn = sbg_vmap_wc(SBSHM_NET_OFF, SBNET_MAP_LEN);
	if (!sbn) {
		dev_warn(dev, "sbnet: cannot map the rings\n");
		return;
	}
	sbn_txs = (struct sbshm_net_slot *)((u8 *)sbn + SBSHM_NET_TX_OFF);
	sbn_rxs = (struct sbshm_net_slot *)((u8 *)sbn + SBSHM_NET_RX_OFF);
	/* the guest's indices start where the host's are (0 after the host's load; never trust more than that) */
	sbn_tx_head = READ_ONCE(sbn->tx_head);
	sbn_rx_tail = READ_ONCE(sbn->rx_tail);
	if (!sbnet_cfg_read(&c))
		dev_warn(dev, "sbnet: the host's config stays mid-write; using it once it settles\n");
	sbn_cfg = c;
	sbn_seen_seq = c.seq;

	nd = alloc_netdev(0, "sbnet%d", NET_NAME_PREDICTABLE, sbnet_setup);
	if (!nd)
		return;
	nd->sysfs_groups[0] = &sbnet_group;
	nd->max_mtu = sbnet_mtu_of(&c);
	nd->mtu = min_t(u32, SBSHM_NET_MTU, nd->max_mtu);
	SET_NETDEV_DEV(nd, dev);
	netif_napi_add(nd, &sbn_napi, sbnet_poll);
	INIT_WORK(&sbn_cfg_work, sbnet_cfg_fn);
	sbn_polled = !irq_ok || !READ_ONCE(hdr->db_to_guest) || !READ_ONCE(hdr->db_to_host);
	timer_setup(&sbn_timer, sbnet_timer_fn, 0);
	sbn_dev = nd;
	netif_carrier_off(nd);
	if (c.link)
		netif_carrier_on(nd);
	ret = register_netdev(nd);
	if (ret) {
		dev_warn(dev, "sbnet: register_netdev: %d\n", ret);
		netif_napi_del(&sbn_napi);
		free_netdev(nd);
		sbn_dev = NULL;
		return;
	}
	WRITE_ONCE(sbn_ready, true);
	mod_timer(&sbn_timer, jiffies + (sbn_polled ? 1 : HZ));
	netdev_info(nd, "host link %u (%s), %pI4/%u, gateway %pI4, DNS %pI4, mtu %u; receive by %s\n", c.link,
		    sbnet_uplink_name(c.uplink), c.guest_ip, c.prefix, c.gw_ip, c.dns_ip, c.mtu,
		    sbn_polled ? "timer poll" : "doorbell");
}
