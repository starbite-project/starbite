/* SPDX-License-Identifier: APSL-2.0 */
/*
 * Samsung S6E3HA8 (Galaxy S9, 1440 x 2960 AMOLED, MIPI command mode, DSC) through sbd_dsim: brightness by AOR
 * (the method S-Boot itself uses on this phone), display on/off, sleep in/out, panel ID.
 *
 * Every command goes through one allow-list. Registers that write the panel's flash, its calibration or its
 * supply voltages can never be sent from this driver.
 */
#ifndef SBD_PANEL_H
#define SBD_PANEL_H

#include "sbd_dsim.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SBD_PANEL_AOR_MIN       0x000cu         /* brightest: the vendor's 400-nit AOR */
#define SBD_PANEL_AOR_DIMMEST   0x0b76u         /* the vendor's 2-nit AOR: level 0 */
#define SBD_PANEL_AOR_MAX       0x0ba3u         /* S-Boot's darkest; never above (the frame is 2979 lines) */
#define SBD_PANEL_LINES         2979u           /* vertical total: AOR counts dark lines of it */

struct sbd_panel {
	struct sbd_io io;
	struct sbd_dsim_stats stats;
	int      level;                 /* last brightness set, 0-255; -1 never */
	uint32_t aor;                   /* last AOR sent */
	bool     display_off, asleep;
	uint32_t refused;               /* commands the allow-list stopped */
};

void sbd_panel_init(struct sbd_panel *p, const struct sbd_io *io);

/* May this DCS command byte be sent at all? */
bool sbd_panel_cmd_allowed(uint8_t cmd);

/* Brightness level 0-255 -> AOR (0x0b76 .. 0x000c), monotonic, light roughly with the square of the level. */
uint32_t sbd_panel_aor_for_level(int level);

/* F0 5A 5A, B1 hi lo, F7 03, F0 A5 A5. One retry of the whole sequence; the key is always locked again. */
int sbd_panel_set_aor(struct sbd_panel *p, uint32_t aor);
int sbd_panel_set_brightness(struct sbd_panel *p, int level);

/* Display off (28) / on (29) between the level-1 key. The panel keeps its memory and its link. */
int sbd_panel_display(struct sbd_panel *p, bool on);

/* Sleep in (10) / out (11) between the level-1 key, with the vendor's waits. Off unless asked (NOTES.md). */
int sbd_panel_sleep(struct sbd_panel *p, bool in);

/* DCS 04, three bytes. This phone's panel answers 91 04 44. */
int sbd_panel_read_id(struct sbd_panel *p, uint8_t id[3]);

#ifdef __cplusplus
}
#endif
#endif /* SBD_PANEL_H */
