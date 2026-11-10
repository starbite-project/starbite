// Cores8ParkStub: an M3 core parked for Windows (docs/WIN11-24H2-A55.md, "All 8 cores").
// SPDX-License-Identifier: GPL-2.0-only
//
// Assembled by gen-stubs.py into Cores8ParkStub.h; Cores8.c copies it to mailbox + 0x800 (the firmware half of an
// ACPI parking-protocol mailbox, ACPI 6.4 5.2.12.14) and fills the literals at its end. Entered by PSCI CPU_ON
// during DXE dispatch (before the release's PSCI EL2 shim, as in B2/B2b): EL1, MMU and caches off (every data access
// is Device memory: aligned accesses only), DAIF masked. No stack; x30 only for the leaf `digit`.
//
// Mailbox: +0x000 CpuId (u32), +0x008 JumpAddress (u64), +0x010..0x7FF the OS's, +0x800..0xFFF ours (this code,
// then the MPIDR at +0xFF8 as the arrival mark the firmware polls).
//
// B2b measured on these cores: CurrentEL 1, CNTFRQ_EL0 0, the event stream (CNTKCTL_EL1 EVNTEN, EVNTI 15: a wake-up
// every 2^16 CNTVCT ticks, ~2.5 ms at 26 MHz) works and WFE returns ~1,060 times a second for a whole Windows session.
// So the core sleeps in WFE and checks the mailbox after every wake-up; Windows' doorbell SGI cannot wake a WFE with
// IRQs masked, but the event stream bounds the delay to ~2.5 ms.
//
// The zone-0 line "=C8 park core n ... state S" gets one digit S: 1 parked (waiting), 2 the OS took the core
// (written just before the jump, so after a Windows session it tells which M3s Windows started).
//
// Hand-off (ACPI parking protocol): JumpAddress != 0 and CpuId == our UID -> JumpAddress = 0 (the acknowledgement
// the OS waits for), then jump to it with x0 = the mailbox address, still EL1 / MMU off / caches off / DAIF masked.

        .text
        .global cores8park_stub
        .balign 4

cores8park_stub:
        adr     x20, lit
        ldr     x19, [x20, #0]          // mailbox page
        ldr     w24, [x20, #8]          // our ACPI processor UID (MADT GICC)
        mrs     x0, mpidr_el1
        str     x0, [x19, #0xff8]       // arrival mark
        dsb     sy
        mrs     x0, cntkctl_el1         // event stream: EVNTEN = 1, EVNTDIR = 0, EVNTI = 15
        bic     x0, x0, #0xfc
        orr     x0, x0, #0xf0
        orr     x0, x0, #0x4
        msr     cntkctl_el1, x0
        isb
        mov     x0, #1                  // state 1: parked
        bl      digit

1:      wfe
        ldr     x1, [x19, #8]           // JumpAddress
        cbz     x1, 1b
        ldr     w0, [x19, #0]           // CpuId
        cmp     w0, w24
        b.ne    1b
        mov     x25, x1
        mov     x0, #2                  // state 2: taken by the OS
        bl      digit
        str     xzr, [x19, #8]          // acknowledge: JumpAddress = 0
        dsb     sy
        mov     x0, x19
        br      x25

// digit: x0 = 0..9 -> the state digit (literal +16; 0 = no line found: skip). Clobbers x1.
digit:  ldr     x1, [x20, #16]
        cbz     x1, 9f
        add     x0, x0, #0x30
        strb    w0, [x1]
        dsb     sy
9:      ret

        .balign 8
lit:    .quad   0                       // +0  mailbox page
        .quad   0                       // +8  UID
        .quad   0                       // +16 the state digit's address in the zone-0 ring (0: none)
