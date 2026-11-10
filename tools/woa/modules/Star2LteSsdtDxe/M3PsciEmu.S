// M3PsciEmu: PSCI 1.0 answers for Windows on the M3 (docs/WIN11-BIGCORES.md, v10 / STARBITE_M3_PSCIEMU).
// SPDX-License-Identifier: GPL-2.0-only
//
// Assembled by gen-stubs.py into M3PsciEmu.h. M3Boot.c copies it into the zero slack at the end of ntoskrnl's .text
// section (non-paged, executable, mapped with .text) and replaces the `smc #0` of ntoskrnl's one generic SMC wrapper
// (sub sp,sp,#0x10; str x7,[sp]; SMC #0; ldr x7,[sp]; str x0,[x7]) with a branch here. The final `b .` is patched
// to branch back to the instruction after that SMC.
//
// Why: Samsung's EL3 answers PSCI_VERSION with NOT_SUPPORTED, and on A55 core 0 the woa shim at EL2 answers PSCI 1.0
// instead (trapping SMCs); the M3 has no shim (EL3 installs its payload on one core only). This gives Windows on the M3
// the same two answers the shim gives (relmods/Star2LtePsciEl2ShimStage0Proven payload, 0x19c0..0x1aa4):
//   PSCI_VERSION (0x84000000)  -> 0x10000 (1.0)
//   PSCI_FEATURES (0x8400000A) -> 0 for VERSION, FEATURES, CPU_OFF, AFFINITY_INFO (32/64), CPU_SUSPEND64, CPU_ON64;
//                                 -1 (NOT_SUPPORTED) for anything else
// and passes every other call to EL3 unchanged. x16/x17 are scratch (SMCCC lets the callee clobber them).

        .text
        .global m3psciemu_stub
        .balign 4

m3psciemu_stub:
        movz    w16, #0
        movk    w16, #0x8400, lsl #16   // 0x84000000
        cmp     w0, w16
        b.eq    1f
        add     w17, w16, #0xa
        cmp     w0, w17
        b.eq    2f
        smc     #0                      // everything else: Samsung's EL3
        b       9f
1:      mov     x0, #0x10000            // PSCI 1.0
        b       9f
2:      cmp     w1, w16                 // FEATURES: x1 = the queried function
        b.eq    3f
        cmp     w1, w17
        b.eq    3f
        add     w17, w16, #2            // CPU_OFF
        cmp     w1, w17
        b.eq    3f
        add     w17, w16, #4            // AFFINITY_INFO (32)
        cmp     w1, w17
        b.eq    3f
        movk    w16, #0xc400, lsl #16   // 0xC4000000
        add     w17, w16, #1            // CPU_SUSPEND (64)
        cmp     w1, w17
        b.eq    3f
        add     w17, w16, #3            // CPU_ON (64)
        cmp     w1, w17
        b.eq    3f
        add     w17, w16, #4            // AFFINITY_INFO (64)
        cmp     w1, w17
        b.eq    3f
        mov     x0, #-1                 // NOT_SUPPORTED
        b       9f
3:      mov     x0, #0
9:      b       9b                      // patched: B back to the wrapper

        .balign 8
lit:    .quad   0                       // unused (gen-stubs wants a literal block at the end)
