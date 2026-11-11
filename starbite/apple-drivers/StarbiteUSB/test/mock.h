/* SPDX-License-Identifier: APSL-2.0
 * Host-test model of the hardware StarbiteUSB touches: register blocks, the DWC3's endpoint commands, its
 * event buffer and TRBs (the controller's side of them), and a little USB host for control transfers.
 */
#ifndef SBUSB_MOCK_H
#define SBUSB_MOCK_H
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../hw/sbusb_func.h"
#include "../hw/sbusb_regs.h"

#define MOCK_DMA_PA     0x91000000ull
#define MOCK_CMDS       4096

struct mock_cmd { unsigned phys; uint32_t cmd, p0, p1, p2; };

struct mock {
	uint32_t dwc3[0x10000 / 4];
	uint32_t phy[0x200 / 4];
	uint32_t single[SBUSB_BLK_COUNT];
	uint32_t writes[SBUSB_BLK_COUNT];
	uint32_t missing;               /* blocks reported unmapped */
	int csftrst_stuck;
	int fail_cmd, fail_phys;        /* this endpoint command on this endpoint answers status 1 */
	uint64_t now_us;
	uint8_t *dma;
	uint32_t evt_w;                 /* the controller's write position in the event buffer */
	uint64_t cur_trb[32];           /* per physical endpoint: the TRB the controller is at; 0 = no transfer */
	struct mock_cmd cmd[MOCK_CMDS];
	unsigned ncmd;
	unsigned syncs;
};

extern struct mock M;
extern int checks, failures;
#define CHECK(c) do { ++checks; if (!(c)) { ++failures; printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECK_EQ(a, b) do { ++checks; long long a_ = (long long)(a), b_ = (long long)(b); if (a_ != b_) { ++failures; \
	printf("  FAIL %s:%d: %s = %lld (0x%llx), expected %s = %lld (0x%llx)\n", __FILE__, __LINE__, #a, a_, a_, #b, b_, b_); } } while (0)

void mock_reset(void);                          /* a powered-up, halted DWC3 3.00a and a PHY in SIDDQ */
struct sbusb_io mock_io(void);
struct sbusb_dma mock_dma(void);
void mock_event(uint32_t ev);
void mock_dev_event(unsigned type, unsigned info);
void mock_ep_event(unsigned phys, unsigned type, unsigned status);
/* number of commands `cmd` issued to phys since command index `from` */
unsigned mock_count(unsigned from, unsigned phys, uint32_t cmd);
/* the controller's TRB for phys (NULL: none armed or not owned by hardware) */
uint8_t *mock_trb(unsigned phys);
uint8_t *mock_trb_buf(const uint8_t *trb);
/* The controller finishes the TRB it is at on phys: `moved` bytes went (data copied in for OUT), HWO cleared,
 * the event posted (XferComplete with LST, XferInProgress without), the ring followed. 0 if none was armed. */
int mock_complete(unsigned phys, const uint8_t *data, uint32_t moved);

/* a host's bus reset + connect at high (0) or full (1) speed */
void mock_host_reset(struct sbusb_func *f, int full_speed);
/* One whole control transfer. data: IN: where the device's bytes go; OUT: what the host sends.
 * Returns the IN bytes received, 0 for an acknowledged OUT / no-data request, -1 when the device stalled. */
int mock_control(struct sbusb_func *f, unsigned type, unsigned req, unsigned value, unsigned index, unsigned length,
    uint8_t *data);
/* reset, address 5, configuration 1, data interface alt 1 */
void mock_enumerate(struct sbusb_func *f, int full_speed);
int mock_summary(const char *name);

#endif
