/* SPDX-License-Identifier: APSL-2.0 */
/* PSCI 0.1 call wrappers (see sb_psci.h). */
#include "sb_psci.h"

int
sb_psci_init(struct sb_psci *p, const char *method, size_t method_len, uint32_t fn_cpu_on,
    uint32_t fn_cpu_off, uint32_t fn_cpu_suspend, sb_psci_conduit_t call, void *ctx)
{
	p->ready = false;
	/* "smc", optionally NUL-terminated; anything else (hvc above all) is refused */
	if (method == NULL || method_len < 3 || method_len > 4 || method[0] != 's' || method[1] != 'm' ||
	    method[2] != 'c' || (method_len == 4 && method[3] != '\0')) {
		return SB_PSCI_E_METHOD;
	}
	if (fn_cpu_on == 0 || !(fn_cpu_on & 0x80000000u) || call == NULL) {
		return SB_PSCI_E_FNID;
	}
	p->call = call;
	p->ctx = ctx;
	p->fn_cpu_on = fn_cpu_on;
	p->fn_cpu_off = fn_cpu_off;
	p->fn_cpu_suspend = fn_cpu_suspend;
	p->ready = true;
	return PSCI_SUCCESS;
}

int32_t
sb_psci_cpu_on(const struct sb_psci *p, uint64_t target, uint64_t entry_pa, uint64_t context)
{
	if (!p->ready) {
		return PSCI_NOT_SUPPORTED;
	}
	/* SMC32 and SMC64 IDs alike: the firmware's result is the low 32 bits of x0, signed */
	return (int32_t)(uint32_t)p->call(p->ctx, p->fn_cpu_on, target, entry_pa, context);
}

const char *
sb_psci_strerror(int32_t rc)
{
	switch (rc) {
	case PSCI_SUCCESS: return "SUCCESS";
	case PSCI_NOT_SUPPORTED: return "NOT_SUPPORTED";
	case PSCI_INVALID_PARAMS: return "INVALID_PARAMETERS";
	case PSCI_DENIED: return "DENIED";
	case PSCI_ALREADY_ON: return "ALREADY_ON";
	case PSCI_ON_PENDING: return "ON_PENDING";
	case PSCI_INTERNAL_FAILURE: return "INTERNAL_FAILURE";
	case PSCI_NOT_PRESENT: return "NOT_PRESENT";
	case PSCI_DISABLED: return "DISABLED";
	case PSCI_INVALID_ADDRESS: return "INVALID_ADDRESS";
	default: return "UNKNOWN";
	}
}
