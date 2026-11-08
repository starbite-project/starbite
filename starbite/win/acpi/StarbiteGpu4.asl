/* SPDX-License-Identifier: GPL-2.0-only
 * The Galaxy S9's display engine and Mali-G72 for Windows: one ACPI device, \_SB.GPU0.
 * docs/WIN11-GPU.md 4.2. Compile with iasl (iasl -tc StarbiteGpu.asl) for an SSDT that our firmware
 * installs (EFI_ACPI_TABLE_PROTOCOL), or append the Device to a dump of the release DSDT for the
 * registry override test path.
 *
 * Interrupts are ACPI GSIVs = GIC SPI + 32 (the existing DSDT's UFS entry: SPI 227 = GSIV 259).
 * SPI numbers: ref/starlte-kernel/include/dt-bindings/interrupt-controller/exynos9810.h
 *   DECON0 frame start 182, frame done 183, extra 188; G3D job 282, mmu 281, gpu 283.
 * _CCA 0: neither DECON nor the Mali is IO-coherent (Mali COHERENCY_FEATURES = 0).
 *
 * STAGE 4 VARIANT (table revision 4, docs/WIN11-GPU.md 6.8): adds the Mali's three interrupt lines for sbwddm's
 * full mode (vendor DT mali@17500000: JOB SPI 282, MMU 281, GPU 283, level). sbwddm claims all three in one
 * ISR and masks/clears a stray line before bring-up; display-only drivers (sbdisplay, sbwddm's fallback mode)
 * do not connect interrupts. DECON's lines stay out (flips and vsync are emulated).
 */
DefinitionBlock ("", "SSDT", 2, "STRBTE", "SBGPU", 0x00000004)
{
    External (\_SB, DeviceObj)

    Scope (\_SB)
    {
        Device (GPU0)
        {
            Name (_HID, "STRB0072")
            Name (_UID, 0)
            Name (_CCA, 0)
            Name (_CRS, ResourceTemplate ()
            {
                Memory32Fixed (ReadWrite, 0x16030000, 0x00010000)   // DECON0 + blender windows
                Memory32Fixed (ReadWrite, 0x16070000, 0x00007000)   // DPU_DMA (IDMA channel n at +0x1000 n)
                Memory32Fixed (ReadWrite, 0x16080000, 0x00000100)   // DSIM0
                Memory32Fixed (ReadWrite, 0x17500000, 0x00005000)   // Mali-G72 MP18
                Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 314 }   // Mali JOB (SPI 282)
                Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 313 }   // Mali MMU (SPI 281)
                Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 315 }   // Mali GPU (SPI 283)
            })
            Method (_STA, 0, NotSerialized)
            {
                Return (0x0F)
            }
        }
    }
}
