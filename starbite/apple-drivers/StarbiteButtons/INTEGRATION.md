# StarbiteButtons: integration (for the integrating session)

Built into the kernel for M8. Needs the open-source IOHIDFamily built in: that step is described once, in
`../StarbiteTouch/INTEGRATION.md` section 1 (same compile-check stand-in, `kcheck/IOKit/hid/IOHIDEvent.h`).

## Files

| File | How |
|---|---|
| `StarbiteButtons.cpp` | `optional iokitcpp`; include paths: this folder, the IOHIDFamily headers |
| `hw/sb_keys.c` | plain C, `standard` |
| `kcheck/` | compile check only: never build it |

Macros: `SB_HAVE_A5_POWER_PRESSED=1` (default) writes `a5xelte_power_pressed_at` (defined by
`bsd/dev/a5xelte_keys.c`, read by SweetFramebuffer.cpp to wake the panel) on every Power press; set 0 when
a5xelte_keys.c is removed. `SB_SWEET_HID_COMPAT=1` (default) defines `sweet_hid_key()` (SweetHID.cpp's entry,
key 0 power, 1 up, 2 down, 3 home), so `a5xelte_keys.c`, `sweet_keys.c` and `starlte_touch.c`'s scripts still
link once SweetHID.cpp is gone and their presses reach this service; set 0 when those files go.

## Personality (KernelConfigTables.cpp)

```
"   {"
"     'IOClass'         = StarbiteButtons;"
"     'IOProviderClass' = IOService;"
"     'IONameMatch'     = 'starbite,gpio-keys';"
"   },"
```
Remove the `SweetHIDButtons` personality (it goes with SweetHID.cpp).

## ADT: a keys node (tools/board/starlte.board + mkadt-board.py)

`/arm-io/keys` (no `reg`; the existing root `/buttons` node is MobileGestalt's identity node and stays as it is):

| Property | Value (ADT encoding) |
|---|---|
| `compatible` | `"starbite,gpio-keys"` |
| `starbite,gpio` | `"pinctrl-alive"` (the block whose first reg range is mapped) |
| `starbite,keys` | optional u32 quads `bank pin usage-page usage`; the S9's are `0x60 4 0x0c 0x30`, `0x20 3 0x0c 0xe9`, `0x20 4 0x0c 0xea`, `0x20 6 0x0c 0x40`. Without it the driver uses exactly this table (`hw/sb_keys.c`). |

## Today's code

| Today | M8 |
|---|---|
| `bsd/dev/a5xelte_keys.c` key thread | stays linked, boot with **`a5keys=0`** (otherwise both drivers report every key: the old thread through `sweet_hid_key()`, now this service) |
| `sweethidtest=N` (Home every N s) | `sbkeyscript=` (e.g. `30h,90h`) |

## Boot-args

`sbkeys=0` (do not start), `sbkeyscript=<s><p|u|d|h>,...` (presses of 80 ms, seconds from the driver's start).
The service publishes `StarbiteKeyEvents` (count); StarbiteTouch's M8 result line reads it.
