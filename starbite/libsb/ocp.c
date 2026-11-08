/* SPDX-License-Identifier: GPL-2.0-only
 * The M3 over-current protection (OCP) path, read side: one implementation for Engineer OS's
 * Overclocking and Starbite OS (sb.h "OCP"). Facts from the vendor 9810 kernel
 * (ref/starlte-kernel drivers/soc/samsung/exynos-ocp.c, exynos9810.dtsi exynos-ocp):
 *  - the S2MPS18 PMIC raises OCP_WARN (PMIC reg 0xD3 OCP_WARN1 bit 7 enable, bits 4:0 level) before
 *    the buck's hard over-current limit; S-Boot leaves it ON on this phone (0xD3 = 0x80, 0xD4 = 0xC0,
 *    Engineer OS ocself-5, 2026-11-08);
 *  - the M3 cluster's GCU throttles in hardware on the warning and can raise INTREQ__CPUCL1_GCUIRQ;
 *  - Linux writes OCPINTCTL.IRP = 2 * f / f_min - 2 (f_min the policy minimum, 650000 kHz) on every
 *    M3 frequency change, clears OCPTHROTTCTL.OCPTHROTTERRA on the interrupt, lowers the maximum one
 *    step and releases it 15 ms later when the big cluster's load is under 50 %; with the handler
 *    off it clamps M3 at max-freq-wo-ocp 1794000.
 * The system registers are implementation defined: they exist only on the M3 (UNDEFINED on an A55),
 * so sb_ocp_worker runs on an M3 worker core (sb_smp_start(4..7, sb_ocp_worker, 0)). It only reads.
 * Runs on the M3: ARMv8.0 (built with -march=armv8-a, checked like smp.c).
 */
#include "sb.h"

int sb_ocp_pmic(uint8_t *warn1, uint8_t *warn2)
{
    return sb_pmic_read(1, SB_OCP_PMIC_WARN1, warn1) && sb_pmic_read(1, SB_OCP_PMIC_WARN2, warn2);
}

unsigned sb_ocp_irp(uint32_t khz, uint32_t min_khz)
{
    if (!min_khz)
        min_khz = SB_OCP_FMIN_KHZ;
    uint64_t irp = (uint64_t)khz * 2U / min_khz;
    irp = irp >= 2 ? irp - 2 : 0;
    return irp > SB_OCP_IRP_MASK ? SB_OCP_IRP_MASK : (unsigned)irp;
}

/* On an M3 core: v[0] MIDR_EL1, v[1] OCPTHROTTCTL, v[2] OCPTHROTTCNTA, v[3] OCPINTCTL, v[4] GCUCTL,
 * v[5] OCPTIDEPBOTTHRESH, v[7] = 1 when done; then it waits for sb_smp_stop (so the boot core gives
 * the FlexPMU C2 hint before the core's CPU_OFF). A trap is caught by the worker's vectors (FAULT). */
void sb_ocp_worker(unsigned w, void *arg)
{
    struct sb_wslot *s = sb_smp_own_slot(w);
    uint64_t v;
    (void)arg;
    if (!s)
        return;
    __asm__ volatile("mrs %0, midr_el1" : "=r"(v));
    s->v[0] = v;
    sb_worker_publish(s);
    if (((v >> 24) & 0xffU) == 0x53U) {     /* 'S': Samsung (Mongoose); never on an A55 */
        __asm__ volatile("mrs %0, S3_0_C15_C12_7" : "=r"(v));
        s->v[1] = v;
        sb_worker_publish(s);
        __asm__ volatile("mrs %0, S3_0_C15_C14_4" : "=r"(v));
        s->v[2] = v;
        sb_worker_publish(s);
        __asm__ volatile("mrs %0, S3_0_C15_C12_5" : "=r"(v));
        s->v[3] = v;
        sb_worker_publish(s);
        __asm__ volatile("mrs %0, S3_1_C15_C11_0" : "=r"(v));
        s->v[4] = v;
        sb_worker_publish(s);
        __asm__ volatile("mrs %0, S3_0_C15_C15_1" : "=r"(v));
        s->v[5] = v;
    }
    s->v[7] = 1;
    sb_worker_publish(s);
    while (!sb_worker_stop())
        __asm__ volatile("yield");
}
