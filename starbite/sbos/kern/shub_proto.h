/* SPDX-License-Identifier: GPL-2.0-only
 * The BCM4775 sensor hub's wire formats (docs/SENSOR-HUB.md 2.3, 2.4, 2.5, 3), pure C: no hardware, no libsb. Built
 * into the kernel (shub.c drives it) and into the host test (starbite/tools/shub-host-test.c, against the GPL sample
 * frames of Samsung's bbdpl/bcm_gps_regs.c and bbd_rpc_lh.c).
 *
 *   SSI      the SPI framing: a write frame [0x58][M lo][M hi][M bytes][00 00], the status byte
 *   packets  B0-stuffed, [seq][size][flags][a detail byte per flag][payload][crc], CRC-8 0x4D sent nibble-swapped
 *   RPC      [id][len][data] in a packet's payload (id and len: one byte, or two big-endian with bit 7 set)
 *   ROM      the sync request / response, the version answer, a patch blob's records as memory writes
 *   SSP      Samsung's sensor protocol on RPC 0x20 / 0x21: the 9-byte request, the answer stream, data frames
 *   compass  heading and tilt from an accelerometer and a magnetometer sample (Android axes) */
#ifndef SBOS_SHUB_PROTO_H
#define SBOS_SHUB_PROTO_H
#include <stdint.h>

/* ---- packets ---- */
#define SHP_F_ACK 0x001U            /* detail: the sequence number acknowledged */
#define SHP_F_RELIABLE 0x002U       /* detail: the sender's reliable sequence number */
#define SHP_F_REL_ACK 0x004U        /* detail: the reliable sequence number acknowledged */
#define SHP_F_REL_NACK 0x008U       /* detail: the last reliable sequence number the receiver has */
#define SHP_F_LOST 0x010U
#define SHP_F_GARBAGE 0x020U
#define SHP_F_SIZE_EXT 0x040U       /* detail: the size's high byte (set by shp_pkt_build as needed) */
#define SHP_F_EXT 0x080U            /* detail: flag bits 15:8 (set by shp_pkt_build as needed) */
#define SHP_F_INTERNAL 0x100U
#define SHP_F_IGNORE_SEQ 0x200U
#define SHP_MAX_PAYLOAD 2048U
#define SHP_RAW_MAX (SHP_MAX_PAYLOAD + 14U)
#define SHP_STUFFED_MAX (2U * SHP_RAW_MAX + 4U)

struct shp_pkt {
    uint8_t seq;
    uint16_t flags;
    uint8_t ack, rel, rel_ack, rel_nack, lost, garbage;
    const uint8_t *payload;
    uint16_t len;
    uint8_t crc;                    /* as sent (nibble-swapped) */
};

uint8_t shp_crc(const uint8_t *p, unsigned n);     /* over [size .. last payload byte], nibble-swapped */
/* the stuffed packet (B0 00 ... B0 01) into out; 0 if it does not fit or the payload is too long */
unsigned shp_pkt_build(uint8_t *out, unsigned cap, const struct shp_pkt *p);
/* an unstuffed packet [seq .. crc]: 1 parsed (payload points into raw), 0 bad CRC / flags / size */
int shp_pkt_parse(const uint8_t *raw, unsigned n, struct shp_pkt *out);

struct shp_rx {
    uint8_t st;
    unsigned n;
    uint32_t good, bad, garbage;    /* packets parsed, packets refused, bytes outside a packet */
    uint8_t buf[SHP_RAW_MAX + 2U];
};
void shp_rx_init(struct shp_rx *r);
/* one stream byte: 1 a packet is complete (valid until the next byte), -1 a packet ended but was refused, 0 else */
int shp_rx_byte(struct shp_rx *r, uint8_t b, struct shp_pkt *out);

/* ---- RPC ---- */
#define SHP_RPC_GET_VERSION 0x00U
#define SHP_RPC_VERSION 0x03U
#define SHP_RPC_WRITE_MEM 0x05U
#define SHP_RPC_START 0x2EU
#define SHP_RPC_SENSOR_REQ 0x20U
#define SHP_RPC_SENSOR_RSP 0x21U
unsigned shp_rpc_put(uint8_t *out, unsigned cap, uint16_t id, const uint8_t *data, unsigned len);
/* the next RPC of a payload from *off: 1 (data, len), 0 the end, -1 malformed */
int shp_rpc_next(const uint8_t *p, unsigned n, unsigned *off, uint16_t *id, const uint8_t **data, uint16_t *len);

/* ---- SSI ---- */
#define SHP_SSI_WRITE 0x58U         /* full duplex, 2-byte lengths, flow control */
#define SHP_SSI_READ_A 0x50U        /* an earlier port / the Tab Active3 notes */
#define SHP_SSI_READ_B 0x70U        /* Samsung's bcm_spi_open as written */
#define SHP_SSI_OVERHEAD 5U
struct shp_ssi_w {
    uint8_t status;
    unsigned n;                     /* chip bytes that came with the write (already cut to what the frame held) */
    const uint8_t *chip;
    unsigned accepted;              /* how many of our bytes the chip took */
};
unsigned shp_ssi_wframe(uint8_t *mosi, unsigned cap, const uint8_t *d, unsigned m);    /* m + 5, 0 if too long */
void shp_ssi_wparse(const uint8_t *miso, unsigned m, struct shp_ssi_w *r);
unsigned shp_ssi_space(uint8_t status);            /* free bytes in the chip's receive ring */
#define SHP_SSI_ST_PE 0x40U         /* the status byte's protocol-error bit */

/* ---- the ROM dialogue ---- */
struct shp_sync {
    uint8_t n, last_seq_rx, last_ack, last_rel_rx, next_seq, next_rel, last_rel_acked;
};
unsigned shp_sync_build(uint8_t *out, unsigned cap, uint8_t attempt);
int shp_sync_parse(const struct shp_pkt *p, struct shp_sync *s);      /* 1: a sync response */
struct shp_version {
    uint32_t asic, rom, patch;
    int big_endian;                 /* the byte order the fields were sent in */
};
#define SHP_ASIC_4775B0 0x004775B0U
/* RPC 0x03's 12 bytes; the byte order is taken from which reading of the first word is the 4775 B0. 1 recognised
 * (v filled), 0 not a 4775 B0 (v holds the little-endian reading) */
int shp_version_parse(const uint8_t *d, unsigned n, struct shp_version *v);
/* a patch blob's records [type][len u16 LE][body]: the next 0xF1 write from *off: 1, 0 the end, -1 malformed or
 * another record type */
int shp_patch_next(const uint8_t *blob, unsigned n, unsigned *off, uint32_t *addr, const uint8_t **data, unsigned *len);
/* RPC 0x05 [u32 addr][u16 n][n bytes] / RPC 0x2E [u32 addr], in the byte order the version answer used */
unsigned shp_rpc_write_mem(uint8_t *out, unsigned cap, uint32_t addr, const uint8_t *d, unsigned n, int big_endian);
unsigned shp_rpc_start(uint8_t *out, unsigned cap, uint32_t addr, int big_endian);

/* ---- SSP ---- */
#define SSP_READ 0U
#define SSP_WRITE 1U
#define SSP_HUB2AP 2U
#define SSP_RETURN 4U
#define SSP_CMD_WHOAMI 0x0FU
#define SSP_CMD_FORMATION 0xF1U
#define SSP_CMD_SCAN 0xF4U
#define SSP_CMD_MAG_MATRIX 0xFDU
#define SSP_CMD_FW_REV 0xF0U
#define SSP_CMD_ADD 0xA1U
#define SSP_CMD_REMOVE 0xA2U
#define SSP_CMD_TIMESTAMP 0xA7U
#define SSP_WHOAMI 0x55U
#define SSP_HDR 9U
#define SSP_DATA_MAX 2048U
void shp_ssp_hdr(uint8_t out[SSP_HDR], uint8_t cmd, uint16_t len, uint16_t opts, uint32_t data);
/* the body of "enable a sensor" (cmd 0xA1, WRITE 10) */
void shp_ssp_enable(uint8_t out[10], uint8_t sensor, int32_t period_ms, int32_t latency_ms, int8_t batch_opts);
/* an SSP request (or a write's data phase) as RPC 0x20 [u16 LE length][bytes] */
unsigned shp_ssp_wrap(uint8_t *out, unsigned cap, const uint8_t *ssp, unsigned n);

enum { SSP_EV_NONE = 0, SSP_EV_READ, SSP_EV_WRITE_GO, SSP_EV_FRAME, SSP_EV_ERR };
struct shp_ssp_rx {
    uint8_t st, type;
    uint16_t opts, need, n;
    uint16_t read_len;              /* the driver: the result length of the read it is waiting for (0 none) */
    uint8_t buf[SSP_DATA_MAX];
};
void shp_ssp_rx_init(struct shp_ssp_rx *s);
/* feed the hub's SSP bytes; stops at the first event (*used bytes eaten): READ / FRAME: s->buf, s->n;
 * WRITE_GO: the hub echoed a write's header (s->opts) and waits for its data; ERR: the stream was reset */
int shp_ssp_feed(struct shp_ssp_rx *s, const uint8_t *p, unsigned n, unsigned *used);

#define SSP_SENSOR_ACCEL 0U
#define SSP_SENSOR_MAG 4U
#define SSP_SENSOR_STEPS 18U
enum { SSP_REC_SENSOR = 1, SSP_REC_INIT_DONE, SSP_REC_OTHER };
struct shp_rec {
    int kind;
    uint8_t type, sensor;
    int16_t x, y, z;                /* accelerometer (4096 = 1 g), magnetometer */
    uint8_t accuracy, overflow;     /* magnetometer */
    uint32_t steps;                 /* step counter: steps since the last report */
    uint64_t ts;
    const uint8_t *raw;             /* a sensor record: its sample bytes (inside the frame), as sent */
    uint8_t raw_len;
};
/* the next record of a data frame from *off: 1, 0 the end, -1 a type or sensor of unknown length (drop the rest) */
int shp_frame_next(const uint8_t *f, unsigned n, unsigned *off, struct shp_rec *r);

/* ---- compass and level (Android axes: x right, y to the top edge, z out of the screen; at rest the
 *      accelerometer reads + on the axis that points up) ---- */
/* the direction the top edge points, 0.1 degrees clockwise from magnetic north (0..3599): 1, or 0 when it cannot be
 * told (free fall, the field along gravity, a zero sample) */
int shp_heading(int ax, int ay, int az, int mx, int my, int mz, int *deg_x10);
/* pitch: the top edge above the horizon; roll: the right edge above the horizon; 0.1 degrees, -900..900 */
int shp_tilt(int ax, int ay, int az, int *pitch_x10, int *roll_x10);
#endif
