/* SPDX-License-Identifier: APSL-2.0 */
/*
 * PSCI 0.1 for the S9's Samsung EL3 firmware, plain C, no IOKit. Shared by StarbitePMGR.cpp and the host tests.
 *
 * Facts (../NOTES.md section 1): the firmware is PSCI 0.1 only. PSCI_VERSION (0x84000000) answers -1
 * (NOT_SUPPORTED), so nothing here ever asks for it, and the function IDs come from the ADT /psci node
 * (tools/board/starlte.board [psci]: cpu_on 0xc4000003, cpu_off 0x84000002, cpu_suspend 0xc4000001). The
 * conduit is SMC: HVC is UNDEFINED at EL1 on the S9 (Samsung's uH owns EL2), so any method but "smc" is refused.
 *
 * The conduit itself (the smc instruction) is the caller's: struct sb_psci.call. The kernel passes a function
 * doing `smc #0` (StarbitePMGR.cpp); the tests pass a mock firmware.
 */
#ifndef STARBITE_PSCI_H
#define STARBITE_PSCI_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* PSCI return codes (ARM DEN 0022) */
#define PSCI_SUCCESS            0
#define PSCI_NOT_SUPPORTED      (-1)
#define PSCI_INVALID_PARAMS     (-2)
#define PSCI_DENIED             (-3)
#define PSCI_ALREADY_ON         (-4)
#define PSCI_ON_PENDING         (-5)
#define PSCI_INTERNAL_FAILURE   (-6)
#define PSCI_NOT_PRESENT        (-7)
#define PSCI_DISABLED           (-8)
#define PSCI_INVALID_ADDRESS    (-9)

/* the one ID we must never send (and the tests check we never do) */
#define PSCI_0_2_FN_PSCI_VERSION 0x84000000u

/* errors of sb_psci_init (negative, distinct from PSCI codes by range) */
#define SB_PSCI_E_METHOD        (-100)  /* not "smc" */
#define SB_PSCI_E_FNID          (-101)  /* cpu_on missing / zero / not a fast call */

typedef int64_t (*sb_psci_conduit_t)(void *ctx, uint64_t fid, uint64_t a1, uint64_t a2, uint64_t a3);

struct sb_psci {
	sb_psci_conduit_t call;
	void *ctx;
	uint32_t fn_cpu_on, fn_cpu_off, fn_cpu_suspend;
	bool ready;
};

/*
 * method: the ADT "method" bytes (NUL-terminated or not, method_len bytes); fn_*: the ADT u32s (0 = absent).
 * cpu_on is required; it must be an SMCCC fast call (bit 31 set), as every PSCI ID is.
 */
int sb_psci_init(struct sb_psci *p, const char *method, size_t method_len, uint32_t fn_cpu_on,
    uint32_t fn_cpu_off, uint32_t fn_cpu_suspend, sb_psci_conduit_t call, void *ctx);

/* CPU_ON(target, entry, context): the firmware's signed 32-bit result (SMCCC: x0, sign-extended from w0) */
int32_t sb_psci_cpu_on(const struct sb_psci *p, uint64_t target, uint64_t entry_pa, uint64_t context);

const char *sb_psci_strerror(int32_t rc);

#ifdef __cplusplus
}
#endif

#endif /* STARBITE_PSCI_H */
