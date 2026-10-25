#!/usr/bin/env python3
"""Re-create the woa team's unpublished UfsPassThruDxe change for the Exynos
UFS host (Linux UFSHCD_QUIRK_PRDT_BYTE_GRAN + 128-byte FMP PRDT entries; see
Star2LteUfsHcDxe.c "UfsPassThruHci.c now emits byte-granular fields directly
(RuO=0x400, PrdtO=0x800, PrdtL=num_entries*sizeof(UTP_TR_PRD))").
Without it every query fails with OCS 4 (mismatch response UPIU size) and the
device never finishes initialization (starlte, 2026-10-25).
Every UTP command descriptor: request UPIU at 0x000, response UPIU at 0x400,
PRDT at 0x800, 128-byte PRDT entries (16 standard bytes + zeroed FMP words) of
at most 4 KiB each; the UTRD's RuO/RuL/PrdtO/PrdtL are in bytes and RuL always
spans the whole 0x400 response area (Linux: ALIGNED_UPIU_SIZE)."""
PATH = "MdeModulePkg/Bus/Ufs/UfsPassThruDxe/UfsPassThruHci.c"
s = open(PATH, newline="").read()
nl = "\r\n" if "\r\n" in s else "\n"


def sub(old, new, count=1):
    global s
    old, new = old.replace("\n", nl), new.replace("\n", nl)
    assert s.count(old) == count, (old, s.count(old))
    s = s.replace(old, new)


sub('#include "UfsPassThru.h"\n',
    '#include "UfsPassThru.h"\n\n'
    '//\n'
    '// Starbite/woa: Exynos UFS (PRDT_BYTE_GRAN, FMP) command descriptor layout.\n'
    '//\n'
    '#define STARBITE_UCD_RESP_OFF  0x400\n'
    '#define STARBITE_UCD_PRDT_OFF  0x800\n'
    '#define STARBITE_PRD_SIZE      128\n'
    '#define STARBITE_RESP_LEN      0x400   // the whole response area, like Linux ALIGNED_UPIU_SIZE\n'
    '//\n'
    '// Exynos UFS: 4 KiB per PRDT entry (Linux caps the segment size for FMP), and PrdtL\n'
    '// is a 16-bit byte count, so one command moves at most 511 entries.\n'
    '//\n'
    '#undef  UFS_MAX_DATA_LEN_PER_PRD\n'
    '#define UFS_MAX_DATA_LEN_PER_PRD  0x1000\n'
    '//\n'
    '// At most 512 KiB per command: Star2LteUfsHcDxe\'s WC bounce buffer (UFS_BOUNCE_BYTES).\n'
    '// Bigger transfers fall through to its cached DMA path, which stalls the HS burst\n'
    '// ("A19 FALL why=04", seen loading a 1 MiB Shell.efi). Also below the 16-bit PrdtL\n'
    '// limit of (0xFFFF / 128) entries.\n'
    '//\n'
    '#define STARBITE_MAX_XFER      0x80000\n')

# PRDT entries with a 128-byte stride.
sub("  for (PrdtIndex = 0; PrdtIndex < PrdtNumber; PrdtIndex++) {\n"
    "    if (RemainingLen < UFS_MAX_DATA_LEN_PER_PRD) {\n"
    "      Prdt[PrdtIndex].DbCount = (UINT32)RemainingLen - 1;\n"
    "    } else {\n"
    "      Prdt[PrdtIndex].DbCount = UFS_MAX_DATA_LEN_PER_PRD - 1;\n"
    "    }\n\n"
    "    Prdt[PrdtIndex].DbAddr  = (UINT32)RShiftU64 ((UINT64)(UINTN)Remaining, 2);\n"
    "    Prdt[PrdtIndex].DbAddrU = (UINT32)RShiftU64 ((UINT64)(UINTN)Remaining, 32);\n",
    "  for (PrdtIndex = 0; PrdtIndex < PrdtNumber; PrdtIndex++) {\n"
    "    UTP_TR_PRD  *Entry = (UTP_TR_PRD *)((UINT8 *)Prdt + PrdtIndex * STARBITE_PRD_SIZE);\n\n"
    "    if (RemainingLen < UFS_MAX_DATA_LEN_PER_PRD) {\n"
    "      Entry->DbCount = (UINT32)RemainingLen - 1;\n"
    "    } else {\n"
    "      Entry->DbCount = UFS_MAX_DATA_LEN_PER_PRD - 1;\n"
    "    }\n\n"
    "    Entry->DbAddr  = (UINT32)RShiftU64 ((UINT64)(UINTN)Remaining, 2);\n"
    "    Entry->DbAddrU = (UINT32)RShiftU64 ((UINT64)(UINTN)Remaining, 32);\n")

# SCSI command descriptor (with the per-command size cap; ScsiDisk retries smaller).
sub("  TotalLen = ROUNDUP8 (sizeof (UTP_COMMAND_UPIU)) + ROUNDUP8 (sizeof (UTP_RESPONSE_UPIU)) + PrdtNumber * sizeof (UTP_TR_PRD);\n",
    "  if (DataLen > STARBITE_MAX_XFER) {\n"
    "    if (DataDirection == UfsDataIn) {\n"
    "      Packet->InTransferLength = STARBITE_MAX_XFER;\n"
    "    } else {\n"
    "      Packet->OutTransferLength = STARBITE_MAX_XFER;\n"
    "    }\n\n"
    "    return EFI_BAD_BUFFER_SIZE;\n"
    "  }\n\n"
    "  TotalLen = STARBITE_UCD_PRDT_OFF + PrdtNumber * STARBITE_PRD_SIZE;\n")
sub("  Trd->RuL    = (UINT16)DivU64x32 ((UINT64)ROUNDUP8 (sizeof (UTP_RESPONSE_UPIU)), sizeof (UINT32));\n"
    "  Trd->RuO    = (UINT16)DivU64x32 ((UINT64)ROUNDUP8 (sizeof (UTP_COMMAND_UPIU)), sizeof (UINT32));\n"
    "  Trd->PrdtL  = (UINT16)PrdtNumber;\n"
    "  Trd->PrdtO  = (UINT16)DivU64x32 ((UINT64)(ROUNDUP8 (sizeof (UTP_COMMAND_UPIU)) + ROUNDUP8 (sizeof (UTP_RESPONSE_UPIU))), sizeof (UINT32));\n",
    "  Trd->RuL    = STARBITE_RESP_LEN;\n"
    "  Trd->RuO    = STARBITE_UCD_RESP_OFF;\n"
    "  Trd->PrdtL  = (UINT16)(PrdtNumber * STARBITE_PRD_SIZE);\n"
    "  Trd->PrdtO  = STARBITE_UCD_PRDT_OFF;\n")
sub("  PrdtBase = (UTP_TR_PRD *)((UINT8 *)TransReq->CmdDescHost + ROUNDUP8 (sizeof (UTP_COMMAND_UPIU)) + ROUNDUP8 (sizeof (UTP_RESPONSE_UPIU)));\n",
    "  PrdtBase = (UTP_TR_PRD *)((UINT8 *)TransReq->CmdDescHost + STARBITE_UCD_PRDT_OFF);\n")
sub("  TransReq->CmdDescSize = TransReq->Trd->PrdtO * sizeof (UINT32) + TransReq->Trd->PrdtL * sizeof (UTP_TR_PRD);\n",
    "  TransReq->CmdDescSize = TransReq->Trd->PrdtO + TransReq->Trd->PrdtL;\n")

# Query request descriptor: the write payload stays right after the request.
sub("    TotalLen = ROUNDUP8 (sizeof (UTP_QUERY_REQ_UPIU)) + ROUNDUP8 (sizeof (UTP_QUERY_RESP_UPIU)) + ROUNDUP8 (DataSize);\n",
    "    if (ROUNDUP8 (sizeof (UTP_QUERY_REQ_UPIU)) + ROUNDUP8 (DataSize) > STARBITE_UCD_RESP_OFF) {\n"
    "      return EFI_INVALID_PARAMETER;\n"
    "    }\n\n"
    "    TotalLen = STARBITE_UCD_RESP_OFF + STARBITE_RESP_LEN;\n")
sub("    TotalLen = ROUNDUP8 (sizeof (UTP_QUERY_REQ_UPIU)) + ROUNDUP8 (sizeof (UTP_QUERY_RESP_UPIU));\n",
    "    TotalLen = STARBITE_UCD_RESP_OFF + STARBITE_RESP_LEN;\n")
sub("    Trd->RuL = (UINT16)DivU64x32 ((UINT64)ROUNDUP8 (sizeof (UTP_QUERY_RESP_UPIU)), sizeof (UINT32));\n"
    "    Trd->RuO = (UINT16)DivU64x32 ((UINT64)ROUNDUP8 (sizeof (UTP_QUERY_REQ_UPIU)) + ROUNDUP8 (DataSize), sizeof (UINT32));\n",
    "    Trd->RuL = STARBITE_RESP_LEN;\n"
    "    Trd->RuO = STARBITE_UCD_RESP_OFF;\n")
sub("    Trd->RuL = (UINT16)DivU64x32 ((UINT64)ROUNDUP8 (sizeof (UTP_QUERY_RESP_UPIU)) + ROUNDUP8 (DataSize), sizeof (UINT32));\n"
    "    Trd->RuO = (UINT16)DivU64x32 ((UINT64)ROUNDUP8 (sizeof (UTP_QUERY_REQ_UPIU)), sizeof (UINT32));\n",
    "    Trd->RuL = STARBITE_RESP_LEN;\n"
    "    Trd->RuO = STARBITE_UCD_RESP_OFF;\n")

# NOP OUT descriptor.
sub("  TotalLen = ROUNDUP8 (sizeof (UTP_NOP_OUT_UPIU)) + ROUNDUP8 (sizeof (UTP_NOP_IN_UPIU));\n",
    "  TotalLen = STARBITE_UCD_RESP_OFF + STARBITE_RESP_LEN;\n")
sub("  Trd->RuL    = (UINT16)DivU64x32 ((UINT64)ROUNDUP8 (sizeof (UTP_NOP_IN_UPIU)), sizeof (UINT32));\n"
    "  Trd->RuO    = (UINT16)DivU64x32 ((UINT64)ROUNDUP8 (sizeof (UTP_NOP_OUT_UPIU)), sizeof (UINT32));\n",
    "  Trd->RuL    = STARBITE_RESP_LEN;\n"
    "  Trd->RuO    = STARBITE_UCD_RESP_OFF;\n")

# Everything that reads the offsets back now gets bytes.
sub("Trd->RuO * sizeof (UINT32)", "Trd->RuO", count=s.count("Trd->RuO * sizeof (UINT32)"))
sub("Trd->RuL * sizeof (UINT32)", "Trd->RuL", count=s.count("Trd->RuL * sizeof (UINT32)"))
assert "* sizeof (UINT32)" not in "".join(l for l in s.splitlines() if "Trd->Ru" in l or "Trd->Prdt" in l)
open(PATH, "w", newline="").write(s)
print("ufs byte-gran layout added")
