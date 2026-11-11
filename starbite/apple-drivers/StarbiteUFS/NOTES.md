# StarbiteUFS: notes

The S9's (Exynos 9810) UFS 2.1 host as fenced `IOBlockStorageDevice`s (docs/APPLE-S9-DRIVERS.md Part 2.6,
Part 3 row 11, milestone M4). Licence: APSL-2.0, all files new.

## Files

| File | What |
|---|---|
| `StarbiteUFS.h/.cpp` | IOKit glue: `StarbiteUFS` (IOService on the `/arm-io/ufs` nub) and `StarbiteUFSRange` (IOBlockStorageDevice, one per usable allowed range) |
| `hw/sbufs_regs.h` | registers, UTRD/UPIU/SCSI constants |
| `hw/sbufs_fence.[ch]` | the fence: property parsing (fail closed), sealing, checks, range-to-LU mapping |
| `hw/sbufs_proto.[ch]` | CDBs, UTRD/UPIU/PRDT building, the doorbell audit (`sbufs_gate`), response parsing |
| `hw/sbufs_hc.[ch]` | take-over, wake, LU probe, slots, doorbell/completion state machine, timeouts, multi-block self test |
| `tools/sbufs-fence.py` | packs the two fence properties from LU 0's GPT (for the board file / loader) |
| `test/` | mock Exynos host + host tests (`sh test/run.sh`) |

## Register and protocol facts

| Fact | Value | Source |
|---|---|---|
| reg ranges of `/arm-io/ufs` | HCI 0x11120000/0x200, vendor HCI 0x11121100/0x200, UniPro 0x11110000/0x8000, UFSP 0x11130000/0x100 | tools/board/starlte.board (dts ufs@0x11120000) |
| interrupt | SPI 0xf2 (INTID 274) | board file |
| host after S-Boot | cap 0x1303ff0f (NUTRS 16, 64AS set), ver 0x210, HCS 0x10f, **HCE reads 0** but the host works; S-Boot's list at 0xef002000, RSR 1, nexus 0x2 | NOTES.md 2026-10-25 (phone), herolte_ufs.c `ufs_take_over_host` |
| take-over | IE 0, AHIT (0x18) 0 (auto-hibernate drops completions on the 9810), IS cleared, RSR 0, UTRLBA/U = ours, RSR 1, read back before any doorbell | herolte_ufs.c (phone-proven); AHIT: exynos9810-woa Star2LteUfsHcDxe via herolte_ufs.c |
| never done | HCE toggle, HCI_SW_RST, link startup, UIC commands, UniPro/M-PHY/clock writes, task management list, UFSP writes | design (S-Boot owns the link); an earlier port ufshc.c says the same |
| device after S-Boot | device W-LUN TUR: OCS 7, CHECK CONDITION 2/04/02; START STOP UNIT (ACTIVE, `cdb[4]=0x10`) to W-LUN 0xD0 wakes it; LUs 0-3 answer TUR | NOTES.md 2026-10-25, herolte_ufs.c `ufs_wake_device` |
| UTRD offsets/lengths | **bytes**, not dwords (PRDT_BYTE_GRAN quirk); response UPIU at 512 (512 bytes), PRDT at 1024 | herolte_ufs.c (phone); an earlier port names the quirk (BSP `UFSHCD_QUIRK_PRDT_BYTE_GRAN`) |
| UTRLCLR | a **1** clears a slot (BROKEN_REQ_LIST_CLR) | herolte_ufs.c, an earlier port |
| vendor nexus | `HCI_UTRL_NEXUS_TYPE` (vs +0x40): bit n set for a SCSI command in slot n, before the doorbell | herolte_ufs.c, an earlier port |
| PRDT entry | 16 bytes (FMP DESCTYPE 0 as S-Boot leaves it), at most 4 KiB of data per entry; word 3 bits 28-31 (FMP FAS/DAS) 0 = no encryption | herolte_ufs.c (phone: 128-byte entries on DESCTYPE 0 returned wrong data, 2026-10-27); an earlier port (4 KiB, FAS/DAS) |
| multi-entry commands | need vs TXPRDT/RXPRDT = 0x8000000c, DATA_REORDER = 0xa, AXIDMA = 0x98000003; 16 entries (64 KiB) per command proven (`ufsmulti=1` daily) | herolte_ufs.c `starlte_ufs_pick_stride` |
| UFSP | left open by S-Boot: region 0 = 0..0xffffffff, all LUs, ctrl 0xf1; regions 1-7 zero | NOTES.md 2026-10-25 (only read here) |
| LU 0 GPT | 4 KiB blocks, 128 x 128-byte entries at LBA 2, 25 partitions; BOOT p10 19458+14080, USERDATA p25 1698432+13917563 | NOTES.md 2026-10-25 |
| DMA addressing | 32-bit by default (BSP mask 32; herolte ran DMA in the low window); `sbufs64=1` only with CAP.64AS | an earlier port (BSP mask), herolte_ufs.c |
| OCS 7 on device errors | the 9810 may report device errors as OCS 7; the response UPIU's sense is still read | an earlier port (UFSHCD_QUIRK_BROKEN_OCS_FATAL_ERROR), phone |

**Disagreement noted:** an earlier port's file says S-Boot leaves FMP DESCTYPE 3 (128-byte PRDT entries) and puts the
response UPIU at 1024. Our phone runs say 16-byte entries (and response at 512) work, single and 16-entry. The
driver follows the phone; the multi-block self test catches a mismatch and falls back to one entry per command.

## Design

- **Fence, three layers** (`hw/sbufs_fence.h`): allow-list (a data command must lie wholly inside ONE usable
  allowed range), deny-list (any overlap refused; an allowed range touching one is dropped at start), built-in
  floors (first 6 and last 6 blocks of every LU, anything past READ CAPACITY, well-known LUs, LUs >= 8).
  Checked twice: at the request (`sbufs_fence_map`, counted in the fence log) and at the doorbell
  (`sbufs_gate` decodes the bytes actually written into the slot: whitelist of opcodes, direction, flags,
  lengths, PRDT, LUN, then the fence again). After ringing, the decoded range is checked a third time into
  `foreign_touched`, which must stay 0.
- **Command whitelist** (gate): TEST UNIT READY; INQUIRY; READ CAPACITY(10)/(16); READ(10/16); WRITE(10/16)
  (FUA only) and SYNCHRONIZE CACHE(10/16) (never 0 blocks = whole LU) only where writes are allowed;
  START STOP UNIT only ACTIVE to the device W-LUN. Everything else (UNMAP, FORMAT, WRITE BUFFER, SECURITY
  PROTOCOL, MODE SELECT, REPORT LUNS, RPMB/BOOT W-LUNs, task management, query requests) never reaches a slot.
- **Writes** need all three: range flag WRITABLE, ADT `starbite,writes-allowed` = 1, boot-arg `sbufsrw=1`.
- **Only LUs named by the allow-list are spoken to** (TUR, READ CAPACITY, one INQUIRY). The LU's own GPT is
  never read by the driver; `IOGUIDPartitionScheme` reads block 1 *of the range*.
- **Slots**: up to 8 (`sbufsqd`, capped by NUTRS), tag = slot. Descriptors in write-combined memory (default):
  two 32-byte UTRDs share a 64-byte cache line, so cached descriptors with clean/invalidate could overwrite a
  neighbour slot's OCS; `sbufscached=1` uses cached memory with explicit maintenance and **one** slot (herolte's
  proven mode) instead.
- **DMA**: `IODMACommand` (64-bit, 4 KiB segments, no system mapper: SysMMU stays in bypass) on the
  `IOMemoryDescriptor` from IOBlockStorageDriver (`setMemoryDescriptor` = `prepare()`); segments must be
  cache-line aligned, dword sized, below 4 GiB and cover the chunk, else the chunk goes through the slot's
  64 KiB bounce buffer. Cache maintenance with `performOperation` (store before a write, clean+invalidate
  before and after a read).
- **Interrupt** from the nub (index 0) through an `IOInterruptEventSource`, with a 20 ms safety poll that also
  runs the timeouts; without an interrupt a 250 us poll while commands are in flight.
- **Errors**: OCS / response / status / short transfer -> I/O error with sense in the log; UNIT ATTENTION (and
  NOT READY 04/xx) retried up to 3 times; 5 s timeout -> UTRLCLR, slot reused; a slot that will not clear or
  a fatal IS bit (DFES, UTPES, HCFES, SBFES) -> host given up, every request fails (no reset: the link is
  S-Boot's and re-initialising it is not this driver's to do).

## Fence property format (for `tools/board` / the loader)

All little endian, on `/arm-io/ufs`. `tools/sbufs-fence.py` writes both from LU 0's GPT and `test/run.sh` feeds its
output to the C parser.

`starbite,allowed-ranges` = header + N records of 64 bytes (N <= 8):

| off | size | field |
|---|---|---|
| 0 | 4 | magic 0x52464253 ("SBFR") |
| 4 | 4 | version 1 |
| 8 | 4 | record size 64 |
| 12 | 4 | N |
| rec+0 | 4 | LU (general, 0..7) |
| rec+4 | 4 | flags: 1 WRITABLE, 2 ROOT (informational); others refused |
| rec+8 | 8 | first LBA (4 KiB blocks, absolute in the LU) |
| rec+16 | 8 | block count (> 0, no wrap) |
| rec+24 | 16 | UUID, RFC 4122 byte order (the GPT partition's unique GUID, converted from GPT's mixed endian) |
| rec+40 | 16 | name, ASCII, NUL padded |
| rec+56 | 8 | reserved, must be 0 |

`starbite,denied-ranges` (optional) = header (magic 0x44464253 "SBFD", version 1, record size 40, N <= 48) +
records {u32 LU, u32 reserved 0, u64 first, u64 count (> 0), char name[16]}.

`starbite,writes-allowed` (optional) = u32 1. Absent at M4.

Any malformed byte in either list (length, magic, version, record size, LU, flags, empty or wrapping range,
reserved bits) and the driver exposes **nothing**.

## Credits

- Starbite: `build/xnu-12377/bsd/dev/herolte_ufs.c` (patches 0204, 0034, 0069, 0300; derived from the SWEET
  driver of ios-sweet/ios-a5xelte) for every phone-proven register fact; read, not copied (this driver is a
  rewrite as IOKit + a host-testable core).
- an earlier port, `drivers/storage/ufs/ufshc.c` of `example-project/refport-kernel` (APSL-2.0 per its
  LICENSE/LICENSES.md; "an earlier port"): the Exynos 9810 quirk list with its sources (byte granularity,
  UTRLCLR, nexus, 4 KiB PRDT entries, FMP FAS/DAS, OCS 7, 32-bit DMA) and the idea of a per-command whitelist
  checked on the descriptor as written. Facts and ideas only; no code taken.
- `starbite/libsb/fence.c` (GPL-2.0-only): read for the partition policy (which names are never touched) only.

## Open questions

1. **Root at M4.** USERDATA now belongs to the iOS 27 full root (commit 30f40f8); iOS 26's root is on the card
   (StarbiteSD's job). M4b as written mounts whatever HFSX volume is at the start of USERDATA read-only. OK, or
   should M4b wait for StarbiteSD and M4 stay storage-only (M4a)?
2. **4 KiB sectors for HFS.** The IOMedia has 4096-byte blocks (the device's); herolte_ufs exposed 512-byte
   sectors to BSD. HFS+ supports 4Kn devices, but our roots were made on 512-byte images. If the mount fails,
   a 512-byte presentation (8:1, read-modify-write for writes) is the fallback; not implemented.
3. **Root matching.** IOKitBSDInit has no "IOMedia under this range" matching (INTEGRATION.md proposes one);
   `rd=disk0` works while StarbiteUFS is the only disk.
4. **Who writes the fence properties**: built once from a TWRP read of LU 0's GPT into the ADT (board tool), or
   by the loader at each boot from the live GPT? The second follows repartitioning; the first is simpler.
5. Interrupt routing is untested on this controller in XNU (herolte_ufs was always polled): M4a runs polled,
   M4b with the interrupt.
