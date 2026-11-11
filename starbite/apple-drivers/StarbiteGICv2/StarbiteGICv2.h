/* SPDX-License-Identifier: APSL-2.0 */
/*
 * StarbiteGICv2: the S9's GIC-400 as an IOInterruptController for XNU's USE_APPLEARMSMP path
 * (iokit/Kernel/arm/AppleARMSMP.cpp). Matched on the ADT /arm-io/gic node ("ARM,gicv2").
 *
 * Who does what (NOTES.md section 2 has the full contract):
 *   pexpert  pe_init_fiq() on every CPU: GICC on, PMR 0xff, BPR 0, banked SGI 1 + PPI 27 enabled at 0xa0, GICD on.
 *            sleh_irq(): GICC_IAR, then PPI 27 -> rtclock_intr, SGI -> cpu_signal_handler, anything else -> the
 *            per-CPU cpu_data interrupt_handler (= this driver's handleInterrupt with the INTID), then GICC_EOIR.
 *   driver   SPIs (32..): registerInterrupt / enable / disable / getInterruptType / cause, priority, routing
 *            (setSPITarget), lazy disable, strays; IPIs: sendIPI() writes GICD_SGIR (AppleARMSMP's PE_cpu_signal
 *            calls it because the ADT cpu nodes carry 3 interrupts); per CPU at CPU_BOOTED: reads its interface
 *            bit (banked ITARGETSR0) and installs handleInterrupt as that CPU's interrupt handler.
 */
#ifndef STARBITE_GICV2_DRIVER_H
#define STARBITE_GICV2_DRIVER_H

#include <IOKit/IOInterruptController.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <machine/machine_routines.h>
#include "hw/sb_gicv2.h"

class StarbiteGICv2 : public IOInterruptController
{
	OSDeclareDefaultStructors(StarbiteGICv2);

public:
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	void free(void) APPLE_KEXT_OVERRIDE;

	IOReturn registerInterrupt(IOService *nub, int source, void *target, IOInterruptHandler handler,
	    void *refCon) APPLE_KEXT_OVERRIDE;
	IOReturn unregisterInterrupt(IOService *nub, int source) APPLE_KEXT_OVERRIDE;
	IOReturn getInterruptType(IOService *nub, int source, int *interruptType) APPLE_KEXT_OVERRIDE;
	IOReturn enableInterrupt(IOService *nub, int source) APPLE_KEXT_OVERRIDE;
	IOReturn disableInterrupt(IOService *nub, int source) APPLE_KEXT_OVERRIDE;
	IOReturn causeInterrupt(IOService *nub, int source) APPLE_KEXT_OVERRIDE;

	IOInterruptAction getInterruptHandlerAddress(void) APPLE_KEXT_OVERRIDE;
	IOReturn handleInterrupt(void *refCon, IOService *nub, int source) APPLE_KEXT_OVERRIDE;

	bool vectorCanBeShared(IOInterruptVectorNumber vectorNumber, IOInterruptVector *vector) APPLE_KEXT_OVERRIDE;
	void initVector(IOInterruptVectorNumber vectorNumber, IOInterruptVector *vector) APPLE_KEXT_OVERRIDE;
	int getVectorType(IOInterruptVectorNumber vectorNumber, IOInterruptVector *vector) APPLE_KEXT_OVERRIDE;
	void disableVectorHard(IOInterruptVectorNumber vectorNumber, IOInterruptVector *vector) APPLE_KEXT_OVERRIDE;
	void enableVector(IOInterruptVectorNumber vectorNumber, IOInterruptVector *vector) APPLE_KEXT_OVERRIDE;
	void causeVector(IOInterruptVectorNumber vectorNumber, IOInterruptVector *vector) APPLE_KEXT_OVERRIDE;

	void sendIPI(unsigned int cpu_id, bool deferred) APPLE_KEXT_OVERRIDE;
	void cancelDeferredIPI(unsigned int cpu_id) APPLE_KEXT_OVERRIDE;

	/* route one SPI to one logical CPU (that CPU must have booted: its interface bit is read then) */
	IOReturn setSPITarget(uint32_t intid, unsigned int logicalCPU);

private:
	struct PerCPU {
		uint32_t booted;        /* CPU_BOOTED seen on this CPU */
		uint8_t ifaceMask;
		int maskError;
		uint32_t giccCtlr, sgiPpiEnable;
		uint64_t ipisSent, deferredSent;
	};

	bool vectorOf(IOService *nub, int source, uint32_t *intid);
	void cpuBooted(unsigned int cpu);
	static bool cpuEvent(void *param, enum cpu_event event, unsigned int cpu_or_cluster);
	static void attachThread(void *arg, wait_result_t wr);

	IOService *fProvider = nullptr;
	IOMemoryMap *fDistMap = nullptr;
	IOMemoryMap *fCpuMap = nullptr;
	IOSimpleLock *fDistLock = nullptr;
	OSSymbol *fName = nullptr;
	IOInterruptHandler fHandler = nullptr;
	struct sb_gic fGic = {};
	uint32_t fNumVectors = 0;
	uint8_t fBootMask = 0;
	uint32_t fSgiRegistrations = 0;
	volatile uint32_t fStray = 0;
	uint32_t fStrayLogged = 0;
	PerCPU fCPU[SB_GIC_MAX_CPUS] = {};
};

#endif /* STARBITE_GICV2_DRIVER_H */
