# sbgw: the Linux guest's network (gw.c, gw.h)

The Linux guest under Starbite OS reaches the Internet through **sbgw**, a gateway on our own stack: like
QEMU's slirp, but NAT (NAPT) onto the phone's current uplink, an `sbnet` (Wi-Fi: Ethernet; mobile data: raw IP on
the modem's PDP channel). Freestanding C like the rest of `starbite/wifi`: no C library, no allocation, no globals,
the log callback gets fixed texts only (no addresses, ports or names).

## The guest's link (fixed, as slirp's)

| what | address |
| --- | --- |
| network | 10.0.2.0/24 |
| gateway (router, DHCP server, ARP, ICMP echo) | 10.0.2.2, MAC 52:55:0a:00:02:02 |
| DNS server (relayed through the NAT to the uplink's DNS server, UDP and TCP) | 10.0.2.3 |
| the guest (DHCP lease 1 day, T1 12 h, T2 21 h; the guest's MAC remembered) | 10.0.2.15 |

MTU: the guest's link is 1500 and DHCP does **not** send option 26: the uplink can change under a lease (Wi-Fi
1500, mobile data 1400 or the PDP's own). Instead the gateway
- clamps the MSS option of every TCP SYN, both ways, to the uplink's mtu - 40 (RFC 6691 style; the far end's
  SYN-ACK too, so the guest never sends segments too large for mobile data),
- answers a larger packet with DF set with ICMP "fragmentation needed" carrying the uplink's MTU (PMTUD),
- fragments a larger packet without DF on the way out.

Fragments the guest sends are dropped (counted in `frags`; Linux sets DF on TCP and on UDP up to the path MTU,
so only UDP datagrams above 1472 bytes would be lost). Fragments arriving from the uplink are dropped by sbnet
already. IPv6 frames from the guest are dropped (no RA, so the guest has link-local IPv6 only and falls back to
IPv4 at once).

## API

```c
struct sbgw_os { void *ctx;
    int (*to_guest)(void *ctx, const uint8_t *frame, uint32_t len);   /* an Ethernet frame to the guest */
    uint64_t (*now_ms)(void *ctx); uint32_t (*random)(void *ctx);
    void (*log)(void *ctx, const char *line); };                       /* optional */
void sbgw_init(struct sbgw *gw, const struct sbgw_os *os);
void sbgw_guest_rx(struct sbgw *gw, const uint8_t *frame, uint32_t len);   /* a frame the guest sent */
void sbgw_tick(struct sbgw *gw);                                           /* every 100-500 ms */
void sbgw_set_uplink(struct sbgw *gw, struct sbnet *up);                   /* NULL: none */
unsigned sbgw_conns(const struct sbgw *gw);
```

`struct sbgw` is about 12 KiB (the table: 256 connections). The kernel owns one, plus one `sbnet` per uplink, and
calls `sbgw_set_uplink` whenever its choice changes (Wi-Fi first, else mobile data, else NULL). A switch (or the
uplink losing or changing its address, noticed at the next packet or tick) flushes the table and sends every TCP
connection of the guest a RST at once (seq = what the guest expects next), so nothing hangs. With no uplink, a SYN,
UDP or ping gets ICMP network unreachable (a `connect()` fails at once with ENETUNREACH), other TCP segments a RST.

sbgw_set_uplink hooks the uplink's sbnet (`sbnet.ip_hook`, see below); `sbnet_init` clears that hook, and the next
`sbgw_tick` puts it back, so re-initialising the Wi-Fi sbnet after a reconnect needs nothing more.

## NAT

- TCP, UDP and ICMP echo; other protocols and the guest's own ICMP errors are dropped.
- The uplink's ports (and ICMP ids) 40000-48999 belong to the NAT (`SBGW_PORT_LO/HI`): sbtcp's ephemeral ports and
  sbnet's DNS ports are 49152-65535, NTP 50123, so the uplink's own sockets never collide. sbnet's own ping id is
  skipped. Packets to the NAT's ports that match no connection are dropped (no RST: stealth).
- UDP: endpoint-independent mapping (one uplink port per guest port, RFC 4787), address-and-port-dependent
  filtering (only the far end a connection went to gets in). TCP and ICMP: one mapping per connection.
- Timeouts: TCP 30 s until the far end's SYN-ACK, 2 h established, 240 s after one FIN, 10 s after both, forgotten
  at a RST; UDP 60 s, 180 s once answered, 15 s to port 53; ICMP 30 s. When the table is full the connection idle
  longest goes (a TCP one gets a RST).
- Checksums: TCP/UDP/ICMP incrementally (RFC 1624) for the address, port and MSS changes; the IPv4 header
  recomputed. The guest's IP ID is kept; the TTL decremented (TTL 1 from the guest: ICMP time exceeded from 10.0.2.2,
  so traceroute shows the gateway).
- ICMP errors from the Internet about the guest's packets (unreachable, time exceeded, parameter problem) are
  translated back, the quoted packet too.
- The guest's checksums are not verified (a NAT passes bad ones on as they are). **The guest must send complete
  checksums**: Track E's NIC must not offer checksum offload (virtio-net: no VIRTIO_NET_F_CSUM / GUEST_CSUM, no
  TSO/GSO features), so Linux computes every checksum itself and sends frames of at most 1514 bytes.

## sbnet hooks (net.h)

- `struct sbnet` gained, at its end: `ip_hook` + `hook_ctx` (set by sbgw_set_uplink) and `held_guest`. The hook sees
  every valid IPv4 packet addressed to the uplink's address while bound, before sbnet's own ICMP/UDP/TCP dispatch,
  and returns 1 when it took it. sbnet's DHCP, DNS, ICMP, UDP and sbtcp work as before.
- `int sbnet_ip_out(struct sbnet *n, uint8_t *f, uint32_t total)`: a complete IPv4 packet at f + 14 (its header
  final), routed like sbnet's own (Ethernet: through ARP to the destination on our subnet or the gateway; raw IP as
  it is), refused above `mtu`, for broadcast, multicast or our own address. A NAT packet never displaces one of
  sbnet's own packets held while ARP resolves (it is dropped instead; TCP retransmits).
- `struct sbnet_os` is unchanged: no initializer anywhere needs an update.

## What Track E (starbite/guest, the guest's virtual NIC) provides

1. A frame pipe both ways between the guest's NIC and the kernel, e.g. two shared-memory rings like virtio-net's
   (descriptors of at most 1514-byte Ethernet frames, no virtio-net header offloads).
2. Guest -> gateway: for each frame the guest queued, `sbgw_guest_rx(&gw, frame, len)` (in the kernel's network
   context, the same one that runs sbnet; sbgw is not re-entrant).
3. Gateway -> guest: `os.to_guest(ctx, frame, len)` copies the frame into the guest's receive ring (and raises its
   interrupt); return non-zero when the ring is full (the frame is dropped and counted; TCP recovers).
4. A MAC for the guest's NIC (any locally administered one; the gateway learns it from DHCP or the first frame).
5. `sbgw_tick` every few hundred ms from the network loop, and `sbgw_set_uplink` from the uplink choice.

Inside the guest nothing special: DHCP on its NIC (`dhclient`, `udhcpc`, systemd-networkd) gives 10.0.2.15/24,
the route and DNS 10.0.2.3.

## Tests

- `sh starbite/wifi/host/run-gw.sh` (the Mac): freestanding builds of gw.c and net.c, then `host/gw-test.c` under
  ASan/UBSan: a guest (sbnet + sbtcp, its DHCP client too), Wi-Fi-like and mobile-like uplinks and a simulated
  Internet (router, far host with sbtcp, DNS, echo, an ICMP-unreachable host). DHCP (bound, renewal, NAK), ARP,
  pings, the DNS relay next to the uplink's own DNS, UDP mapping, TCP transfers (clean and with 3 % loss and
  reordering), the uplink's own TCP alongside, MSS clamp both ways, raw IP with no packet above 1400, DF/fragment
  handling, ICMP error translation, uplink switch / loss / new address / no uplink (RSTs and unreachables), table
  exhaustion and timeouts; every packet from the gateway checked against a from-scratch checksum; then 200k
  mutated and random packets on both sides.
- `limactl shell starbite -- sudo sh starbite/wifi/host/run-gw-netns.sh` (the VM, as root): a network namespace
  `sbguest` is the guest with Linux's own stack (busybox udhcpc, ping, nslookup, glibc's resolver, curl) on a TAP
  whose other end is `host/gw-tap` (sbgw + two sbnet uplinks: a TAP as Wi-Fi, a TUN with MTU 1400 as mobile data,
  masqueraded to the Internet by Linux). DHCP, pings, DNS, HTTPS, a 64 MiB local and a 25 MB Internet download on
  each uplink, a switch mid-download, no uplink; everything removed at the end.
