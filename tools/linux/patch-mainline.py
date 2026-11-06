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
  11.  arm64 __delay: no WFE with interrupts off. The event stream wakes it far
       too rarely on this SoC: panic=10's mdelay() countdown ran ~12x long
       (plan 7.6).

Port (work copy):
  6.   s9p-decon: flip event at the TE edge that latches the new buffer
       (module param early_event, default on; plan 1.3.1, cause C).
       6b-6d: drm_name=, the framebuffer's pitch/offset, ARGB8888.
       6e: geometry dumps (geom_log, dump_geometry) and the panel_mask_off
       test for the black bars (plan, "Display: black bars").
       6f: the S6E3HA8 self mask as Samsung handles it: panel_mask=1 (default)
       re-sends the rounded-corner image (patches/s6e3ha8-star-self-mask.hex)
       and the enable after every sleep-out, 2 = mask off, 0 = leave (plan 6.6).
       6g: the panic screen: drm_panic's get_scanout_buffer/panic_flush hooks. The
       driver draws a Starbite banner and the newest kernel log (TER16x32) into its
       black frame within panic_budget_ms (500), then makes it the scanout with the
       flip's register writes; panic_banner= (plan 7).
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
  10.  s9p-clkgate: restart by INFORM2 + PMU SWRESET and power-off by PS_HOLD, as
       Samsung's exynos-reboot.c (the only restart handler was s3c2410_wdt's:
       reboot -f hung, panic=10 reset minutes late); waits on the counter; at a
       panic, cluster 1's watchdog armed for panic_timeout + 5 s as a backstop
       (panic_wdt=); s9p_clkgate.pmu_reset=0 = off.

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

    # 11. arm64 __delay: no WFE with interrupts off (2026-11-06, the first panics on the phone).
    #     panic=10's countdown is mdelay(100) x 100 with interrupts off and the other CPUs parked;
    #     __delay waits in WFE for the timer event stream, and on this SoC that wait ran about 12x
    #     long (old kernel: reset ~2 min after the panic; new kernel: the loader's 60 s watchdog won).
    edit(src, 'arch/arm64/lib/delay.c',
         '#include <linux/delay.h>\n',
         '#include <linux/delay.h>\n'
         '#include <linux/irqflags.h>\t/* Starbite: no WFE with interrupts off */\n',
         'Starbite: no WFE with interrupts off')
    edit(src, 'arch/arm64/lib/delay.c',
         '\t} else \tif (arch_timer_evtstrm_available()) {\n',
         '\t} else if (arch_timer_evtstrm_available() && !irqs_disabled()) {\n'
         '\t\t/*\n'
         '\t\t * Starbite (tools/linux/patch-mainline.py step 11): with interrupts\n'
         '\t\t * off only the event stream ends this WFE, and on the Galaxy S9\n'
         '\t\t * (Exynos 9810) the delay then ran about 12x long: panic=10 reset after\n'
         '\t\t * two minutes. With interrupts off the loop below spins on the counter.\n'
         '\t\t */\n',
         'Starbite (tools/linux/patch-mainline.py step 11)')


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


# 6f. The S6E3HA8 self mask, Samsung's way (docs/LINUX-REBUILD-PLAN.md 6.6).
SELF_MASK_HEX = Path(__file__).resolve().parent / 'patches' / 's6e3ha8-star-self-mask.hex'
SELF_MASK_LEN = 3552
SELF_MASK_SHA256 = 'bc4c1d6cfd4649cb802717e1f4677ff08e36ace5ee21430a88511c7727c1b440'


def self_mask_c_array():
    import hashlib
    b = bytes.fromhex(''.join(l.strip() for l in SELF_MASK_HEX.read_text().splitlines()
                              if l.strip() and not l.startswith('#')))
    if len(b) != SELF_MASK_LEN or hashlib.sha256(b).hexdigest() != SELF_MASK_SHA256:
        sys.exit(f'{SELF_MASK_HEX}: not the G960F STAR_SELF_MASK_IMG ({len(b)} bytes)')
    return '\n'.join('\t' + ' '.join(f'0x{x:02x},' for x in b[i:i + 12]) for i in range(0, len(b), 12))


DECON_MASK_PROTO = '''/* Starbite self mask (6f): Samsung's aod_init_panel, between sleep-out and display-on */
static void s9p_panel_mask_init(struct s9p_decon *priv);

'''

DECON_MASK = r'''
/*
 * Starbite self mask (docs/LINUX-REBUILD-PLAN.md 6.6). The S6E3HA8 darkens
 * panel rows 0-149 and 2810-2959 through a run-length coded mask held in its
 * side RAM: Samsung's rounded corners (0xff = black in the corners, 0 = pass
 * through in the middle). S-Boot uploads that image and enables the mask
 * before Linux starts (sboot.bin G960FXXUHFVG6 carries the same 3552 bytes and
 * the same enable), and Samsung's kernel re-sends both after every panel init
 * (panel_drv.c __panel_seq_init -> aod_drv.c aod_init_panel: SELF_MASK_IMG_SEQ,
 * then SELF_MASK_ENA_SEQ, before PANEL_DISPLAY_ON_SEQ). This driver sleep-cycles
 * the panel on every blank and never re-sent them; a mask that is on with a
 * lost or corrupted image is a black band of 150 rows at each end.
 *
 *   panel_mask=1 (default)  restore, as Samsung: F0 5A 5A, 75 10 (side-RAM path:
 *                           mask), 4C/5C + the image in 496-byte DCS long writes,
 *                           F0 A5 A5; then F0 5A 5A, 7A 01 0000 0095 0AFA 0B8F
 *                           090F 00000000, F0 A5 A5. Rounded corners.
 *   panel_mask=2            off (SELF_MASK_DIS: F0 5A 5A, 7A 00, F0 A5 A5).
 *                           Square corners, nothing masked.
 *   panel_mask=0            leave the panel alone (the driver before 6f).
 * Sent after every sleep-out (before display-on), and at once when written at
 * run time with the display on. panel_mask_off=1 (6e) still wins: then 6e's
 * 7A 00 is sent right after display-on and this does nothing. A restore that
 * fails part way falls back to 7A 00, so a half-written image never stays on.
 */
#define S9P_MASK_LEAVE		0
#define S9P_MASK_RESTORE	1
#define S9P_MASK_OFF		2
#define S9P_SRAM_ALIGN		16	/* panel.c SRAM_BYTE_ALIGN */
#define S9P_SRAM_CHUNK		511	/* panel.c panel_dsi_write_mem: min(remained, 511) */

/* STAR_SELF_MASK_IMG (s6e3ha8_self_mask_img.h, SM-G960F), 300 rows x 1440 */
static const u8 s9p_self_mask_img[@LEN@] = {
@IMG@
};

static int s9p_panel_mask = S9P_MASK_RESTORE;
static u8 s9p_sram_buf[1 + S9P_SRAM_CHUNK];	/* under s9p_geom_lock */

/* caller holds s9p_geom_lock; panel_dsi_write_mem's chunking */
static int s9p_panel_sram_write(struct s9p_decon *priv, const u8 *img, unsigned int size)
{
	unsigned int done = 0, n;
	int ret;

	while (done < size) {
		n = min(size - done, (unsigned int)S9P_SRAM_CHUNK);
		n -= n % S9P_SRAM_ALIGN;
		if (!n)
			return -EINVAL;
		s9p_sram_buf[0] = done ? 0x5c : 0x4c;	/* WRITE_SIDE_RAM_CONTINUE / _START */
		memcpy(s9p_sram_buf + 1, img + done, n);
		ret = s9p_decon_dsi_write(priv, s9p_sram_buf, n + 1);
		if (ret)
			return ret;
		done += n;
	}
	return 0;
}

/* caller holds s9p_geom_lock */
static void s9p_panel_mask_send(struct s9p_decon *priv, int mode, const char *why)
{
	static const u8 key_on[] = { 0xf0, 0x5a, 0x5a }, key_off[] = { 0xf0, 0xa5, 0xa5 };
	static const u8 sd_path[] = { 0x75, 0x10 };
	static const u8 mask_ena[] = { 0x7a, 0x01, 0x00, 0x00, 0x00, 0x95, 0x0a, 0xfa,
				       0x0b, 0x8f, 0x09, 0x0f, 0x00, 0x00, 0x00, 0x00 };
	static const u8 mask_dis[] = { 0x7a, 0x00 };
	int ret = 0, ret2;

	if (mode == S9P_MASK_RESTORE) {
		/* SELF_MASK_IMG_SEQ */
		ret = s9p_decon_dsi_write(priv, key_on, sizeof(key_on));
		if (!ret)
			ret = s9p_decon_dsi_write(priv, sd_path, sizeof(sd_path));
		if (!ret)
			ret = s9p_panel_sram_write(priv, s9p_self_mask_img, sizeof(s9p_self_mask_img));
		s9p_decon_dsi_write(priv, key_off, sizeof(key_off));
		/* SELF_MASK_ENA_SEQ */
		if (!ret) {
			ret = s9p_decon_dsi_write(priv, key_on, sizeof(key_on));
			if (!ret)
				ret = s9p_decon_dsi_write(priv, mask_ena, sizeof(mask_ena));
			s9p_decon_dsi_write(priv, key_off, sizeof(key_off));
		}
		if (!ret) {
			dev_info(priv->drm.dev, "panel self mask restored (%s): %zu B side RAM, 7A 01 rows 0-149/2810-2959\n",
				 why, sizeof(s9p_self_mask_img));
			return;
		}
		dev_warn(priv->drm.dev, "panel self mask restore failed (%s): %d, turning the mask off\n",
			 why, ret);
	}
	/* SELF_MASK_DIS_SEQ (mode off, or a failed restore) */
	ret2 = s9p_decon_dsi_write(priv, key_on, sizeof(key_on));
	if (!ret2)
		ret2 = s9p_decon_dsi_write(priv, mask_dis, sizeof(mask_dis));
	s9p_decon_dsi_write(priv, key_off, sizeof(key_off));
	dev_info(priv->drm.dev, "panel self mask off (%s, 7A 00): %d\n", why, ret2);
}

static void s9p_panel_mask_init(struct s9p_decon *priv)
{
	int mode = READ_ONCE(s9p_panel_mask);

	if (READ_ONCE(s9p_panel_mask_off) || mode == S9P_MASK_LEAVE ||
	    !priv->dsim || !s9p_panel_dcs)
		return;
	mutex_lock(&s9p_geom_lock);
	s9p_panel_mask_send(priv, mode, "sleep-out");
	mutex_unlock(&s9p_geom_lock);
}

static int s9p_panel_mask_set(const char *val, const struct kernel_param *kp)
{
	int mode, ret = kstrtoint(val, 0, &mode);

	if (ret)
		return ret;
	if (mode < S9P_MASK_LEAVE || mode > S9P_MASK_OFF)
		return -EINVAL;
	WRITE_ONCE(s9p_panel_mask, mode);
	mutex_lock(&s9p_geom_lock);
	if (mode != S9P_MASK_LEAVE && s9p_geom_dev && s9p_geom_dev->enabled &&
	    s9p_geom_dev->dsim && s9p_panel_dcs)
		s9p_panel_mask_send(s9p_geom_dev, mode, "sysfs");
	mutex_unlock(&s9p_geom_lock);
	return 0;
}

static const struct kernel_param_ops s9p_panel_mask_ops = {
	.set = s9p_panel_mask_set,
	.get = param_get_int,
};
module_param_cb(panel_mask, &s9p_panel_mask_ops, &s9p_panel_mask, 0644);
MODULE_PARM_DESC(panel_mask, "S6E3HA8 self mask after every sleep-out: 1 = restore Samsung's rounded-corner image + enable (default), 2 = off (7A 00), 0 = leave (Starbite)");
'''


# 6g. The panic screen (docs/LINUX-REBUILD-PLAN.md, "Panic screen and restart").
DECON_PANIC_INCLUDES = '''#include <linux/iopoll.h>
/* Starbite panic screen (6g) */
#include <drm/drm_fb_dma_helper.h>
#include <drm/drm_panic.h>
#include <linux/font.h>
#include <linux/iosys-map.h>
#include <linux/kmsg_dump.h>
#include <linux/panic.h>
#include <linux/panic_notifier.h>
#include <linux/sched.h>
#include <linux/sched/clock.h>
#include <linux/timekeeping.h>
#include <linux/utsname.h>
'''

DECON_PANIC = r'''/*
 * Starbite panic screen (docs/LINUX-REBUILD-PLAN.md 7). drm_panic (CONFIG_DRM_PANIC)
 * calls the two hooks below from its kmsg dumper at the moment of the panic: after
 * ramoops has saved the record, before panic=10's countdown. CONFIG_DRM_PANIC_DEBUG's
 * debugfs file calls them for a dry run. The driver draws the whole screen itself,
 * within a time budget:
 *
 *  - get_scanout_buffer picks the frame and starts the clock. The frame is the black
 *    frame allocated at probe (1440 x 2960 ARGB8888, 5760-byte rows, write-combined,
 *    always mapped), or without it the plane's buffer if that has a kernel mapping.
 *    drm_panic's own screen gets a one-pixel sink: its kmsg screen walks the whole
 *    printk ring with a quadratic search before the first pixel (seconds with a big
 *    ring, minutes with log_buf_len=4M), and nothing can stop it once started.
 *  - panic_flush draws, then makes the frame the scanout with a flip's register
 *    writes: base address and stride, the window's shadow update request, the trigger
 *    unmasked. There is no lock, no sleep and no wait for the latch. It wakes a
 *    blanked panel: sleep-out, 120 ms counted on the clock (no WFE), display-on. The
 *    panel runs MIPI command mode with the TE-driven hardware trigger left unmasked,
 *    so DECON sends the frame at the next TE and keeps refreshing it until the reset.
 *    DSC is applied in hardware after the blender.
 *  - Budget: panic_budget_ms (default 500), counted from get_scanout_buffer. Past it
 *    the drawing stops where it is, the banner having come first, and the frame is
 *    shown as it is. A slow draw never holds up the restart. Time comes from
 *    ktime_get_mono_fast_ns(), which reads the counter: no interrupt needed.
 *
 * Layout, the loader's crash screen:
 *  - Banner from y 200, 36 px margin, 32 x 64 px cells (TER16x32 at 2x), 42 columns:
 *    "STARBITE: LINUX KERNEL PANIC" (red), the panic message over two rows (white),
 *    CPU / task / uptime and the release (muted), the restart line (gold), a blue rule.
 *  - Below it, y 624..2800 (clear of the rounded corners and the self mask's bands):
 *    the newest kernel log in TER16x32, 85 columns x 68 rows, white. The "Kernel panic
 *    - not syncing" line is gold. The log comes from one read of the ring's newest
 *    16 KiB, wrapped the way drm_panic wraps it.
 *
 *   panic_banner=0     no banner (log only)
 *   panic_budget_ms=N  the drawing budget
 *   echo 1 > /sys/kernel/debug/dri/16030000.decon/drm_panic_plane_0
 *                      dry run ("STARBITE: PANIC SCREEN TEST", gold); the next flip
 *                      replaces it
 */
#ifdef CONFIG_DRM_PANIC
#define S9P_PANIC_X		36	/* the loader's LEFT */
#define S9P_PANIC_TOP		200	/* the loader's TOP */
#define S9P_PANIC_CELL_W	32
#define S9P_PANIC_CELL_H	64
#define S9P_PANIC_ROWS		6
#define S9P_PANIC_RULE_Y	(S9P_PANIC_TOP + S9P_PANIC_ROWS * S9P_PANIC_CELL_H + 16)
#define S9P_PANIC_RULE_H	4
#define S9P_PANIC_LOG_Y		(S9P_PANIC_RULE_Y + 24)
#define S9P_PANIC_LOG_END	(PANEL_H - 160)	/* above the self mask's bottom band (2810..) */
#define S9P_PANIC_SLAB		64	/* rows filled between two budget checks */
/* the loader's crash.c colours */
#define S9P_PANIC_BLACK		0xff000000
#define S9P_PANIC_WHITE		0xffffffff
#define S9P_PANIC_BLUE		0xff2a5bf0
#define S9P_PANIC_MUTED		0xffb1bad3
#define S9P_PANIC_GOLD		0xffffd43b
#define S9P_PANIC_RED		0xffff4d4d

static bool s9p_panic_banner = true;
module_param_named(panic_banner, s9p_panic_banner, bool, 0644);
MODULE_PARM_DESC(panic_banner, "Starbite banner above the kernel log on the panic screen (default on)");

static unsigned int s9p_panic_budget_ms = 500;
module_param_named(panic_budget_ms, s9p_panic_budget_ms, uint, 0644);
MODULE_PARM_DESC(panic_budget_ms, "panic screen: stop drawing after this many ms and show what is there (default 500)");

static const char *s9p_panic_msg;	/* panic()'s message, from the panic notifier */
static char s9p_panic_log[16384];	/* the newest kernel log, read once per draw */

static int s9p_panic_note(struct notifier_block *nb, unsigned long ev, void *data)
{
	s9p_panic_msg = data;
	return NOTIFY_DONE;
}

static struct notifier_block s9p_panic_nb = {
	.notifier_call = s9p_panic_note,
};

struct s9p_panic_ctx {
	u8 *base;
	unsigned int pitch;
	const struct font_desc *font;
	u64 deadline;		/* ktime_get_mono_fast_ns() */
	bool late;		/* the budget ran out */
};

static bool s9p_panic_late(struct s9p_panic_ctx *c)
{
	if (!c->late && ktime_get_mono_fast_ns() > c->deadline)
		c->late = true;
	return c->late;
}

static void s9p_panic_fill(u8 *base, unsigned int pitch, unsigned int x0, unsigned int y0,
			   unsigned int x1, unsigned int y1, u32 c)
{
	unsigned int x, y;

	for (y = y0; y < y1 && y < PANEL_H; y++) {
		u32 *p = (u32 *)(base + (size_t)y * pitch);

		for (x = x0; x < x1 && x < PANEL_W; x++)
			p[x] = c;
	}
}

/* a black rectangle in slabs, checking the budget between them */
static void s9p_panic_clear(struct s9p_panic_ctx *c, unsigned int x0, unsigned int y0,
			    unsigned int x1, unsigned int y1)
{
	unsigned int y;

	for (y = y0; y < y1 && !s9p_panic_late(c); y += S9P_PANIC_SLAB)
		s9p_panic_fill(c->base, c->pitch, x0, y, x1, min(y + S9P_PANIC_SLAB, y1),
			       S9P_PANIC_BLACK);
}

/* at most n characters of s from (x0, y0), each glyph pixel sx x sy, cut at the margin */
static void s9p_panic_text(struct s9p_panic_ctx *c, unsigned int x0, unsigned int y0,
			   unsigned int sx, unsigned int sy, const char *s, unsigned int n, u32 fg)
{
	const struct font_desc *f = c->font;
	unsigned int fp = DIV_ROUND_UP(f->width, 8);
	unsigned int i, gx, gy;

	for (i = 0; i < n && s[i]; i++) {
		const u8 *g = (const u8 *)f->data + (size_t)(unsigned char)s[i] * f->height * fp;
		unsigned int x = x0 + i * f->width * sx;

		if (x + f->width * sx > PANEL_W - S9P_PANIC_X)
			break;
		for (gy = 0; gy < f->height; gy++) {
			u32 *row = (u32 *)(c->base + (size_t)(y0 + gy * sy) * c->pitch);

			for (gx = 0; gx < f->width; gx++) {
				if (!(g[gy * fp + gx / 8] & (0x80 >> (gx % 8))))
					continue;
				if (sx == 1 && sy == 1)
					row[x + gx] = fg;
				else
					s9p_panic_fill(c->base, c->pitch, x + gx * sx, y0 + gy * sy,
						       x + (gx + 1) * sx, y0 + (gy + 1) * sy, fg);
			}
		}
	}
}

/* banner row r: 32 x 64 px cells */
static void s9p_panic_row(struct s9p_panic_ctx *c, unsigned int r, const char *s,
			  unsigned int n, u32 fg)
{
	s9p_panic_text(c, S9P_PANIC_X, S9P_PANIC_TOP + r * S9P_PANIC_CELL_H,
		       max(1u, S9P_PANIC_CELL_W / c->font->width),
		       max(1u, S9P_PANIC_CELL_H / c->font->height), s, n, fg);
}

static void s9p_panic_draw_banner(struct s9p_panic_ctx *c)
{
	bool real = panic_in_progress();
	const char *msg = real ? s9p_panic_msg : "drm_panic test from debugfs, nothing crashed";
	unsigned int cols = (PANEL_W - 2 * S9P_PANIC_X) / S9P_PANIC_CELL_W;
	unsigned int len, n;
	char line[64];

	s9p_panic_row(c, 0, real ? "STARBITE: LINUX KERNEL PANIC" : "STARBITE: PANIC SCREEN TEST",
		      ~0u, real ? S9P_PANIC_RED : S9P_PANIC_GOLD);
	if (msg) {
		len = strnlen(msg, 256);
		while (len && (msg[len - 1] == '\n' || msg[len - 1] == ' '))
			len--;
		n = min(len, cols);
		s9p_panic_row(c, 1, msg, n, S9P_PANIC_WHITE);
		if (len > n)
			s9p_panic_row(c, 2, msg + n, len - n, S9P_PANIC_WHITE);
	}
	snprintf(line, sizeof(line), "CPU %d  %.16s (PID %d)  UP %llu S", raw_smp_processor_id(),
		 current->comm, task_pid_nr(current), local_clock() / NSEC_PER_SEC);
	s9p_panic_row(c, 3, line, ~0u, S9P_PANIC_MUTED);
	snprintf(line, sizeof(line), "LINUX %.48s", init_utsname()->release);
	s9p_panic_row(c, 4, line, ~0u, S9P_PANIC_MUTED);
	if (!real)
		snprintf(line, sizeof(line), "THE NEXT FRAME ENDS THE TEST");
	else if (panic_timeout > 0)
		snprintf(line, sizeof(line), "RESTARTING IN %d S", panic_timeout);
	else if (panic_timeout < 0)
		snprintf(line, sizeof(line), "RESTARTING NOW");
	else
		snprintf(line, sizeof(line), "NO RESTART: VOL DOWN + POWER 7 S");
	s9p_panic_row(c, 5, line, ~0u, S9P_PANIC_GOLD);
	s9p_panic_fill(c->base, c->pitch, S9P_PANIC_X, S9P_PANIC_RULE_Y, PANEL_W - S9P_PANIC_X,
		       S9P_PANIC_RULE_Y + S9P_PANIC_RULE_H, S9P_PANIC_BLUE);
}

/*
 * The newest kernel log, bottom up, as drm_panic's kmsg screen lays it out, but from
 * a single read of the ring's newest 16 KiB (one linear pass over the ring) and with
 * the budget checked per line.
 */
static void s9p_panic_draw_log(struct s9p_panic_ctx *c)
{
	const struct font_desc *f = c->font;
	unsigned int cols = (PANEL_W - 2 * S9P_PANIC_X) / f->width;
	int y = S9P_PANIC_LOG_END - (int)f->height;	/* top of the bottom row */
	struct kmsg_dump_iter iter;
	const char *start, *end;
	size_t len = 0;

	kmsg_dump_rewind(&iter);
	if (!kmsg_dump_get_buffer(&iter, false, s9p_panic_log, sizeof(s9p_panic_log), &len) || !len)
		return;
	end = s9p_panic_log + min(len, sizeof(s9p_panic_log));
	while (end > s9p_panic_log && end[-1] == '\n')
		end--;
	while (end > s9p_panic_log && y >= S9P_PANIC_LOG_Y && !s9p_panic_late(c)) {
		unsigned int n, rows, r;
		u32 fg;

		start = end;
		while (start > s9p_panic_log && start[-1] != '\n')
			start--;
		n = end - start;
		rows = n ? DIV_ROUND_UP(n, cols) : 1;
		fg = strnstr(start, "Kernel panic - not syncing", n) ? S9P_PANIC_GOLD : S9P_PANIC_WHITE;
		for (r = 0; r < rows; r++) {
			int ry = y - (int)(rows - 1 - r) * (int)f->height;

			if (ry >= S9P_PANIC_LOG_Y)
				s9p_panic_text(c, S9P_PANIC_X, ry, 1, 1, start + r * cols,
					       min(cols, n - r * cols), fg);
		}
		y -= rows * f->height;
		end = start > s9p_panic_log ? start - 1 : start;
	}
}

static const struct font_desc *s9p_panic_font(void)
{
	return find_font("TER16x32") ?:
	       get_default_font(PANEL_W - 2 * S9P_PANIC_X, S9P_PANIC_LOG_END - S9P_PANIC_LOG_Y,
				NULL, NULL);
}

/* drm_panic's own screen: one pixel that goes nowhere (the driver draws, see above) */
static void s9p_panic_sink(struct drm_scanout_buffer *sb, unsigned int x, unsigned int y,
			   u32 color)
{
}

/* drm_panic holds mode_config.panic_lock around both hooks */
static int s9p_panic_get_scanout_buffer(struct drm_plane *plane, struct drm_scanout_buffer *sb)
{
	struct s9p_decon *priv = to_s9p_decon(plane->dev);

	priv->panic_t0 = ktime_get_mono_fast_ns();
	if (priv->black_cpu) {
		priv->panic_base = priv->black_cpu;
		priv->panic_dma = priv->black_dma;
		priv->panic_pitch = PANEL_W * 4;
	} else {
		struct drm_framebuffer *fb = plane->state ? plane->state->fb : NULL;
		struct drm_gem_dma_object *obj;

		if (!fb || fb->width < PANEL_W || fb->height < PANEL_H || fb->format->cpp[0] != 4)
			return -ENODEV;
		obj = drm_fb_dma_get_gem_obj(fb, 0);
		if (!obj || !obj->vaddr)
			return -ENODEV;
		priv->panic_base = (u8 *)obj->vaddr + fb->offsets[0];
		priv->panic_dma = obj->dma_addr + fb->offsets[0];
		priv->panic_pitch = fb->pitches[0];
	}
	sb->format = drm_format_info(DRM_FORMAT_ARGB8888);
	sb->width = 1;
	sb->height = 1;
	sb->pitch[0] = 4;
	sb->set_pixel = s9p_panic_sink;
	return 0;
}

/* a busy wait on the counter: no WFE, no timer interrupt */
static void s9p_panic_spin_ms(unsigned int ms)
{
	u64 end = ktime_get_mono_fast_ns() + (u64)ms * NSEC_PER_MSEC;

	while (ktime_get_mono_fast_ns() < end)
		cpu_relax();
}

static void s9p_panic_flush(struct drm_plane *plane)
{
	struct s9p_decon *priv = to_s9p_decon(plane->dev);
	struct s9p_panic_ctx c = {
		.base = priv->panic_base,
		.pitch = priv->panic_pitch,
		.font = s9p_panic_font(),
		.deadline = priv->panic_t0 + (u64)READ_ONCE(s9p_panic_budget_ms) * NSEC_PER_MSEC,
	};

	if (c.font) {
		/* banner first: it says the most, so it is what a short budget keeps */
		s9p_panic_clear(&c, 0, 0, PANEL_W, S9P_PANIC_LOG_Y);
		if (s9p_panic_banner && !c.late)
			s9p_panic_draw_banner(&c);
		s9p_panic_clear(&c, 0, S9P_PANIC_LOG_Y, PANEL_W, PANEL_H);
		if (!c.late)
			s9p_panic_draw_log(&c);
	}
	if (priv->panic_base == priv->black_cpu && !panic_in_progress())
		priv->panic_dirty = true;	/* a test: display-off clears it again */
	wmb();		/* the frame is in memory before DECON is told to fetch it */
	s9p_decon_program_address(priv, priv->panic_dma, priv->panic_pitch);
	if (!priv->enabled) {
		/* blanked (display off, maybe asleep) or never enabled: wake the panel */
		if (s9p_panel_sleep) {
			s9p_decon_dcs(priv, MIPI_DCS_EXIT_SLEEP_MODE);
			s9p_panic_spin_ms(PANEL_SLEEP_DELAY_MS);
		}
		s9p_decon_dcs(priv, MIPI_DCS_SET_DISPLAY_ON);
	}
	priv->panic_ms = (ktime_get_mono_fast_ns() - priv->panic_t0) / NSEC_PER_MSEC;
	priv->panic_late = c.late;
}

static struct drm_plane_helper_funcs s9p_panic_plane_helpers;

/* before drm_dev_register(), which registers drm_panic for planes with the hook */
static void s9p_panic_setup(struct s9p_decon *priv)
{
	struct drm_plane *plane = &priv->pipe.plane;
	const struct font_desc *f = s9p_panic_font();

	s9p_panic_plane_helpers = *plane->helper_private;
	s9p_panic_plane_helpers.get_scanout_buffer = s9p_panic_get_scanout_buffer;
	s9p_panic_plane_helpers.panic_flush = s9p_panic_flush;
	drm_plane_helper_add(plane, &s9p_panic_plane_helpers);
	atomic_notifier_chain_register(&panic_notifier_list, &s9p_panic_nb);
	if (!f)
		return;
	drm_info(&priv->drm, "panic screen: %s, log font %s (%u x %u characters), banner %s, budget %u ms\n",
		 priv->black_cpu ? "own frame" : "the plane's buffer", f->name,
		 (PANEL_W - 2 * S9P_PANIC_X) / f->width, (S9P_PANIC_LOG_END - S9P_PANIC_LOG_Y) / f->height,
		 s9p_panic_banner ? "on" : "off", s9p_panic_budget_ms);
	if (f->height < 32)
		drm_warn(&priv->drm, "panic log font %s is small: CONFIG_FONT_TER16x32 missing?\n",
			 f->name);
}

/* after a debugfs dry run: how long it took (a real panic never gets here) */
static int s9p_panic_last_get(char *buf, const struct kernel_param *kp)
{
	struct s9p_decon *priv = s9p_geom_dev;

	if (!priv)
		return sysfs_emit(buf, "none\n");
	return sysfs_emit(buf, "%u ms%s\n", priv->panic_ms, priv->panic_late ? " (budget ran out)" : "");
}

static const struct kernel_param_ops s9p_panic_last_ops = {
	.get = s9p_panic_last_get,
};
module_param_cb(panic_last_ms, &s9p_panic_last_ops, NULL, 0444);
MODULE_PARM_DESC(panic_last_ms, "time the last panic screen took (debugfs dry run)");
#else
static void s9p_panic_setup(struct s9p_decon *priv) { }
#endif

'''


# 10. Restart and power-off (docs/LINUX-REBUILD-PLAN.md, "Panic screen and restart").
CLKGATE_RESET = r'''
/*
 * Starbite restart and power-off (tools/linux/patch-mainline.py step 10;
 * docs/LINUX-REBUILD-PLAN.md 7).
 *
 * Restart, Samsung's way (exynos-reboot.c exynos_reboot after sec_reboot.c;
 * the Starbite loader, Starbite Recovery and XNU's crash return do the same):
 * INFORM2 = 0x12345678 (a reset, else S-Boot starts LP charging), then PMU
 * SWRESET (+0x400) = 1. This is a warm reset that keeps DRAM, so ramoops
 * survives. INFORM3 is not touched: syscon-reboot-mode has already written it
 * for a clean reboot ("reboot recovery", "reboot download"), and a panic keeps
 * the boot's value (the loader's crash return patches the DTB's recovery value
 * to normal). The handler makes plain writes to a mapping made at boot: no
 * regmap lock, no sleeping, and its waits count on the counter, so nothing
 * here waits for a timer interrupt or a WFE wake-up. Priority 192, before
 * s3c2410_wdt's 128. Verified on the phone: a plain reboot leaves through
 * SWRESET (RST_STAT 0x20000000).
 *
 * Without it the only restart handler is s3c2410_wdt's (PSCI is v0.1 on this
 * firmware, no SYSTEM_RESET; the PMU node carries reboot-mode only). After
 * device_shutdown() its own shutdown has masked the watchdog's reset again
 * (PMU 0x408 and 0x40c bit 23): "Reboot failed -- System halted".
 *
 * Panic backstop: a panic notifier (first in the chain) arms cluster 1's
 * watchdog for panic_timeout + 5 s (at most 82 s), the loader's own sequence
 * (wdt_arm: WTCON 0, WTDAT = WTCNT, WTCLRINT, WTCON 0xFF39, PMU bit 23
 * cleared in 0x408 then 0x40C). If anything after the panic screen stalls,
 * the watchdog resets 5 s after the planned SWRESET instead of whenever (the
 * phone's first panics sat for 60 s to 2 min: panic()'s mdelay() countdown
 * waits in WFE, which this SoC wakes far too rarely with interrupts off; see
 * patch-mainline.py step 11). Not armed with panic=0 (no restart wanted).
 * s9p_clkgate.panic_wdt=0 = no backstop.
 *
 * Power-off: PS_HOLD low (PMU +0x330c bit 8, exynos_power_off). With a cable
 * in, the PMIC keeps the phone on; after 3 s it restarts instead, as the
 * loader's own power-off does (reboot-mode wrote INFORM3 = normal).
 *
 * s9p_clkgate.pmu_reset=0: none of this (mainline: watchdog restart, no
 * power-off, no backstop).
 * The boot line "s9p-reset:" logs RST_STAT, the watchdog's PMU disable/mask
 * bits and the reset sequencer (0x500, bit 0 = Samsung's DFD dump) as found.
 */
#define S9P_PMU_SWRESET		0x0400
#define S9P_PMU_RST_STAT	0x0404
#define S9P_PMU_WDT_DISABLE	0x0408
#define S9P_PMU_WDT_MASK	0x040c
#define  S9P_PMU_WDT1_BIT	BIT(23)	/* cluster 1's watchdog */
#define S9P_PMU_RESET_SEQ	0x0500
#define S9P_PMU_INFORM2		0x0808
#define S9P_PMU_INFORM3		0x080c
#define S9P_PMU_PS_HOLD		0x330c
#define  S9P_PS_HOLD_OUT	BIT(8)
#define S9P_INFORM2_RESET	0x12345678
#define S9P_WTCON		0x00
#define S9P_WTDAT		0x04
#define S9P_WTCNT		0x08
#define S9P_WTCLRINT		0x0c
#define S9P_WTCON_ARM		0xff39	/* prescaler 255, /128, enable, reset */
#define S9P_WDT_HZ		793	/* 26 MHz / 256 / 128 = 793.457 */
#define S9P_WDT_MAX_S		82	/* 65535 counts */

static void __iomem *s9p_pmu;
static void __iomem *s9p_wdt1;
static bool s9p_pmu_reset = true;
module_param_named(pmu_reset, s9p_pmu_reset, bool, 0444);
MODULE_PARM_DESC(pmu_reset, "restart by PMU SWRESET, power off by PS_HOLD, panic watchdog backstop (Starbite; 0 = watchdog restart only)");
static bool s9p_panic_wdt = true;
module_param_named(panic_wdt, s9p_panic_wdt, bool, 0644);
MODULE_PARM_DESC(panic_wdt, "at a panic, arm cluster 1's watchdog for panic_timeout + 5 s (Starbite backstop)");

/* a busy wait on the architected counter: no WFE, no timer interrupt, no timekeeping */
static void s9p_spin_ms(unsigned int ms)
{
	u64 rate = arch_timer_get_rate() ?: 26000000;
	u64 start = arch_timer_read_counter();
	u64 ticks = rate * ms / 1000;

	while (arch_timer_read_counter() - start < ticks)
		cpu_relax();
}

static void s9p_pmu_swreset(void)
{
	writel_relaxed(S9P_INFORM2_RESET, s9p_pmu + S9P_PMU_INFORM2);
	dsb(sy);
	writel_relaxed(1, s9p_pmu + S9P_PMU_SWRESET);
	dsb(sy);
}

static int s9p_pmu_restart(struct sys_off_data *data)
{
	s9p_pmu_swreset();
	s9p_spin_ms(1000);
	pr_emerg("s9p-reset: SWRESET did not reset (INFORM3 %08x), next handler\n",
		 readl_relaxed(s9p_pmu + S9P_PMU_INFORM3));
	return NOTIFY_DONE;
}

static int s9p_pmu_power_off(struct sys_off_data *data)
{
	writel_relaxed(readl_relaxed(s9p_pmu + S9P_PMU_PS_HOLD) & ~S9P_PS_HOLD_OUT,
		       s9p_pmu + S9P_PMU_PS_HOLD);
	dsb(sy);
	s9p_spin_ms(3000);
	pr_emerg("s9p-reset: still on with PS_HOLD low (a cable keeps the PMIC on), restarting\n");
	s9p_pmu_swreset();
	s9p_spin_ms(1000);
	return NOTIFY_DONE;
}

/* the loader's wdt_arm, for s seconds */
static void s9p_wdt1_arm(unsigned int s)
{
	u32 count = min(s, (unsigned int)S9P_WDT_MAX_S) * S9P_WDT_HZ;

	writel_relaxed(0, s9p_wdt1 + S9P_WTCON);
	writel_relaxed(count, s9p_wdt1 + S9P_WTDAT);
	writel_relaxed(count, s9p_wdt1 + S9P_WTCNT);
	writel_relaxed(0, s9p_wdt1 + S9P_WTCLRINT);
	writel_relaxed(S9P_WTCON_ARM, s9p_wdt1 + S9P_WTCON);
	writel_relaxed(readl_relaxed(s9p_pmu + S9P_PMU_WDT_DISABLE) & ~S9P_PMU_WDT1_BIT,
		       s9p_pmu + S9P_PMU_WDT_DISABLE);
	writel_relaxed(readl_relaxed(s9p_pmu + S9P_PMU_WDT_MASK) & ~S9P_PMU_WDT1_BIT,
		       s9p_pmu + S9P_PMU_WDT_MASK);
	dsb(sy);
}

static int s9p_pmu_panic(struct notifier_block *nb, unsigned long ev, void *data)
{
	int t = READ_ONCE(panic_timeout);
	unsigned int s;

	if (!s9p_wdt1 || !READ_ONCE(s9p_panic_wdt) || !t)
		return NOTIFY_DONE;
	s = t > 0 ? min(t, S9P_WDT_MAX_S - 5) + 5 : 5;
	s9p_wdt1_arm(s);
	pr_emerg("s9p-reset: panic: SWRESET in %d s, cluster-1 watchdog backstop at %u s\n",
		 t > 0 ? t : 0, s);
	return NOTIFY_DONE;
}

static struct notifier_block s9p_pmu_panic_nb = {
	.notifier_call = s9p_pmu_panic,
	.priority = INT_MAX,	/* first: before anything that could stall */
};

static int __init s9p_pmu_reset_init(void)
{
	struct device_node *np;

	np = of_find_compatible_node(NULL, NULL, "samsung,exynos9810-pmu");
	if (!np)
		return 0;
	s9p_pmu = of_iomap(np, 0);
	of_node_put(np);
	if (!s9p_pmu)
		return -ENOMEM;
	pr_info("s9p-reset: RST_STAT %08x WDT disable %08x mask %08x reset sequencer %08x INFORM2 %08x INFORM3 %08x\n",
		readl_relaxed(s9p_pmu + S9P_PMU_RST_STAT), readl_relaxed(s9p_pmu + S9P_PMU_WDT_DISABLE),
		readl_relaxed(s9p_pmu + S9P_PMU_WDT_MASK), readl_relaxed(s9p_pmu + S9P_PMU_RESET_SEQ),
		readl_relaxed(s9p_pmu + S9P_PMU_INFORM2), readl_relaxed(s9p_pmu + S9P_PMU_INFORM3));
	if (!s9p_pmu_reset) {
		pr_info("s9p-reset: off (pmu_reset=0): watchdog restart only, no power-off\n");
		return 0;
	}
	if (IS_ERR(register_sys_off_handler(SYS_OFF_MODE_RESTART, SYS_OFF_PRIO_HIGH,
					    s9p_pmu_restart, NULL)))
		pr_err("s9p-reset: restart handler not registered\n");
	if (IS_ERR(register_sys_off_handler(SYS_OFF_MODE_POWER_OFF, SYS_OFF_PRIO_DEFAULT,
					    s9p_pmu_power_off, NULL)))
		pr_err("s9p-reset: power-off handler not registered\n");
	/* cluster 1's watchdog (the DT node s3c2410_wdt drives), for the panic backstop */
	np = of_find_compatible_node(NULL, NULL, "samsung,exynos7-wdt");
	if (np) {
		s9p_wdt1 = of_iomap(np, 0);
		of_node_put(np);
	}
	if (s9p_wdt1)
		atomic_notifier_chain_register(&panic_notifier_list, &s9p_pmu_panic_nb);
	pr_info("s9p-reset: restart = INFORM2 + PMU SWRESET, power-off = PS_HOLD low, panic watchdog backstop %s\n",
		s9p_wdt1 ? (s9p_panic_wdt ? "on" : "off (panic_wdt=0)") : "missing (no watchdog node)");
	return 0;
}
postcore_initcall(s9p_pmu_reset_init);
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

    # 6f. s9p-decon: the S6E3HA8 self mask, as Samsung's kernel handles it (2026-11-06). S-Boot uploads
    # the rounded-corner mask image (side RAM) and enables it (7A 01, rows 0-149 and 2810-2959); Samsung's
    # kernel re-sends image + enable after every panel init, this driver sleep-cycles the panel and never
    # did. panel_mask (default 1) re-sends both after every sleep-out, before display-on; 2 = mask off.
    # The image is patches/s6e3ha8-star-self-mask.hex (checked). Analysis: plan 6.6.
    edit(w, decon,
         '\tif (!priv->dsim || !len || len > 64)\n\t\treturn -EINVAL;\n',
         '\t/* Starbite self mask: side-RAM writes are 1 + 496 bytes (vendor panel_dsi_write_mem) */\n'
         '\tif (!priv->dsim || !len || len > 512)\n\t\treturn -EINVAL;\n',
         'Starbite self mask: side-RAM writes')
    edit(w, decon,
         '\t} else {\n\t\tfor (i = 0; i < len; i += 4) {\n',
         '\t} else {\n'
         '\t\t/* Starbite self mask: the payload FIFO drained (DSIM_FIFOCTRL EMPTY_PL_SFR) */\n'
         '\t\tif (readl_poll_timeout_atomic(priv->dsim + 0x68, v, v & BIT(8), 10, 20000))\n'
         '\t\t\tdev_warn_once(priv->drm.dev, "DSI payload FIFO not empty before %02x+%u (FIFOCTRL=%08x)\\n",\n'
         '\t\t\t\t      buf[0], len, v);\n'
         '\t\tfor (i = 0; i < len; i += 4) {\n',
         'Starbite self mask: the payload FIFO drained')
    edit(w, decon,
         'MODULE_PARM_DESC(panel_mask_off, "send the S6E3HA8 self-mask disable (F0 5A 5A, 7A 00, F0 A5 A5) at every enable (Starbite black-bar test)");\n',
         'MODULE_PARM_DESC(panel_mask_off, "send the S6E3HA8 self-mask disable (F0 5A 5A, 7A 00, F0 A5 A5) at every enable (Starbite black-bar test)");\n'
         + DECON_MASK.replace('@LEN@', str(SELF_MASK_LEN)).replace('@IMG@', self_mask_c_array()),
         'static int s9p_panel_mask = S9P_MASK_RESTORE;')
    edit(w, decon,
         '/* Panel up: the reverse, with the boost converter\'s settling time. */\n'
         'static void s9p_panel_up(struct s9p_decon *priv)\n'
         '{\n'
         '\tif (s9p_panel_sleep) {\n'
         '\t\ts9p_decon_dcs(priv, MIPI_DCS_EXIT_SLEEP_MODE);\n'
         '\t\tmsleep(PANEL_SLEEP_DELAY_MS);\n'
         '\t}\n'
         '\ts9p_decon_dcs(priv, MIPI_DCS_SET_DISPLAY_ON);\n'
         '}\n',
         DECON_MASK_PROTO +
         '/* Panel up: the reverse, with the boost converter\'s settling time. */\n'
         'static void s9p_panel_up(struct s9p_decon *priv)\n'
         '{\n'
         '\tif (s9p_panel_sleep) {\n'
         '\t\ts9p_decon_dcs(priv, MIPI_DCS_EXIT_SLEEP_MODE);\n'
         '\t\tmsleep(PANEL_SLEEP_DELAY_MS);\n'
         '\t}\n'
         '\ts9p_panel_mask_init(priv);\t/* Starbite self mask: before display-on, as Samsung */\n'
         '\ts9p_decon_dcs(priv, MIPI_DCS_SET_DISPLAY_ON);\n'
         '}\n',
         's9p_panel_mask_init(priv);\t/* Starbite self mask')

    # 6g. s9p-decon: the panic screen (2026-11-06; budget and own drawing after the first phone panic).
    # drm_panic's hooks: the driver draws a Starbite banner and the newest kernel log (TER16x32) into
    # its black frame within panic_budget_ms (500), then makes the frame the scanout with the flip's
    # register writes (no locks, no sleeping, no WFE); a blanked panel is woken. Needs
    # CONFIG_DRM_PANIC and CONFIG_FONT_TER16x32 (starbite.config); nothing without CONFIG_DRM_PANIC.
    edit(w, decon, '#include <linux/iopoll.h>\n', DECON_PANIC_INCLUDES, 'Starbite panic screen (6g)')
    edit(w, decon,
         '\tdma_addr_t black_dma;\n',
         '\tdma_addr_t black_dma;\n'
         '\t/* Starbite panic screen: the frame it is drawn into */\n'
         '\tu8 *panic_base;\n'
         '\tdma_addr_t panic_dma;\n'
         '\tunsigned int panic_pitch;\n'
         '\tbool panic_dirty;\t/* a debugfs test drew into the black frame */\n'
         '\tu64 panic_t0;\t\t/* ktime_get_mono_fast_ns() at get_scanout_buffer */\n'
         '\tunsigned int panic_ms;\t/* the last draw took (dry runs) */\n'
         '\tbool panic_late;\t/* ... and ran out of budget */\n',
         'Starbite panic screen: the frame it is drawn into')
    edit(w, decon,
         '/* ------------------------------------------------------------- connector */\n',
         DECON_PANIC + '/* ------------------------------------------------------------- connector */\n',
         'static int s9p_panic_get_scanout_buffer(')
    edit(w, decon,
         '\t\t\t\t\t   &priv->connector);\n\tif (ret)\n\t\tgoto err_config;\n',
         '\t\t\t\t\t   &priv->connector);\n\tif (ret)\n\t\tgoto err_config;\n'
         '\ts9p_panic_setup(priv);\t/* Starbite panic screen: before drm_dev_register() */\n',
         's9p_panic_setup(priv);\t/* Starbite panic screen')
    edit(w, decon,
         '\tif (s9p_wait_shadow_idle(priv))\n\t\treturn;\n'
         '\ts9p_decon_program_address(priv, priv->black_dma, PANEL_W * 4);\n',
         '\tif (s9p_wait_shadow_idle(priv))\n\t\treturn;\n'
         '\tif (priv->panic_dirty) {\t/* Starbite panic screen: a test drew into it */\n'
         '\t\tmemset(priv->black_cpu, 0, PANEL_W * PANEL_H * 4);\n'
         '\t\twmb();\n'
         '\t\tpriv->panic_dirty = false;\n'
         '\t}\n'
         '\ts9p_decon_program_address(priv, priv->black_dma, PANEL_W * 4);\n',
         'Starbite panic screen: a test drew into it')

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

    # 10. s9p-clkgate: restart through PMU SWRESET, power-off through PS_HOLD (2026-11-06). The only
    #     restart handler was s3c2410_wdt's: reboot / reboot -f hung, panic=10 reset ~2 min late.
    clk = 'src/soc/s9p-clkgate.c'
    edit(w, clk, '#include <linux/of_address.h>\n',
         '#include <linux/of_address.h>\n'
         '#include <linux/moduleparam.h>\t/* Starbite restart (10) */\n'
         '#include <linux/notifier.h>\n'
         '#include <linux/panic.h>\n'
         '#include <linux/panic_notifier.h>\n'
         '#include <linux/reboot.h>\n'
         '#include <clocksource/arm_arch_timer.h>\n',
         'Starbite restart (10)')
    edit(w, clk, 'postcore_initcall(s9p_clkgate_init);\n',
         'postcore_initcall(s9p_clkgate_init);\n' + CLKGATE_RESET,
         'static int s9p_pmu_restart(')


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
