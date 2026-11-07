/*
 * SSDT for the Galaxy S9's Wi-Fi under Windows 11 (docs/WIFI-BCM4361.md, section 7): one ACPI device, \_SB.WLAN,
 * for SbWifi's WiFiCx driver to bind to (INF match ACPI\STBW4361). Windows cannot enumerate this PCIe controller
 * (not ECAM; the link is trained by software after the radio is powered by a GPIO), so the driver runs it itself:
 * the device carries only the PCIe channel 0 block's own pages, the endpoint's BAR window and configuration
 * window, and the controller's interrupt. The PMU, CMU_FSYS1, SYSREG_FSYS1 and GPIO words the bring-up needs are
 * mapped by the driver directly (single words; those pages belong to other devices of the DSDT, so they are not
 * claimed here to avoid a resource conflict).
 *
 * Built with iasl (ACPICA) into wlan.aml for the USB session's Star2LteSsdtDxe (a plain SSDT blob).
 */
DefinitionBlock ("", "SSDT", 2, "STRBTE", "WLAN4361", 0x00000001)
{
    Scope (\_SB)
    {
        Device (WLAN)
        {
            Name (_HID, "STBW4361")
            Name (_CID, "STBW4361")
            Name (_UID, Zero)
            Name (_CCA, Zero)                   // DMA is not cache coherent (uncached buffers)
            Method (_STA, 0, NotSerialized)
            {
                Return (0x0F)
            }
            Name (_CRS, ResourceTemplate ()
            {
                Memory32Fixed (ReadWrite, 0x116A0000, 0x00001000)  // ELBI (Samsung's glue)
                Memory32Fixed (ReadWrite, 0x116C0000, 0x00001000)  // PCS
                Memory32Fixed (ReadWrite, 0x116D0000, 0x00001000)  // PHY (PMA)
                Memory32Fixed (ReadWrite, 0x11700000, 0x00001000)  // DBI (root port, iATU viewport)
                Memory32Fixed (ReadWrite, 0x11800000, 0x00F00000)  // the endpoint's BAR window
                Memory32Fixed (ReadWrite, 0x127FE000, 0x00001000)  // type 0 configuration window
                Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 285 }  // SPI 253: INTx
            })
        }
    }
}
