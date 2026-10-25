#!/usr/bin/env python3
"""Add Starbite breadcrumbs (pmsg ring at 0xFED14000, read from TWRP's
/sys/fs/pstore/pmsg-ramoops-0) around the MMU bring-up in a clean edk2
checkout: MemoryPeim 'M'/'N' around InitMmu; ArmConfigureMmu '1' entry,
'2' TCR, '3' root table allocated, '4' zeroed, 'r' per region, '5' MAIR,
'6' TTBR0, '7' caches on, '8' MMU on. Preserves edk2's CRLF line endings.
  regen.sh runs this, then gen-vectors.py, and saves the diff."""
CRUMB = r"""
/* Starbite: breadcrumb into the ramoops pmsg ring (0xFED14000, "DBGC"),
   read from TWRP /sys/fs/pstore/pmsg-ramoops-0. Physical == virtual here. */
static inline void StarbiteCrumb (char C)
{
  volatile unsigned int   *W = (volatile unsigned int *)(unsigned long)0xFED14000UL;
  volatile unsigned char  *B = (volatile unsigned char *)(unsigned long)0xFED14000UL;
  unsigned int            S;
  if (W[0] != 0x43474244u) { W[0] = 0x43474244u; W[1] = 0u; W[2] = 0u; }
  S = W[2];
  if (S < 0x3F00u) { B[12u + S] = (unsigned char)C; W[2] = S + 1u; }
  __asm__ __volatile__ ("dsb sy" ::: "memory");
}
"""


def c(ch):
    return "StarbiteCrumb ('" + ch + "');"


def patch(path, edits, after_includes):
    s = open(path, newline="").read()
    nl = "\r\n" if "\r\n" in s else "\n"
    fix = lambda t: t.replace("\n", nl)
    idx = s.index(fix(after_includes)) + len(fix(after_includes))
    s = s[:idx] + fix(CRUMB) + s[idx:]
    for old, new in edits:
        old, new = fix(old), fix(new)
        assert s.count(old) == 1, (path, old)
        s = s.replace(old, new)
    open(path, "w", newline="").write(s)


patch("UefiCpuPkg/Library/ArmMmuLib/AArch64/ArmMmuLibCore.c", [
    ("  ASSERT (ArmReadCurrentEL () < AARCH64_EL3);\n  if (ArmReadCurrentEL () == AARCH64_EL3) {",
     "  " + c("1") + "\n  ASSERT (ArmReadCurrentEL () < AARCH64_EL3);\n  if (ArmReadCurrentEL () == AARCH64_EL3) {"),
    ("  ArmSetTCR (TCR);\n\n  // Allocate pages for translation table\n  TranslationTable = AllocatePages (1);",
     "  ArmSetTCR (TCR);\n  " + c("2") + "\n\n  // Allocate pages for translation table\n  TranslationTable = AllocatePages (1);\n  " + c("3")),
    ("  ZeroMem (TranslationTable, RootTableEntryCount * sizeof (UINT64));\n\n  while (MemoryTable->Length != 0) {\n",
     "  ZeroMem (TranslationTable, RootTableEntryCount * sizeof (UINT64));\n  " + c("4") + "\n\n  while (MemoryTable->Length != 0) {\n    " + c("r") + "\n"),
    ("    MAIR_ATTR (TT_ATTR_INDX_MEMORY_WRITE_BACK, MAIR_ATTR_NORMAL_MEMORY_WRITE_BACK)\n    );\n",
     "    MAIR_ATTR (TT_ATTR_INDX_MEMORY_WRITE_BACK, MAIR_ATTR_NORMAL_MEMORY_WRITE_BACK)\n    );\n  " + c("5") + "\n"),
    ("    ArmSetTTBR0 (TranslationTable);\n  }\n\n  if (!ArmMmuEnabled ()) {\n",
     "    ArmSetTTBR0 (TranslationTable);\n  }\n  " + c("6") + "\n\n  if (!ArmMmuEnabled ()) {\n"),
    ("    ArmEnableDataCache ();\n\n    ArmEnableMmu ();\n  }",
     "    ArmEnableDataCache ();\n    " + c("7") + "\n\n    ArmEnableMmu ();\n    " + c("8") + "\n  }"),
], "#include \"ArmMmuLibInternal.h\"\n")
patch("ArmPlatformPkg/MemoryInitPei/MemoryInitPeiLib.c", [
    ("  // Build Memory Allocation Hob\n  InitMmu (MemoryTable);",
     "  // Build Memory Allocation Hob\n  " + c("M") + "\n  InitMmu (MemoryTable);\n  " + c("N")),
], "#include <Library/PcdLib.h>\n")
print("crumbs added")
