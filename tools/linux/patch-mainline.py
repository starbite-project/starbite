#!/usr/bin/env python3
"""Starbite's own edits, on top of the S9+ port (run by build-kernel.sh).

    patch-mainline.py --tree SRC        the mainline tree (before the port's build71.sh)
    patch-mainline.py --port WORK       the work copy of tools/linux/port (never the vendored copy)

Both are idempotent: each edit is checked against its anchor (exactly once) and
skipped when its marker is already there, so a drifted input fails loudly.

Tree (mainline v7.1):
  1-3. dw_mmc: "starbite,dma-32bit", error reports, "starbite,max-data-timeout"
       (the microSD; the timeout one is required).
  4.   panfrost: no S9P_GEM_DMA32. The port's patch_tree.py puts every Panfrost
       buffer in the ~1.9 GB low zone (shared with CMA); with kmsro the
       compositor scans out CMA dumb buffers, never Panfrost shmem, so it buys
       nothing here (plan 1.3.6). A marker comment in panfrost_gem.c makes
       patch_tree.py skip its hunk.
  5.   panfrost: runtime-PM autosuspend 1000 ms on the Exynos 9810 (50 ms
       elsewhere). Every G3D power-up costs two TZPC SMCs, a PMU power-up and a
       Panfrost reset (plan 1.3.3). Runtime: .../17500000.gpu/power/autosuspend_delay_ms.

Port (work copy):
  6.   s9p-decon: flip event at the TE edge that latches the new buffer
       (module param early_event, default on; plan 1.3.1, cause C).
       6b-6d: drm_name=, the framebuffer's pitch/offset, ARGB8888.
       6e: geometry dumps (geom_log, dump_geometry) and the panel_mask_off
       test for the black bars (plan, "Display: black bars").
  7.   s9p-g3d: the rail check. With step_volt off the rail must read 950 mV
       before every clock change and after every power-up; a lower reading is
       rewritten and counted (rail_fixups). The 260 MHz step uses 950 mV even
       with step_volt on (plan 1.3.2).
  8.   s9p-cpufreq: transition_delay_us 5000 (was 20000), a boot parameter
       (s9p_cpufreq.transition_delay_us=); schedutil's rate_limit_us follows it.
  9.   build71.sh: every star2lte name becomes starlte; the Shero logo is optional;
       the Starbite config fragment is merged (merge_config.sh -m + olddefconfig)
       and checked right after the port's own enables (STARBITE_CONFIG,
       STARBITE_CHECK = tools/linux/check-kernel.py); the boot.img size limit
       only warns (the loader takes the Image from its store).

The G3D power domain needs no edit: s9p_g3d_pd.always_on is already a 0644
parameter (echo 1 > /sys/module/s9p_g3d_pd/parameters/always_on, or
s9p_g3d_pd.always_on=1 on the command line), set per distro.
"""
import argparse
import re
import sys
from pathlib import Path


def edit(root, rel, anchor, new, marker, count=1):
    p = Path(root) / rel
    s = p.read_text()
    if marker in s:
        print(f'{rel}: {marker!r} already there')
        return
    if s.count(anchor) != count:
        sys.exit(f'{rel}: anchor for {marker!r} found {s.count(anchor)}x, expected {count}')
    p.write_text(s.replace(anchor, new))
    print(f'{rel}: {marker!r} applied')


# ---------------------------------------------------------------- the tree --
def patch_tree(src):
    mmc = 'drivers/mmc/host/dw_mmc.c'
    # 1. dw_mmc: "starbite,dma-32bit". The S9's microSD controller (DW MSHC
    #    2.70a) reports 64-bit IDMAC addressing in HCON and needs the 64-bit
    #    descriptor format, but its transfers failed with the descriptor list
    #    in the high RAM bank (0x8_8000_0000): the stock DT uses desc-size 4
    #    (32-bit). Keep the 64-bit descriptors, cap the DMA mask at 32 bits.
    edit(src, mmc,
         '''			if (dma_set_mask_and_coherent(host->dev, DMA_BIT_MASK(64)))
				dev_info(host->dev, "Fail to set 64-bit DMA mask");''',
         '''			if (dma_set_mask_and_coherent(host->dev, DMA_BIT_MASK(64)))
				dev_info(host->dev, "Fail to set 64-bit DMA mask");
			/* starbite,dma-32bit: 64-bit descriptors, 32-bit reach */
			if (device_property_read_bool(host->dev, "starbite,dma-32bit") &&
			    !dma_set_mask_and_coherent(host->dev, DMA_BIT_MASK(32)))
				dev_info(host->dev, "DMA limited to 32-bit addresses\\n");''',
         'starbite,dma-32bit')

    # 2. dw_mmc: say why a transfer failed (the stock messages are dev_dbg and
    #    this kernel has no dynamic debug). First 20 errors only.
    edit(src, mmc,
         '''	else if (status & SDMMC_INT_RESP_ERR)
		cmd->error = -EIO;
	else
		cmd->error = 0;
''',
         '''	else if (status & SDMMC_INT_RESP_ERR)
		cmd->error = -EIO;
	else
		cmd->error = 0;

	{	/* starbite: command errors */
		static int starbite_cmd_errs;
		if (cmd->error && starbite_cmd_errs++ < 20)
			dev_err(host->dev, "starbite: CMD%u arg 0x%08x error %d rintsts 0x%08x resp 0x%08x\\n",
				cmd->opcode, cmd->arg, cmd->error, status, cmd->resp[0]);
	}
''',
         'starbite: CMD%u')
    edit(src, mmc,
         '''		dev_dbg(host->dev, "data error, status 0x%08x\\n", status);''',
         '''		{	/* starbite: data errors with the IDMAC status */
			static int starbite_data_errs;
			if (starbite_data_errs++ < 20)
				dev_err(host->dev, "starbite: data error, rintsts 0x%08x idsts64 0x%08x dir %d blocks %u x %u\\n",
					status, host->dma_64bit_address ? mci_readl(host, IDSTS64) : mci_readl(host, IDSTS),
					host->dir_status, data->blocks, data->blksz);
		}''',
         'starbite: data error')

    # 3. dw_mmc: "starbite,max-data-timeout". The vendor driver always writes
    #    TMOUT = ~0 on this controller; with mainline's per-request value every
    #    read ended in DRTO (rintsts 0x20c), even at 400 kHz.
    edit(src, mmc,
         '''	if (drv_data && drv_data->set_data_timeout)
		return drv_data->set_data_timeout(host, timeout_ns);
''',
         '''	if (drv_data && drv_data->set_data_timeout)
		return drv_data->set_data_timeout(host, timeout_ns);

	if (device_property_read_bool(host->dev, "starbite,max-data-timeout")) {
		mci_writel(host, TMOUT, 0xFFFFFFFF);
		return;
	}
''',
         'starbite,max-data-timeout')

    # 4. panfrost: keep the port's S9P_GEM_DMA32 hunk out. patch_tree.py adds it
    #    only when "S9P_GEM_DMA32" is absent from the file, so the marker below
    #    (put in before build71.sh runs patch_tree.py) is all it takes.
    gem = 'drivers/gpu/drm/panfrost/panfrost_gem.c'
    g = (Path(src) / gem).read_text()
    if 'mapping_set_gfp_mask' in g and 'S9P_GEM_DMA32: below 4 GB' in g:
        sys.exit(f'{gem}: S9P_GEM_DMA32 hunk is applied: the tree was not reset (build-kernel.sh resets it)')
    edit(src, gem,
         '#include <linux/err.h>\n',
         '#include <linux/err.h>\n'
         '/* S9P_GEM_DMA32 hunk dropped by Starbite (tools/linux/patch-mainline.py): with\n'
         ' * kmsro the compositor scans out CMA dumb buffers, never Panfrost shmem. */\n',
         'S9P_GEM_DMA32 hunk dropped by Starbite')

    # 5. panfrost: 1 s autosuspend on the Exynos 9810 only.
    drv = 'drivers/gpu/drm/panfrost/panfrost_drv.c'
    p = Path(src) / drv
    s = p.read_text()
    if 'STARBITE_AUTOSUSPEND' in s:
        print(f'{drv}: STARBITE_AUTOSUSPEND already there')
    else:
        m = re.findall(r'^(\t+)pm_runtime_set_autosuspend_delay\(([^,]+), 50\);[^\n]*\n', s, re.M)
        if len(m) != 1:
            sys.exit(f'{drv}: autosuspend anchor found {len(m)}x, expected 1')
        ind, dev = m[0]
        old = re.search(r'^\t+pm_runtime_set_autosuspend_delay\([^,]+, 50\);[^\n]*\n', s, re.M).group(0)
        new = (f'{ind}/* STARBITE_AUTOSUSPEND: each G3D power-up on the Exynos 9810 costs two\n'
               f'{ind} * TZPC SMCs, a PMU power-up and a GPU reset, so stay up for 1 s there */\n'
               f'{ind}pm_runtime_set_autosuspend_delay({dev},\n'
               f'{ind}\tof_device_is_compatible({dev}->of_node, "samsung,exynos9810-mali") ?\n'
               f'{ind}\t1000 : 50); /* ~3 frames */\n')
        p.write_text(s.replace(old, new, 1))
        print(f'{drv}: STARBITE_AUTOSUSPEND applied (device {dev})')


# ------------------------------------------------------------ the port copy --
DECON_PARAM = '''
/*
 * Starbite (plan 1.3.1, "cause C"): the flip event used to be armed after the
 * shadow latch had already been seen, so drm_crtc_arm_vblank_event() asked for
 * the TE AFTER the latching one: one flip per two TEs, a hard 30 fps cap. With
 * early_event the update returns right after programming the address and the
 * frame-start interrupt sends the event at the edge that latched it (once
 * SHADOW_REG_UPDATE_REQ reads clear). The old buffer is still released only
 * after the latch, so there is no tearing. 0 = the port's original behaviour.
 */
static bool s9p_early_event = true;
module_param_named(early_event, s9p_early_event, bool, 0644);
MODULE_PARM_DESC(early_event, "send the flip event at the TE that latches the new buffer (Starbite; 0 = one TE later, as before)");
'''

DECON_FLUSH = '''
/* Starbite early_event: send a parked flip event; caller holds event_lock */
static void s9p_flip_event_send(struct s9p_decon *priv)
{
	struct drm_crtc *crtc = &priv->pipe.crtc;

	if (!priv->flip_event)
		return;
	drm_crtc_send_vblank_event(crtc, priv->flip_event);
	drm_crtc_vblank_put(crtc);
	priv->flip_event = NULL;
}

'''

DECON_IRQ_OLD = '''		drm_crtc_handle_vblank(&priv->pipe.crtc);
	}
	if (pend & DECON_FRAME_DONE_INT_PEND)'''
DECON_IRQ_NEW = '''		drm_crtc_handle_vblank(&priv->pipe.crtc);
		/* Starbite early_event: this edge latched the parked flip */
		if (READ_ONCE(priv->flip_event)) {
			u32 v;

			spin_lock(&priv->drm.event_lock);
			if (priv->flip_event &&
			    !readl_poll_timeout_atomic(priv->decon + DECON_SHADOW_REG_UPDATE_REQ,
						       v, !(v & s9p_shadow_mask(priv)), 1, 20))
				s9p_flip_event_send(priv);
			spin_unlock(&priv->drm.event_lock);
		}
	}
	if (pend & DECON_FRAME_DONE_INT_PEND)'''

DECON_DISABLE_OLD = '''	drm_crtc_vblank_off(&pipe->crtc);
	s9p_decon_blank(priv);'''
DECON_DISABLE_NEW = '''	spin_lock_irq(&priv->drm.event_lock);	/* Starbite early_event */
	s9p_flip_event_send(priv);
	spin_unlock_irq(&priv->drm.event_lock);
	drm_crtc_vblank_off(&pipe->crtc);
	s9p_decon_blank(priv);'''

DECON_UPDATE_OLD = '''	struct drm_framebuffer *fb = pipe->plane.state->fb;

	if (fb && priv->enabled) {
		if (s9p_wait_shadow_idle(priv)) {'''
DECON_UPDATE_NEW = '''	struct drm_framebuffer *fb = pipe->plane.state->fb;

	/*
	 * Starbite early_event: program the address and park the event; the
	 * frame-start interrupt sends it at the latching edge. No second poll,
	 * so the commit (and the next one) no longer waits a whole frame here.
	 */
	if (s9p_early_event && fb && priv->enabled) {
		unsigned long flags;

		if (s9p_wait_shadow_idle(priv)) {
			drm_err(&priv->drm,
				"previous shadow update stuck, skipping flip\\n");
		} else {
			obj = to_drm_gem_dma_obj(fb->obj[0]);
			s9p_decon_program_address(priv, obj->dma_addr);
			spin_lock_irqsave(&crtc->dev->event_lock, flags);
			/* a parked event whose flip has latched (shadow idle above) */
			s9p_flip_event_send(priv);
			if (crtc->state->event && crtc->state->active &&
			    drm_crtc_vblank_get(crtc) == 0) {
				priv->flip_event = crtc->state->event;
				crtc->state->event = NULL;
			}
			spin_unlock_irqrestore(&crtc->dev->event_lock, flags);
		}
	} else if (fb && priv->enabled) {
		if (s9p_wait_shadow_idle(priv)) {'''


DECON_GEOM = r'''/*
 * Starbite geometry (docs/LINUX-REBUILD-PLAN.md, "Display: black bars"). The
 * driver inherits every size register from sboot and logs only two of them at
 * probe, so a picture that does not reach the panel's edges cannot be placed
 * from the log. This prints everything between the framebuffer and the panel
 * that has a size in it: DECON (blender, splitter, OUTFIFO, data path), the
 * live window, the fetch channel in both banks, DSIM (resolution, porches,
 * transfers per frame, DSC slices, TE) and, on request, the DECON shadow bank
 * and the DSC encoders' PPS (picture height). Reads only; the registers are
 * the ones of blocks this driver already reads or writes, DSC only when the
 * data path says the encoder is in use.
 *
 *   geom_log=1 (default)  dumps at probe and at every pipe enable (unblank), and
 *                         one line per framebuffer geometry change on commits
 *   echo 1|2 > /sys/module/s9p_decon/parameters/dump_geometry
 *                         dump now (2 adds the shadow bank and DSC)
 *   panel_mask_off=1      S6E3HA8 "self mask" off (F0 5A 5A, 7A 00, F0 A5 A5,
 *                         the vendor's own SELF_MASK_DIS sequence) at every
 *                         enable, or right away when written at run time.
 *                         The mask darkens rows 0-149 and 2810-2959 (vendor
 *                         SELF_MASK_ENA: 7A 01 0000 0095 0AFA 0B8F) through an
 *                         image in the panel's side RAM; a test for the black
 *                         bars, off by default.
 */
#define S9P_GEOM_SHD			0x7000	/* DECON shadow (active) bank */
#define S9P_GEOM_DSC(n)			(0x4000 + 0x1000 * (n))
#define DSIM_PAYLOAD			0x005c

static unsigned int s9p_geom_log = 1;
module_param_named(geom_log, s9p_geom_log, uint, 0644);
MODULE_PARM_DESC(geom_log, "log scanout geometry: dumps at probe/enable and framebuffer geometry changes (Starbite; 0 = off)");

static struct s9p_decon *s9p_geom_dev;	/* set once probe has succeeded */
static DEFINE_MUTEX(s9p_geom_lock);

#define GR(base, off)	readl_relaxed((base) + (off))

static void s9p_decon_dump_geometry(struct s9p_decon *priv, const char *why, int level)
{
	struct device *dev = priv->drm.dev;
	void __iomem *d = priv->decon;
	void __iomem *ch = s9p_chan_regs(priv, priv->chan);
	u32 en = GR(d, DECON_DATA_PATH_CONTROL_0);
	u32 path2 = GR(d, 0x0230);
	int w, b;

	dev_info(dev, "geom[%s] decon: global=%08x trig=%08x scaled=%08x splitter=%08x/%08x outfifo=%08x/%08x/%08x\n",
		 why, GR(d, 0x0000), GR(d, DECON_HW_SW_TRIG_CONTROL), GR(d, 0x00a0), GR(d, 0x0100),
		 GR(d, 0x0104), GR(d, 0x0120), GR(d, 0x0124), GR(d, 0x0128));
	dev_info(dev, "geom[%s] decon: bg_size=%08x bg_color=%08x/%08x lrm=%08x path=%08x/%08x/%08x dsim_conn=%08x start_time=%08x\n",
		 why, GR(d, 0x0200), GR(d, 0x0208), GR(d, 0x020c), GR(d, 0x0210), en,
		 GR(d, DECON_DATA_PATH_CONTROL_1), path2, GR(d, 0x0250), GR(d, 0x0260));
	for (w = 0; w < DECON_MAX_WIN; w++) {
		void __iomem *r = d + 0x1000 + 0x30 * w;

		if (!(en & DECON_WIN_EN(w)))
			continue;
		dev_info(dev, "geom[%s] win%d: ctrl=%08x/%08x start=%08x end=%08x colormap=%08x/%08x\n",
			 why, w, GR(r, 0x00), GR(r, 0x04), GR(r, 0x08), GR(r, 0x0c), GR(r, 0x10), GR(r, 0x14));
		if (level >= 2)
			dev_info(dev, "geom[%s] win%d shadow: ctrl=%08x/%08x start=%08x end=%08x\n",
				 why, w, GR(r, S9P_GEOM_SHD + 0x00), GR(r, S9P_GEOM_SHD + 0x04),
				 GR(r, S9P_GEOM_SHD + 0x08), GR(r, S9P_GEOM_SHD + 0x0c));
	}
	for (b = 0; b < 2; b++) {
		void __iomem *c = ch + (b ? IDMA_SHD_BANK : 0);

		dev_info(dev, "geom[%s] idma%u %s: en=%08x in_con=%08x out_con=%08x src_size=%08x src_off=%08x img_size=%08x blk_off=%08x blk_size=%08x base=%08x\n",
			 why, priv->chan, b ? "shadow" : "normal", GR(c, 0x00), GR(c, IDMA_IN_CON),
			 GR(c, 0x0c), GR(c, IDMA_SRC_SIZE), GR(c, 0x14), GR(c, IDMA_IMG_SIZE),
			 GR(c, 0x24), GR(c, 0x28), GR(c, IDMA_IN_BASE_ADDR_Y));
	}
	if (priv->dsim)
		dev_info(dev, "geom[%s] dsim: link=%08x/%08x mipi=%08x dphy=%08x transfers=%08x resol=%08x vporch=%08x hporch=%08x config=%08x cprs=%08x slice=%08x/%08x cmd=%08x te=%08x/%08x cmd_status=%08x\n",
			 why, GR(priv->dsim, 0x08), GR(priv->dsim, 0x0c), GR(priv->dsim, 0x18),
			 GR(priv->dsim, 0x1c), GR(priv->dsim, 0x30), GR(priv->dsim, 0x3c),
			 GR(priv->dsim, 0x40), GR(priv->dsim, 0x44), GR(priv->dsim, 0x4c),
			 GR(priv->dsim, 0x74), GR(priv->dsim, 0x78), GR(priv->dsim, 0x7c),
			 GR(priv->dsim, 0x80), GR(priv->dsim, 0x84), GR(priv->dsim, 0x88),
			 GR(priv->dsim, 0x8c));
	if (level < 2)
		return;
	dev_info(dev, "geom[%s] decon shadow: global=%08x scaled=%08x splitter=%08x outfifo=%08x/%08x bg_size=%08x path2=%08x\n",
		 why, GR(d, S9P_GEOM_SHD + 0x0000), GR(d, S9P_GEOM_SHD + 0x00a0),
		 GR(d, S9P_GEOM_SHD + 0x0100), GR(d, S9P_GEOM_SHD + 0x0120),
		 GR(d, S9P_GEOM_SHD + 0x0124), GR(d, S9P_GEOM_SHD + 0x0200),
		 GR(d, S9P_GEOM_SHD + 0x0230));
	/* COMP_OUTIF_PATH: 0x10 = DSC encoder 0, 0x20 = encoder 1 (dpu_9810 decon.h) */
	for (b = 0; b < 2; b++) {
		void __iomem *s = d + S9P_GEOM_DSC(b);

		if (!(path2 & (0x10 << b)))
			continue;
		dev_info(dev, "geom[%s] dsc%d: ctrl0=%08x ctrl3=%08x pps00=%08x pps04(height)=%08x pps08(width|slice_h)=%08x pps12(slice_w)=%08x\n",
			 why, b, GR(s, 0x00), GR(s, 0x0c), GR(s, 0x20), GR(s, 0x24), GR(s, 0x28), GR(s, 0x2c));
	}
}

/* one line whenever what a commit scans out changes shape */
static void s9p_decon_log_fb(struct s9p_decon *priv, struct drm_plane_state *st)
{
	struct drm_framebuffer *fb = st->fb;
	u32 sig[ARRAY_SIZE(priv->geom_last)];

	if (!s9p_geom_log || !fb)
		return;
	sig[0] = fb->width;
	sig[1] = fb->height;
	sig[2] = fb->pitches[0];
	sig[3] = fb->offsets[0];
	sig[4] = fb->format->format;
	sig[5] = (u32)fb->modifier;
	sig[6] = st->src_x >> 16;
	sig[7] = st->src_y >> 16;
	sig[8] = st->src_w >> 16;
	sig[9] = st->src_h >> 16;
	sig[10] = st->crtc_x;
	sig[11] = st->crtc_y;
	sig[12] = st->crtc_w;
	sig[13] = st->crtc_h;
	if (!memcmp(sig, priv->geom_last, sizeof(sig)))
		return;
	memcpy(priv->geom_last, sig, sizeof(sig));
	drm_info(&priv->drm,
		 "geom: fb %ux%u pitch %u offset %u %p4cc mod %llx, src %u,%u %ux%u, crtc %d,%d %ux%u; idma src_size %08x img_size %08x\n",
		 fb->width, fb->height, fb->pitches[0], fb->offsets[0], &fb->format->format,
		 fb->modifier, sig[6], sig[7], sig[8], sig[9], st->crtc_x, st->crtc_y,
		 st->crtc_w, st->crtc_h,
		 GR(s9p_chan_regs(priv, priv->chan), IDMA_SRC_SIZE),
		 GR(s9p_chan_regs(priv, priv->chan), IDMA_IMG_SIZE));
}

static int s9p_dump_geometry_set(const char *val, const struct kernel_param *kp)
{
	int level, ret = kstrtoint(val, 0, &level);

	if (ret)
		return ret;
	mutex_lock(&s9p_geom_lock);
	if (s9p_geom_dev && level > 0)
		s9p_decon_dump_geometry(s9p_geom_dev, "sysfs", level);
	mutex_unlock(&s9p_geom_lock);
	return 0;
}

static const struct kernel_param_ops s9p_dump_geometry_ops = {
	.set = s9p_dump_geometry_set,
};
module_param_cb(dump_geometry, &s9p_dump_geometry_ops, NULL, 0200);
MODULE_PARM_DESC(dump_geometry, "write 1 (or 2: + shadow bank and DSC) to log the scanout registers now (Starbite)");

/* One DSI write from the command FIFO: 1 byte DCS short, 2 bytes DCS short
 * with parameter, longer a DCS long write through the payload FIFO (the
 * vendor's dsim_long_data_wr: little-endian words, then the header). */
static int s9p_decon_dsi_write(struct s9p_decon *priv, const u8 *buf, unsigned int len)
{
	unsigned int i, j;
	u32 v, hdr;

	if (!priv->dsim || !len || len > 64)
		return -EINVAL;
	writel_relaxed(DSIM_INTSRC_PH_FIFO_EMPTY, priv->dsim + DSIM_INTSRC);
	if (len == 1) {
		hdr = DSIM_PKTHDR_ID(MIPI_DCS_SHORT_WRITE) | DSIM_PKTHDR_DATA0(buf[0]);
	} else if (len == 2) {
		hdr = DSIM_PKTHDR_ID(0x15) | DSIM_PKTHDR_DATA0(buf[0]) | DSIM_PKTHDR_DATA1(buf[1]);
	} else {
		for (i = 0; i < len; i += 4) {
			for (v = 0, j = 0; j < 4 && i + j < len; j++)
				v |= (u32)buf[i + j] << (8 * j);
			writel_relaxed(v, priv->dsim + DSIM_PAYLOAD);
		}
		hdr = DSIM_PKTHDR_ID(0x39) | DSIM_PKTHDR_DATA0(len & 0xff) |
		      DSIM_PKTHDR_DATA1(len >> 8);
	}
	wmb();
	writel_relaxed(hdr, priv->dsim + DSIM_PKTHDR);
	if (readl_poll_timeout_atomic(priv->dsim + DSIM_INTSRC, v,
				      v & DSIM_INTSRC_PH_FIFO_EMPTY, 10, 20000)) {
		dev_warn(priv->drm.dev, "panel did not take DSI write %02x+%u (INTSRC=%08x)\n",
			 buf[0], len, v);
		return -ETIMEDOUT;
	}
	return 0;
}

static bool s9p_panel_mask_off;

static void s9p_panel_mask_apply(struct s9p_decon *priv)
{
	static const u8 key_on[] = { 0xf0, 0x5a, 0x5a }, mask_off[] = { 0x7a, 0x00 },
			key_off[] = { 0xf0, 0xa5, 0xa5 };
	int ret;

	if (!READ_ONCE(s9p_panel_mask_off) || !s9p_panel_dcs)
		return;
	mutex_lock(&s9p_geom_lock);
	ret = s9p_decon_dsi_write(priv, key_on, sizeof(key_on));
	if (!ret)
		ret = s9p_decon_dsi_write(priv, mask_off, sizeof(mask_off));
	s9p_decon_dsi_write(priv, key_off, sizeof(key_off));
	mutex_unlock(&s9p_geom_lock);
	dev_info(priv->drm.dev, "panel self mask off (7A 00): %d\n", ret);
}

static int s9p_panel_mask_off_set(const char *val, const struct kernel_param *kp)
{
	int ret = param_set_bool(val, kp);

	if (!ret && s9p_panel_mask_off && s9p_geom_dev && s9p_geom_dev->enabled)
		s9p_panel_mask_apply(s9p_geom_dev);
	return ret;
}

static const struct kernel_param_ops s9p_panel_mask_off_ops = {
	.set = s9p_panel_mask_off_set,
	.get = param_get_bool,
};
module_param_cb(panel_mask_off, &s9p_panel_mask_off_ops, &s9p_panel_mask_off, 0644);
MODULE_PARM_DESC(panel_mask_off, "send the S6E3HA8 self-mask disable (F0 5A 5A, 7A 00, F0 A5 A5) at every enable (Starbite black-bar test)");

#undef GR

'''


def patch_port(work):
    w = Path(work)

    # 6. s9p-decon early_event
    decon = 'src/gpu/drm/s9p/s9p-decon.c'
    s = (w / decon).read_text()
    if 's9p_early_event' not in s:
        for a in ('#include <linux/iopoll.h>', 'module_param_named(panel_sleep, s9p_panel_sleep, bool, 0644);\n',
                  '\tint irq_frame_start;\n', DECON_IRQ_OLD, DECON_DISABLE_OLD, DECON_UPDATE_OLD,
                  'static irqreturn_t s9p_decon_irq(int irq, void *data)\n'):
            if s.count(a) != 1:
                sys.exit(f'{decon}: anchor {a[:60]!r} found {s.count(a)}x, expected 1')
        anchor = 'MODULE_PARM_DESC(panel_sleep, "also put the panel into sleep mode when blanking");\n'
        if s.count(anchor) != 1:
            sys.exit(f'{decon}: panel_sleep description anchor not found once')
        s = s.replace(anchor, anchor + DECON_PARAM, 1)
        s = s.replace('\tint irq_frame_start;\n',
                      '\tint irq_frame_start;\n'
                      '\t/* Starbite early_event: the flip waiting for its latching TE */\n'
                      '\tstruct drm_pending_vblank_event *flip_event;\n', 1)
        s = s.replace('static irqreturn_t s9p_decon_irq(int irq, void *data)\n',
                      DECON_FLUSH + 'static irqreturn_t s9p_decon_irq(int irq, void *data)\n', 1)
        s = s.replace(DECON_IRQ_OLD, DECON_IRQ_NEW, 1)
        s = s.replace(DECON_DISABLE_OLD, DECON_DISABLE_NEW, 1)
        s = s.replace(DECON_UPDATE_OLD, DECON_UPDATE_NEW, 1)
        (w / decon).write_text(s)
        print(f'{decon}: early_event applied')
    else:
        print(f'{decon}: early_event already there')

    # 6b. s9p-decon: drm_name= for ChromeOS boots (2026-11-05). minigbm picks a buffer backend by the DRM
    # driver's name and its "exynos" backend calls Exynos GEM ioctls this driver lacks; Mesa's kmsro needs
    # "exynos" to pair the display with Panfrost, so the name changes only when the boot asks for it
    # (s9p_decon.drm_name=vkms: minigbm's dumb-buffer backend, for ChromeOS with software GL).
    edit(w, decon,
         'static const struct drm_driver s9p_drm_driver = {\n',
         'static char *s9p_drm_name;\n'
         'module_param_named(drm_name, s9p_drm_name, charp, 0444);\n'
         'MODULE_PARM_DESC(drm_name, "DRM driver name to report instead of exynos (ChromeOS boots)");\n\n'
         'static struct drm_driver s9p_drm_driver = {\n',
         's9p_drm_name')
    edit(w, decon,
         '\tpriv = devm_drm_dev_alloc(&pdev->dev, &s9p_drm_driver,\n',
         '\tif (s9p_drm_name && *s9p_drm_name)\n'
         '\t\ts9p_drm_driver.name = s9p_drm_name;\n'
         '\tpriv = devm_drm_dev_alloc(&pdev->dev, &s9p_drm_driver,\n',
         's9p_drm_driver.name = s9p_drm_name')

    # 6c. s9p-decon: honour the framebuffer's pitch (2026-11-05). The driver kept sboot's fetch setup
    # (IDMA_SRC_SIZE 0x0b9005a0: 2960 rows of a 1440-pixel-wide buffer) and only moved the base address,
    # so every client had to hand it 5760-byte rows. Mesa's kms_swrast (ChromiumOS, boot 8) pads rows to
    # 64-pixel tiles, 1472 px = 5888 bytes: the picture sheared 32 px per row (near-horizontal lines every
    # ~46 rows, the text smeared into one tilted band). SRC_SIZE's low half is the buffer's full width in
    # pixels (IMG_SIZE stays the 1440x2960 crop); it sits in the same shadowed IDMA bank as the base
    # address, so it latches with the same update request. The plane check refuses what the fetch can't do.
    edit(w, decon,
         '\tu32 inherited_path1;\n',
         '\tu32 inherited_path1;\n'
         '\tu32 src_size;\t\t/* Starbite stride: IDMA_SRC_SIZE as programmed */\n',
         'Starbite stride: IDMA_SRC_SIZE as programmed')
    edit(w, decon,
         'static void s9p_decon_program_address(struct s9p_decon *priv, dma_addr_t addr)\n'
         '{\n'
         '\tvoid __iomem *ch = s9p_chan_regs(priv, priv->chan);\n',
         'static void s9p_decon_program_address(struct s9p_decon *priv, dma_addr_t addr,\n'
         '\t\t\t\t      unsigned int pitch)\n'
         '{\n'
         '\tvoid __iomem *ch = s9p_chan_regs(priv, priv->chan);\n'
         '\tu32 src = (priv->src_size & 0xffff0000) | (pitch / 4);\n'
         '\n'
         '\t/* Starbite stride: the buffer\'s row length, in pixels; latched with the address */\n'
         '\tif (src != priv->src_size) {\n'
         '\t\twritel_relaxed(src, ch + IDMA_SRC_SIZE);\n'
         '\t\tdrm_dbg_kms(&priv->drm, "stride %u -> %u px\\n",\n'
         '\t\t\t    priv->src_size & 0xffff, pitch / 4);\n'
         '\t\tpriv->src_size = src;\n'
         '\t}\n',
         'Starbite stride: the buffer')
    edit(w, decon,
         '\tobj = to_drm_gem_dma_obj(plane_state->fb->obj[0]);\n'
         '\ts9p_wait_shadow_idle(priv);\n'
         '\ts9p_decon_program_address(priv, obj->dma_addr);\n',
         '\tobj = to_drm_gem_dma_obj(plane_state->fb->obj[0]);\n'
         '\ts9p_wait_shadow_idle(priv);\n'
         '\ts9p_decon_program_address(priv, obj->dma_addr + plane_state->fb->offsets[0],\n'
         '\t\t\t\t  plane_state->fb->pitches[0]);\n',
         'obj->dma_addr + plane_state->fb->offsets[0]')
    edit(w, decon,
         '\t\t\ts9p_decon_program_address(priv, obj->dma_addr);\n',
         '\t\t\ts9p_decon_program_address(priv, obj->dma_addr + fb->offsets[0],\n'
         '\t\t\t\t\t\t  fb->pitches[0]);\n',
         'obj->dma_addr + fb->offsets[0]', count=2)
    edit(w, decon,
         '\ts9p_decon_program_address(priv, priv->black_dma);\n',
         '\ts9p_decon_program_address(priv, priv->black_dma, PANEL_W * 4);\n',
         'priv->black_dma, PANEL_W * 4')
    edit(w, decon,
         '\tsrc = readl_relaxed(s9p_chan_regs(priv, priv->chan) + IDMA_SRC_SIZE);\n',
         '\tsrc = readl_relaxed(s9p_chan_regs(priv, priv->chan) + IDMA_SRC_SIZE);\n'
         '\tpriv->src_size = src;\t/* Starbite stride: sboot\'s geometry */\n',
         "Starbite stride: sboot's geometry")
    edit(w, decon,
         'static const struct drm_simple_display_pipe_funcs s9p_pipe_funcs = {\n',
         '/* Starbite stride: what the inherited fetch can scan (linear XRGB8888, full panel, no crop) */\n'
         'static int s9p_decon_pipe_check(struct drm_simple_display_pipe *pipe,\n'
         '\t\t\t\tstruct drm_plane_state *plane_state,\n'
         '\t\t\t\tstruct drm_crtc_state *crtc_state)\n'
         '{\n'
         '\tstruct drm_framebuffer *fb = plane_state->fb;\n'
         '\n'
         '\tif (!fb)\n'
         '\t\treturn 0;\n'
         '\tif (fb->pitches[0] % 4 || fb->pitches[0] / 4 < PANEL_W ||\n'
         '\t    fb->pitches[0] / 4 > 0x3fff || fb->offsets[0] % 16 ||\n'
         '\t    plane_state->src_x || plane_state->src_y)\n'
         '\t\treturn -EINVAL;\n'
         '\treturn 0;\n'
         '}\n'
         '\n'
         'static const struct drm_simple_display_pipe_funcs s9p_pipe_funcs = {\n'
         '\t.check = s9p_decon_pipe_check,\n',
         's9p_decon_pipe_check')

    # 6d. s9p-decon: accept ARGB8888 framebuffers too (2026-11-05). ChromiumOS's compositor buffers are
    # AR24; with XRGB8888 alone every ADDFB2 after Chrome's first modeset failed ("Unsupported pixel format
    # AR24") and the panel kept the copied boot logo. Same layout; the fetch channel stays XRGB (sboot's
    # IDMA_IN_CON), so the alpha byte is ignored and scanout is unchanged. XRGB stays first (fbdev).
    edit(w, decon,
         'static const u32 s9p_formats[] = {\n\tDRM_FORMAT_XRGB8888,\n};\n',
         'static const u32 s9p_formats[] = {\n\tDRM_FORMAT_XRGB8888,\n'
         '\tDRM_FORMAT_ARGB8888,\t/* Starbite: ChromiumOS, alpha ignored by the XRGB fetch */\n};\n',
         'Starbite: ChromiumOS, alpha ignored')

    # 6e. s9p-decon: geometry diagnostics and the self-mask test (2026-11-05). Black bars at the top and
    # bottom of the panel under Phosh/ChromeOS; nothing the driver programs has a size in it (window,
    # IDMA SRC/IMG are sboot's 1440x2960, logged at probe), so the dumps print the rest of the path
    # (DECON blender/splitter/OUTFIFO, DSIM resolution/transfers/porches, DSC PPS on request) and every
    # framebuffer geometry change, and panel_mask_off tests the S6E3HA8 self mask (rows 0-149 and
    # 2810-2959). Analysis and the phone checklist: docs/LINUX-REBUILD-PLAN.md "Display: black bars".
    edit(w, decon,
         '\tu32 src_size;\t\t/* Starbite stride: IDMA_SRC_SIZE as programmed */\n',
         '\tu32 src_size;\t\t/* Starbite stride: IDMA_SRC_SIZE as programmed */\n'
         '\tu32 geom_last[14];\t/* Starbite geometry: last logged fb/plane shape */\n',
         'Starbite geometry: last logged fb/plane shape')
    edit(w, decon,
         '/* ----------------------------------------------------------------- pipe */\n',
         DECON_GEOM + '/* ----------------------------------------------------------------- pipe */\n',
         'static void s9p_decon_dump_geometry(')
    edit(w, decon,
         '\ts9p_panel_up(priv);\n\tobj = to_drm_gem_dma_obj(plane_state->fb->obj[0]);\n',
         '\ts9p_panel_up(priv);\n'
         '\ts9p_panel_mask_apply(priv);\t/* Starbite geometry: off unless panel_mask_off */\n'
         '\tobj = to_drm_gem_dma_obj(plane_state->fb->obj[0]);\n',
         's9p_panel_mask_apply(priv);\t/* Starbite geometry')
    edit(w, decon,
         '\ts9p_frame_start_irq_set(priv, true);\n\tpriv->enabled = true;\n\tdrm_crtc_vblank_on(&pipe->crtc);\n',
         '\ts9p_frame_start_irq_set(priv, true);\n\tpriv->enabled = true;\n\tdrm_crtc_vblank_on(&pipe->crtc);\n'
         '\ts9p_decon_log_fb(priv, plane_state);\n'
         '\tif (s9p_geom_log) {\t/* Starbite geometry: after every unblank */\n'
         '\t\tmutex_lock(&s9p_geom_lock);\n'
         '\t\ts9p_decon_dump_geometry(priv, "enable", 1);\n'
         '\t\tmutex_unlock(&s9p_geom_lock);\n'
         '\t}\n',
         'Starbite geometry: after every unblank')
    edit(w, decon,
         '\tstruct drm_framebuffer *fb = pipe->plane.state->fb;\n\n'
         '\t/*\n\t * Starbite early_event: program',
         '\tstruct drm_framebuffer *fb = pipe->plane.state->fb;\n\n'
         '\ts9p_decon_log_fb(priv, pipe->plane.state);\t/* Starbite geometry */\n'
         '\t/*\n\t * Starbite early_event: program',
         's9p_decon_log_fb(priv, pipe->plane.state);\t/* Starbite geometry */')
    edit(w, decon,
         '\tpriv->dsim = devm_ioremap(&pdev->dev, DSIM_PHYS, 0x100);\n',
         '\tpriv->dsim = devm_ioremap(&pdev->dev, DSIM_PHYS, 0x100);\n'
         '\tif (s9p_geom_log)\t/* Starbite geometry: what sboot left */\n'
         '\t\ts9p_decon_dump_geometry(priv, "probe", 1);\n',
         'Starbite geometry: what sboot left')
    edit(w, decon,
         '\tdrm_info(drm, "DECON scanout %ux%u on window %u\\n",\n',
         '\ts9p_geom_dev = priv;\t/* Starbite geometry: sysfs dump and panel_mask_off */\n'
         '\tdrm_info(drm, "DECON scanout %ux%u on window %u\\n",\n',
         's9p_geom_dev = priv;')

    # 7. s9p-g3d: rail check, 950 mV at 260 MHz
    g3d = 'src/soc/s9p-g3d.c'
    edit(w, g3d,
         '\t{ 260000, 160,  4, 2, 0x58 },\t/* sboot\'s clock, 850 mV */\n',
         '\t{ 260000, 160,  4, 2, 0x68 },\t/* sboot\'s clock, 950 mV (Starbite: was 850 mV;\n'
         '\t\t\t\t\t * NOTES.md: jobs need 950 mV, lower faulted) */\n',
         "sboot's clock, 950 mV (Starbite")
    edit(w, g3d,
         'static bool g3d_step_volt;\n',
         '''static bool g3d_step_volt;

/*
 * Starbite rail check (plan 1.3.2): with step_volt off the rail has to read
 * 950 mV whenever the clock moves and after every power-up. A field reading
 * of 850 mV at 260 MHz conflicted with that; whatever lowers it, it is put
 * back here and counted, so /sys/module/s9p_g3d/parameters/rail_fixups says
 * whether it happens at all. A read or write that fails is counted apart in
 * rail_fixup_errors (and warned with its error code): then the rail was not
 * checked or not restored, and the GPU may run the next clock below 950 mV.
 */
static bool g3d_rail_check = true;
module_param_named(rail_check, g3d_rail_check, bool, 0644);
MODULE_PARM_DESC(rail_check, "with step_volt off, read BUCK6 back before each clock change and restore 950 mV (Starbite)");
static unsigned int g3d_rail_fixups;
module_param_named(rail_fixups, g3d_rail_fixups, uint, 0444);
MODULE_PARM_DESC(rail_fixups, "times BUCK6 read below 950 mV and was restored");
static unsigned int g3d_rail_fixup_errors;
module_param_named(rail_fixup_errors, g3d_rail_fixup_errors, uint, 0444);
MODULE_PARM_DESC(rail_fixup_errors, "times BUCK6 could not be read, or read low and could not be restored");

/* caller holds g3d_lock */
static void s9p_g3d_rail_verify(unsigned int khz)
{
	u8 value;
	int err;

	if (g3d_step_volt || !g3d_rail_set || !READ_ONCE(g3d_rail_check))
		return;
	err = s9p_acpm_pmic_read(G3D_BUCK6_REG, &value);
	if (err) {
		g3d_rail_fixup_errors++;
		pr_warn_ratelimited("s9p-g3d: BUCK6 read failed (%d) before %u kHz, rail not checked\\n",
				    err, khz);
		return;
	}
	if (value >= G3D_VSEL_950MV)
		return;
	err = s9p_acpm_pmic_write(G3D_BUCK6_REG, G3D_VSEL_950MV);
	if (err) {
		g3d_rail_fixup_errors++;
		pr_warn_ratelimited("s9p-g3d: BUCK6 read %u uV before %u kHz, restoring 950 mV failed (%d)\\n",
				    300000u + value * 6250u, khz, err);
		return;
	}
	udelay(200);
	g3d_rail_fixups++;
	pr_warn_ratelimited("s9p-g3d: BUCK6 read %u uV before %u kHz, restored 950 mV\\n",
			    300000u + value * 6250u, khz);
}
''',
         'g3d_rail_check')
    edit(w, g3d,
         '''	bool up = g3d_cur_khz < st->khz;

''',
         '''	bool up = g3d_cur_khz < st->khz;

	s9p_g3d_rail_verify(st->khz);	/* Starbite */
''',
         's9p_g3d_rail_verify(st->khz);')
    edit(w, g3d,
         '''	g3d_domain_powered = powered;
	if (powered && g3d_con0 && g3d_rail_set) {
''',
         '''	g3d_domain_powered = powered;
	if (powered && g3d_con0 && g3d_rail_set) {
		s9p_g3d_rail_verify(g3d_cur_khz);	/* Starbite: after power-up */
''',
         'Starbite: after power-up')

    # 8. s9p-cpufreq: 5 ms transition delay, a boot parameter
    cpf = 'src/cpufreq/s9p-cpufreq.c'
    edit(w, cpf,
         '\tpolicy->transition_delay_us = 20 * USEC_PER_MSEC;\n',
         '\tpolicy->transition_delay_us = s9p_transition_delay_us;\t/* Starbite: was 20 ms */\n',
         's9p_transition_delay_us;')
    edit(w, cpf,
         'static int s9p_init(struct cpufreq_policy *policy)\n',
         '''/*
 * Starbite (plan 1.3.5): 20 ms between frequency decisions left QML shells
 * ramping a frame or two behind the load. 5 ms; schedutil's rate_limit_us
 * starts from it (cpufreq/policyN/schedutil/rate_limit_us changes it at run
 * time).
 */
static unsigned int s9p_transition_delay_us = 5000;
module_param_named(transition_delay_us, s9p_transition_delay_us, uint, 0444);
MODULE_PARM_DESC(transition_delay_us, "cpufreq transition delay (us) given to both policies");

static int s9p_init(struct cpufreq_policy *policy)
''',
         'static unsigned int s9p_transition_delay_us')

    # 9. build71.sh
    b71 = 'scripts/build/build71.sh'
    p = w / b71
    s = p.read_text()
    if 'star2lte' in s:
        s = s.replace('star2lte', 'starlte')
        print(f'{b71}: star2lte -> starlte')
    p.write_text(s)
    if 'exynos9810-starlte.dts' not in s:
        sys.exit(f'{b71}: no exynos9810-starlte.dts after the rename')
    edit(w, b71,
         'install -m644 "$PROJ/branding/shero_logo.ppm" drivers/video/logo/logo_shero_clut224.ppm\n',
         '# Starbite: the Shero logo is not vendored (CONFIG_LOGO is off in starbite.config)\n'
         '[ ! -f "$PROJ/branding/shero_logo.ppm" ] || install -m644 "$PROJ/branding/shero_logo.ppm" drivers/video/logo/logo_shero_clut224.ppm\n',
         'the Shero logo is not vendored')
    # the Starbite loader takes Image + DTB from its store (room up to OS_RD,
    # checked by check-kernel.py image); the port's Samsung boot.img is a
    # by-product here, so its BOOT-partition limit only warns
    edit(w, b71,
         '[ "$SIZE" -le 57671680 ] || { echo "TOO BIG"; sync; exit 1; }\n',
         '[ "$SIZE" -le 57671680 ] || echo "boot.img too big for BOOT (Starbite: not used, warning only)"\n',
         'Starbite: not used, warning only')
    edit(w, b71,
         '''for extra in "$@"; do
	cfg $extra
done
make -s O=out olddefconfig
''',
         '''for extra in "$@"; do
	cfg $extra
done
make -s O=out olddefconfig
# Starbite: config fragment(s) on top, then every requested value is checked
if [ -n "${STARBITE_CONFIG:-}" ]; then
	./scripts/kconfig/merge_config.sh -m -O out out/.config $STARBITE_CONFIG || exit 1
	make -s O=out olddefconfig || exit 1
	python3 "$STARBITE_CHECK" config out/.config $STARBITE_CONFIG || exit 1
fi
''',
         'STARBITE_CONFIG')


ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
ap.add_argument('--tree', help='mainline source tree')
ap.add_argument('--port', help='work copy of tools/linux/port')
ap.add_argument('legacy', nargs='?', help=argparse.SUPPRESS)
a = ap.parse_args()
if a.legacy:
    a.tree = a.legacy
if not a.tree and not a.port:
    ap.error('nothing to patch: --tree and/or --port')
if a.port:
    if Path(a.port).resolve() == (Path(__file__).resolve().parent / 'port'):
        sys.exit('refusing to patch the vendored port itself: give the work copy')
    patch_port(a.port)
if a.tree:
    patch_tree(a.tree)
