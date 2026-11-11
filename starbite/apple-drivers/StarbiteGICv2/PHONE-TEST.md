# StarbiteGICv2: phone test (milestone M2)

The GIC has no test boot of its own: it is half of M2, run together with StarbitePMGR. The full procedure (build,
ADT, command line `sbstop=smp`, chained loader with its own 120 s dead-man, store row, `cpus=4` then `cpus=8`, how
to read the result) is in **../StarbitePMGR/PHONE-TEST.md**.

What M2 proves for this driver, and the lines to look for in pmsg:

| Proves | Line |
|---|---|
| The driver mapped and probed the GIC-400 and took the SPIs | `StarbiteGICv2: GIC-400 at 0x10101000/0x10102000: IIDR 0x... TYPER 0x..., N INTIDs, 8 CPU interfaces, security ext yes, ...` |
| AppleARMSMP got it as its interrupt controller | `StarbiteGICv2: attached to the platform interrupt controller (PassthruInterruptController): 0` |
| Every CPU read its own interface bit and has the handler | `StarbiteGICv2: all CPUs initialized; CPU interface bits: 0:0x.. 1:0x.. ...; SGI registrations 2N, strays 0` |
| IPIs through GICD_SGIR reach every core | `StarbitePMGR: ... IPI ok` |
| PPI 27 still works on every core (pexpert's half) | `StarbitePMGR: ... timer 26 MHz` |

Pass: the M2 PASS line, `strays 0`, and every CPU listed with a distinct interface bit and no "(GICC OFF)".
A non-zero `strays` count is not a failure for M2 but names an SPI firmware left enabled (it is masked at the
first hit); report the `stray INTID` kprintf lines.
