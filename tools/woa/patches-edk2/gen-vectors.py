#!/usr/bin/env python3
"""Add Starbite catch-all exception vectors to edk2's MemoryInitPeiLib (on top
of 0001's breadcrumbs). Any exception during the MMU bring-up logs
"X ESR ELR FAR SPSR" into the pmsg ring and parks the CPU.
Run in the edk2 checkout; the result is kept as a patch in this directory."""
PATH = "ArmPlatformPkg/MemoryInitPei/MemoryInitPeiLib.c"
s = open(PATH, newline="").read()
nl = "\r\n" if "\r\n" in s else "\n"
vectors = "".join('  ".balign 128\\n b StarbiteVecEntry\\n"\n' for _ in range(16))
code = r'''
/* Starbite: catch-all exception vectors for the MMU bring-up. Any exception
   logs "X" + ESR, ELR, FAR, SPSR into the pmsg ring, then parks the CPU. */
static void StarbiteHex (unsigned long V)
{
  int I;
  for (I = 60; I >= 0; I -= 4) {
    StarbiteCrumb ("0123456789abcdef"[(V >> I) & 15]);
  }
}

void StarbiteFault (unsigned long Esr, unsigned long Elr, unsigned long Far, unsigned long Spsr)
{
  StarbiteCrumb ('\n'); StarbiteCrumb ('X');
  StarbiteCrumb (' '); StarbiteHex (Esr);
  StarbiteCrumb (' '); StarbiteHex (Elr);
  StarbiteCrumb (' '); StarbiteHex (Far);
  StarbiteCrumb (' '); StarbiteHex (Spsr);
  StarbiteCrumb ('\n');
  for (;;) {
    __asm__ __volatile__ ("wfe");
  }
}

__asm__ (
  ".text\n"
  ".balign 2048\n"
  ".globl StarbiteVectors\n"
  "StarbiteVectors:\n"
VECTORS  "StarbiteVecEntry:\n"
  " mrs x0, esr_el1\n mrs x1, elr_el1\n mrs x2, far_el1\n mrs x3, spsr_el1\n"
  " b StarbiteFault\n"
);
extern char StarbiteVectors[];
'''.replace("VECTORS", vectors)
anchor = "static inline void StarbiteCrumb (char C)"
i = s.index(anchor)
j = s.index(nl + "}" + nl, i) + len(nl) + 1 + len(nl)   # the function's closing brace
s = s[:j] + code.replace("\n", nl) + s[j:]
old = "  StarbiteCrumb ('M');" + nl
new = (old + '  __asm__ __volatile__ ("msr vbar_el1, %0\\n isb" : : "r" (StarbiteVectors) : "memory");' + nl
       + "  StarbiteCrumb ('V');" + nl)
assert s.count(old) == 1
s = s.replace(old, new)
open(PATH, "w", newline="").write(s)
print("vectors added")
