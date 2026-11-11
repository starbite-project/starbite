/* SPDX-License-Identifier: APSL-2.0 */
/*
 * StarbitePMGR: the S9's CPU power manager for XNU's USE_APPLEARMSMP path (iokit/Kernel/arm/AppleARMSMP.cpp waits
 * for an IOPMGR, then calls enableCPUCore for every secondary). Matched on the ADT /psci node ("starbite,psci").
 *
 *   enableCPUCore   (M3 only) the PMU CAL values, read back, then PSCI 0.1 CPU_ON over SMC with the ADT's
 *                   function ID, target = the ADT's starbite,psci-target, entry = sweet_secondary_entry's PA
 *                   (or a trampoline below 4 GiB if the ADT names one and the entry is above).
 *   idle            WFI, which XNU's cpu_idle() executes itself: no idle timer, no deeper state.
 *   CPU_BOOTED      on each core: MPIDR and MIDR checked against the ADT (an A55 where an M3 should be fails).
 *   M2 self-test    a thread after IOAllCPUInitialized: every core up, an IPI to each, a timer tick on each;
 *                   prints "StarbitePMGR: M2 PASS: cluster E: 4 up, cluster P: 4 up, IPI ok, timer 26 MHz",
 *                   then sbstop=smp resets into TWRP (kernel patch 0423+).
 */
#ifndef STARBITE_PMGR_DRIVER_H
#define STARBITE_PMGR_DRIVER_H

#include <IOKit/IOPMGR.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <machine/machine_routines.h>
#include "hw/sb_smp.h"

class StarbitePMGR : public IOPMGR
{
	OSDeclareDefaultStructors(StarbitePMGR);

public:
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;

	void enableCPUCore(unsigned int cpu_id, uint64_t entry_pa) APPLE_KEXT_OVERRIDE;
	void enableCPUCore(unsigned int cpu_id) APPLE_KEXT_OVERRIDE;
	void disableCPUCore(unsigned int cpu_id) APPLE_KEXT_OVERRIDE;
	void enableCPUCluster(unsigned int cluster_id) APPLE_KEXT_OVERRIDE;
	void disableCPUCluster(unsigned int cluster_id) APPLE_KEXT_OVERRIDE;
	void initCPUIdle(ml_processor_info_t *info) APPLE_KEXT_OVERRIDE;
	void enterCPUIdle(UInt64 *newIdleTimeoutTicks) APPLE_KEXT_OVERRIDE;
	void exitCPUIdle(UInt64 *newIdleTimeoutTicks) APPLE_KEXT_OVERRIDE;
	void updateCPUIdle(UInt64 *newIdleTimeoutTicks) APPLE_KEXT_OVERRIDE;

private:
	bool readTopology(void);
	bool mapPMU(void);
	bool setUpEntry(IOService *provider);
	static bool cpuEvent(void *param, enum cpu_event event, unsigned int cpu_or_cluster);
	static void selfTestThread(void *arg, wait_result_t wr);
	void selfTest(void);

	struct sb_psci fPSCI = {};
	struct sb_smp fSMP = {};
	struct sb_pmu_io fPMUIo = {};
	IOMemoryMap *fPMUMap = nullptr;
	IOMemoryMap *fTrampMap = nullptr;
	uint64_t fEntryPA = 0;
	uint64_t fTrampolinePA = 0;
	uint32_t fBootedCount = 0;
};

#endif /* STARBITE_PMGR_DRIVER_H */
