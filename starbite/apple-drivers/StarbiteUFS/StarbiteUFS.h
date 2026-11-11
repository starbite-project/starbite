/* SPDX-License-Identifier: APSL-2.0 */
/*
 * StarbiteUFS: the Galaxy S9's (Exynos 9810) UFS 2.1 host as IOBlockStorageDevices, FENCED.
 *
 *   /arm-io/ufs nub ("starbite,ufs")
 *     StarbiteUFS (IOService)                 the controller: takes the UFSHCI over as S-Boot left it, owns the
 *       |                                     request list, the doorbell/completion state machine, the fence
 *       +-- StarbiteUFSRange (IOBlockStorageDevice)  one per usable Starbite-owned range of the allow-list
 *             IOBlockStorageDriver -> IOMedia (whole range) -> IOGUIDPartitionScheme -> IOMedia ...
 *
 * Block 0 of a StarbiteUFSRange is the range's first LBA: Apple's software never sees an Android LBA, not even the
 * LU's GPT. The plain-C core (hw/) does the protocol, the fence and the state machine; this file is the IOKit glue.
 * Boot-args, ADT properties and log lines: PHONE-TEST.md, NOTES.md.
 */
#ifndef STARBITEUFS_H
#define STARBITEUFS_H

#include <IOKit/IOService.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOCommandGate.h>
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODMACommand.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/storage/IOBlockStorageDevice.h>

#include "hw/sbufs_hc.h"

class StarbiteUFSRange;

struct StarbiteUFSXfer {
	StarbiteUFSXfer *next;          /* ready queue */
	bool used, pooled;
	unsigned range;
	uint32_t lun;
	uint64_t lba, nblks, done;      /* absolute start, total, blocks finished */
	bool write, fua;
	bool sync;                      /* SYNCHRONIZE CACHE over [lba, lba+nblks): no data */
	bool finished;                  /* sync: done, status valid */
	IOReturn status;
	uint32_t chunk;                 /* blocks in flight */
	bool bounce;                    /* the chunk in flight went through the slot's bounce buffer */
	unsigned retries;
	IOMemoryDescriptor *buffer;
	IODMACommand *dma;
	IOStorageCompletion completion;
};

class StarbiteUFS : public IOService
{
	OSDeclareDefaultStructors(StarbiteUFS);

public:
	virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	virtual void stop(IOService *provider) APPLE_KEXT_OVERRIDE;
	virtual void free(void) APPLE_KEXT_OVERRIDE;

	/* for StarbiteUFSRange */
	IOReturn submit(unsigned range, IOMemoryDescriptor *buffer, UInt64 block, UInt64 nblks,
	    IOStorageAttributes *attributes, IOStorageCompletion *completion);
	IOReturn synchronize(unsigned range, UInt64 block, UInt64 nblks);
	const struct sbufs_range *rangeInfo(unsigned range) const;
	bool rangeWritable(unsigned range) const;
	const char *vendor() const { return fVendor; }
	const char *product() const { return fProduct; }
	const char *revision() const { return fRevision; }
	uint32_t maxBlocksPerCommand() const;

	/* sbufs_hw_ops back ends (static: C callbacks) */
	static uint32_t hwRead(void *ctx, unsigned bank, uint32_t off);
	static void hwWrite(void *ctx, unsigned bank, uint32_t off, uint32_t v);
	static void hwBarrier(void *ctx);
	static uint64_t hwNowUs(void *ctx);
	static void hwDelayUs(void *ctx, uint32_t us);
	static void hwLog(void *ctx, const char *line);
	static void hwClean(void *ctx, uint32_t off, uint32_t len);
	static void hwInval(void *ctx, uint32_t off, uint32_t len);
	static void doneThunk(void *ctx, unsigned slot, void *cookie, const struct sbufs_result *r);

private:
	bool readConfig(IOService *provider);
	bool mapRegisters(IOService *provider);
	bool allocDMA(void);
	bool bringUp(void);
	void logRanges(void);
	void publishRanges(void);
	void logUFSP(void);

	StarbiteUFSXfer *getXfer(void);
	void putXfer(StarbiteUFSXfer *x);
	IOReturn submitGated(StarbiteUFSXfer *x);
	void pump(void);
	bool startChunk(StarbiteUFSXfer *x);
	void chunkDone(StarbiteUFSXfer *x, unsigned slot, const struct sbufs_result *r);
	void finish(StarbiteUFSXfer *x, IOReturn status);
	void reap(bool fromIrq);
	void armPoll(void);
	IOReturn syncGated(unsigned range, UInt64 block, UInt64 nblks);
	void fenceRefusal(int verdict, unsigned range, uint64_t block, uint64_t nblks, bool write);

	void handleInterrupt(IOInterruptEventSource *src, int count);
	void handlePollTimer(IOTimerEventSource *src);
	void handleTreeTimer(IOTimerEventSource *src);
	void handleReportTimer(IOTimerEventSource *src);
	bool mediaPublished(void *ref, IOService *media, IONotifier *notifier);

	void dumpTree(const char *why);
	void dumpEntry(IORegistryEntry *e, unsigned depth);
	bool reportRoot(void);
	void fenceSummary(void);
	bool underUs(IORegistryEntry *e) const;

	static IOReturn gatedSubmit(OSObject *owner, void *a0, void *a1, void *a2, void *a3);
	static IOReturn gatedSync(OSObject *owner, void *a0, void *a1, void *a2, void *a3);

	IOService *fNub;
	IOWorkLoop *fWorkLoop;
	IOCommandGate *fGate;
	IOInterruptEventSource *fIrq;
	IOTimerEventSource *fPollTimer, *fTreeTimer, *fReportTimer;
	IONotifier *fMediaNotifier;
	IOMemoryMap *fHciMap, *fVsMap, *fUfspMap;
	volatile uint32_t *fHci, *fVs, *fUfsp;
	IOBufferMemoryDescriptor *fDma;
	bool fDmaCached;

	struct sbufs_fence fFence;
	struct sbufs_hc fHc;
	StarbiteUFSRange *fRanges[SBUFS_MAX_RANGES];

	enum { kPool = 32 };
	StarbiteUFSXfer fPool[kPool];
	StarbiteUFSXfer *fReadyHead, *fReadyTail;
	StarbiteUFSXfer *fSlotXfer[SBUFS_MAX_SLOTS];
	unsigned fActive;               /* xfers accepted, not completed */

	/* boot-args */
	uint32_t fArgRW, fArgIrq, fArgMulti, fArgQD, fArgBounce, fArg64, fArgReport, fArgCached;
	bool fAdtWrites;
	char fStop[16];

	char fVendor[9], fProduct[17], fRevision[5];
	uint64_t fReqs, fBounced, fUARetries, fRefusalsLogged;
	bool fTreeDumped, fStopped, fPollArmed, fRootReported, fReportDone;
	unsigned fReportTicks;
	uint64_t fStartAbs;
};

class StarbiteUFSRange : public IOBlockStorageDevice
{
	OSDeclareDefaultStructors(StarbiteUFSRange);

public:
	bool initRange(StarbiteUFS *controller, unsigned index);
	unsigned index() const { return fIndex; }

	virtual IOReturn doAsyncReadWrite(IOMemoryDescriptor *buffer, UInt64 block, UInt64 nblks,
	    IOStorageAttributes *attributes, IOStorageCompletion *completion) APPLE_KEXT_OVERRIDE;
	virtual IOReturn doSynchronize(UInt64 block, UInt64 nblks, IOStorageSynchronizeOptions options = 0)
	APPLE_KEXT_OVERRIDE;
	virtual IOReturn doUnmap(IOBlockStorageDeviceExtent *extents, UInt32 extentsCount,
	    IOStorageUnmapOptions options = 0) APPLE_KEXT_OVERRIDE;
	virtual IOReturn doEjectMedia(void) APPLE_KEXT_OVERRIDE;
	virtual IOReturn doFormatMedia(UInt64 byteCapacity) APPLE_KEXT_OVERRIDE;
	virtual UInt32 doGetFormatCapacities(UInt64 *capacities, UInt32 capacitiesMaxCount) const APPLE_KEXT_OVERRIDE;
	virtual char *getVendorString(void) APPLE_KEXT_OVERRIDE;
	virtual char *getProductString(void) APPLE_KEXT_OVERRIDE;
	virtual char *getRevisionString(void) APPLE_KEXT_OVERRIDE;
	virtual char *getAdditionalDeviceInfoString(void) APPLE_KEXT_OVERRIDE;
	virtual IOReturn reportBlockSize(UInt64 *blockSize) APPLE_KEXT_OVERRIDE;
	virtual IOReturn reportEjectability(bool *isEjectable) APPLE_KEXT_OVERRIDE;
	virtual IOReturn reportMaxValidBlock(UInt64 *maxBlock) APPLE_KEXT_OVERRIDE;
	virtual IOReturn reportMediaState(bool *mediaPresent, bool *changedState = NULL) APPLE_KEXT_OVERRIDE;
	virtual IOReturn reportRemovability(bool *isRemovable) APPLE_KEXT_OVERRIDE;
	virtual IOReturn reportWriteProtection(bool *isWriteProtected) APPLE_KEXT_OVERRIDE;
#if TARGET_OS_OSX
	virtual IOReturn doLockUnlockMedia(bool doLock) APPLE_KEXT_OVERRIDE;
	virtual IOReturn doSynchronizeCache(void) APPLE_KEXT_OVERRIDE;
	virtual IOReturn reportLockability(bool *isLockable) APPLE_KEXT_OVERRIDE;
	virtual IOReturn reportPollRequirements(bool *pollRequired, bool *pollIsExpensive) APPLE_KEXT_OVERRIDE;
#endif

private:
	StarbiteUFS *fController;
	unsigned fIndex;
	char fVendor[9], fProduct[40], fRevision[5], fInfo[64];
};

#endif /* STARBITEUFS_H */
