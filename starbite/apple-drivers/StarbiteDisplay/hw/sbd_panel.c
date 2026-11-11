/* SPDX-License-Identifier: APSL-2.0 */
/* S6E3HA8 panel commands (see sbd_panel.h). Command facts and their sources: NOTES.md. */
#include "sbd_panel.h"

void
sbd_panel_init(struct sbd_panel *p, const struct sbd_io *io)
{
	unsigned char *b = (unsigned char *)p;

	for (size_t i = 0; i < sizeof(*p); i++) {
		b[i] = 0;
	}
	p->io = *io;
	p->level = -1;
}

bool
sbd_panel_cmd_allowed(uint8_t cmd)
{
	switch (cmd) {
	case 0xf0:      /* level-2 key */
	case 0x9f:      /* level-1 key */
	case 0xb1:      /* AOR (only at offset 0: this driver never sends the B0 offset command) */
	case 0xf7:      /* take gamma / AOR */
	case 0x28:      /* display off */
	case 0x29:      /* display on */
	case 0x10:      /* sleep in */
	case 0x11:      /* sleep out */
		return true;
	default:
		/* never: C0/C1 (the panel's flash), C8/B3/A1 (calibration), B5/F4/FC (supply voltages), CA (gamma
		 * without the calibration maths), 01 (reset), 34 (TE off: the DECON's trigger would stop), 53 (AOD),
		 * B0 (offset: turns a harmless register write into another one) */
		return false;
	}
}

static int
panel_send(struct sbd_panel *p, const uint8_t *bytes, uint32_t n)
{
	if (n == 0 || !sbd_panel_cmd_allowed(bytes[0])) {
		p->refused++;
		return SBD_DSIM_BAD_ARG;
	}
	return sbd_dsim_dcs_write(&p->io, bytes, n, &p->stats);
}

uint32_t
sbd_panel_aor_for_level(int level)
{
	uint32_t l, on_min, on_max, on;

	if (level < 0) {
		level = 0;
	}
	if (level > 255) {
		level = 255;
	}
	l = (uint32_t)level;
	/* lit lines of the frame: light is about proportional to them; the square makes steps look even */
	on_min = SBD_PANEL_LINES - SBD_PANEL_AOR_DIMMEST;
	on_max = SBD_PANEL_LINES - SBD_PANEL_AOR_MIN;
	on = on_min + ((on_max - on_min) * l * l + (255u * 255u) / 2) / (255u * 255u);
	return SBD_PANEL_LINES - on;
}

int
sbd_panel_set_aor(struct sbd_panel *p, uint32_t aor)
{
	static const uint8_t key_on[3] = { 0xf0, 0x5a, 0x5a }, commit[2] = { 0xf7, 0x03 }, key_off[3] = { 0xf0, 0xa5, 0xa5 };
	uint8_t b1[3];
	int err = SBD_DSIM_OK;

	if (aor < SBD_PANEL_AOR_MIN) {
		aor = SBD_PANEL_AOR_MIN;
	}
	if (aor > SBD_PANEL_AOR_MAX) {
		aor = SBD_PANEL_AOR_MAX;
	}
	b1[0] = 0xb1;
	b1[1] = (uint8_t)(aor >> 8);
	b1[2] = (uint8_t)aor;
	if (!sbd_dsim_ready(&p->io)) {
		return SBD_DSIM_NOT_READY;
	}
	for (int attempt = 0; attempt < 2; attempt++) {
		err = panel_send(p, key_on, sizeof(key_on));
		if (err == SBD_DSIM_OK) {
			err = panel_send(p, b1, sizeof(b1));
		}
		if (err == SBD_DSIM_OK) {
			err = panel_send(p, commit, sizeof(commit));
		}
		(void)panel_send(p, key_off, sizeof(key_off));          /* never leave the key open */
		if (err == SBD_DSIM_OK) {
			p->aor = aor;
			break;
		}
		p->io.udelay(p->io.ctx, 20000);
	}
	return err;
}

int
sbd_panel_set_brightness(struct sbd_panel *p, int level)
{
	int err;

	level = level < 0 ? 0 : (level > 255 ? 255 : level);
	err = sbd_panel_set_aor(p, sbd_panel_aor_for_level(level));
	if (err == SBD_DSIM_OK) {
		p->level = level;
	}
	return err;
}

/* one command between the level-1 key, the key closed again whatever happened */
static int
keyed(struct sbd_panel *p, uint8_t cmd)
{
	static const uint8_t key1_on[3] = { 0x9f, 0xa5, 0xa5 }, key1_off[3] = { 0x9f, 0x5a, 0x5a };
	int err = panel_send(p, key1_on, sizeof(key1_on));

	if (err == SBD_DSIM_OK) {
		err = panel_send(p, &cmd, 1);
	}
	(void)panel_send(p, key1_off, sizeof(key1_off));
	return err;
}

int
sbd_panel_display(struct sbd_panel *p, bool on)
{
	int err = keyed(p, on ? 0x29 : 0x28);

	if (err == SBD_DSIM_OK) {
		p->display_off = !on;
	}
	return err;
}

int
sbd_panel_sleep(struct sbd_panel *p, bool in)
{
	int err;

	if (in) {
		err = sbd_panel_display(p, false);
		p->io.udelay(p->io.ctx, 20000);                 /* a frame and more */
		if (err == SBD_DSIM_OK) {
			err = keyed(p, 0x10);
		}
		p->io.udelay(p->io.ctx, 100000);                /* the vendor's wait after sleep-in */
		if (err == SBD_DSIM_OK) {
			p->asleep = true;
		}
		return err;
	}
	err = keyed(p, 0x11);
	p->io.udelay(p->io.ctx, 30000);                         /* the vendor's 10 ms and a frame more */
	if (err != SBD_DSIM_OK) {
		return err;
	}
	p->asleep = false;
	if (p->level >= 0) {
		(void)sbd_panel_set_brightness(p, p->level);
	}
	p->io.udelay(p->io.ctx, 20000);
	return sbd_panel_display(p, true);
}

int
sbd_panel_read_id(struct sbd_panel *p, uint8_t id[3])
{
	int n = sbd_dsim_dcs_read(&p->io, 0x04, id, 3, &p->stats);

	return n == 3 ? SBD_DSIM_OK : (n < 0 ? n : SBD_DSIM_RX_ERROR);
}
