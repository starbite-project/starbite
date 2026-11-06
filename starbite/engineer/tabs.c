/* SPDX-License-Identifier: GPL-2.0-only
 * Engineer OS tabs: what each tab root lists and what each row opens. Every page of the loader's
 * EngineerMode (shim/starlte/em.c build_tab / open_item) is here, regrouped by subject, plus the
 * Starbite Lab tests (lab.c) and utilities (util.c) of v2.
 */
#include "eng.h"

static const char *const tab_hint[NTABS] = {
    "",
    "health check, CPU / memory / thermal / battery stress, display, touch",
    "battery tests, charger, PMIC regulators and writes",
    "CPU, clocks, every thermal sensor, GPU, RTC, timer",
    "UFS, partitions and LUs, backups, sectors, microSD",
    "OS store, catalog, settings editor, start an OS",
    "Engineer OS log, pmsg, loader handoff, audit",
    "armed mode, peek / poke, memory, reboot to anything",
};

void tab_build(int t)
{
    switch (t) {
    case 0:
        dash_fill();
        kv_hdr("DEVICE");
        row_add("Device information", "model, serials, S-Boot, Knox bit, locks", 0);
        row_add("SoC and CPU", "Exynos 9810 chip ID, core, caches, ISA", 0);
        row_add("Memory map", "DRAM banks, carve-outs, Engineer OS layout", 0);
        row_add("Reset and boot reason", "PMU RST_STAT, INFORM, PMIC sources", 0);
        row_add("Battery graph", "fuel gauge current and voltage, live", 0);
        /* The other tabs as rows: with Volume and Power alone (no touch, no Bixby) the tab bar
         * cannot be reached, so the Dashboard is the hub (hold Power at a tab's root comes back
         * here). Rows 5..11 = tabs 1..7. */
        kv_hdr("TABS");
        for (int i = 1; i < NTABS; ++i)
            row_add(tab_title_of(i), tab_hint[i], 0);
        kv_hdr("LEAVE");
        row_add("Exit to the boot menu", "reboot: S-Boot, then the Starbite menu", 0);
        kv_note("Keys: Volume moves, Power selects, hold Power (or Bixby) goes back. Every test stops on "
                "Power held.");
        break;
    case 1:
        kv_hdr("STARBITE LAB");
        row_add("Full health check", "15 quick checks with a pass / fail report", 0);
        row_add("CPU stress", "all 8 cores: integer, FP, NEON, caches, checked", R_WARN);
        row_add("Memory stress", "patterns, walking bits, random, bandwidth", 0);
        row_add("Thermal test", "every sensor vs time under load, throttling", R_WARN);
        row_add("Battery tests", "discharge, voltage sag, charge, capacity", R_WARN);
        row_add("Storage test", "UFS / card read benchmark and verify", 0);
        row_add("Display stress", "full-screen fills, scrolls, flips (no GPU)", 0);
        row_add("Touch grid", "touch every cell of a 6 x 12 grid", 0);
        kv_hdr("QUICK TESTS");
        row_add("Core bring-up test", "PSCI CPU_ON each core, check-in, CPU_OFF", R_WARN);
        row_add("Memory test", "16 MiB of DRAM, non-cacheable window", 0);
        row_add("Memory bandwidth", "DRAM and cached, read, write, copy", 0);
        row_add("Display test", "colour fields, grey ramp, checkerboard, grid", 0);
        row_add("Display panel", "panel, resolution, framebuffer", 0);
        row_add("Brightness", "panel AOR via DSI (asks first), panel ID", R_WARN);
        row_add("Touch screen", "S6SY761: ten-finger paint, controller info", 0);
        row_add("Keys", "live key states", 0);
        row_add("Speed test", "UFS 16 MiB sequential read", 0);
        kv_hdr("RESULTS");
        row_add("Test history", "this session and earlier ones (pmsg)", 0);
        row_add("Save report to the OS store", "results, thermal log, log: one text entry", R_WARN);
        break;
    case 2:
        kv_hdr("BATTERY");
        row_add("Battery and charger", "MAX77705 fuel gauge + charger (live)", 0);
        row_add("Battery graph", "current and voltage over time", 0);
        row_add("Battery tests", "health, discharge, sag, charge, capacity", R_WARN);
        kv_hdr("PMIC");
        row_add("PMIC regulators", "S2MPS18 LDO1-45 via ACPM", 0);
        row_add("PMIC register dump", "S2MPS18 PMIC + TOP, hex", 0);
        row_add("Touch rails", "LDO35 / LDO43: state, switch on (tier 1)", R_WARN);
        row_add("PMIC and charger writes", "any register: tier 1 to 3", R_RED);
        kv_hdr("SYSTEM");
        row_add("Reboot and power", "menu, recovery, download, power off", 0);
        break;
    case 3:
        kv_hdr("CPU AND GPU");
        row_add("CPU", "clusters and core map", 0);
        row_add("Clocks", "CPU, GPU, memory and bus PLLs (live)", 0);
        row_add("Temperatures", "TMU: big, little, GPU, ISP (live)", 0);
        row_add("Every thermal sensor", "all TMU probes, thermistors, fuel gauge", 0);
        row_add("GPU (Mali-G72)", "power, voltage, ID, cores (read only)", 0);
        kv_hdr("TIME");
        row_add("Real-time clock", "S2MPS18 RTC, live", 0);
        row_add("Timer", "generic timer vs RTC", 0);
        kv_note("Clocks are read here. Writing a CMU or PMU register is Tools > Register peek / poke (tier 3).");
        break;
    case 4:
        kv_hdr("UFS");
        row_add("UFS host controller", "UFSHCI 2.1 + Exynos vs_hci, live", 0);
        row_add("UFS device and health", "model, firmware, capacity, wear", 0);
        row_add("Partitions", "live GPT: every partition with size", 0);
        row_add("GPT viewer", "both headers, CRCs, entries", 0);
        row_add("Partitions and LUs", "hexdump, back up, restore, erase", R_WARN);
        row_add("Sector viewer", "raw LU0 blocks, hex + ASCII, edit", 0);
        row_add("Speed test", "16 MiB sequential read (read only)", 0);
        row_add("Storage test", "benchmark and verify, UFS or card", 0);
        kv_hdr("MICROSD");
        row_add("microSD card", "the card, its OS store, power it up", 0);
        row_add("Card store at boot", "settings flag (tier 1)", R_WARN);
        break;
    case 5:
        kv_hdr("STARBITE");
        row_add("OS store", "inspect, verify, copy, rename, remove", 0);
        row_add("Catalog", "every entry: method, files, ready", 0);
        row_add("Settings record", "default, last, next, timeout, flags", 0);
        row_add("Settings editor", "default OS, timeout, POST and log level", R_WARN);
        row_add("Start an OS", "boot any installed system (boot-next)", 0);
        row_add("Write fence", "what each kind of target needs", 0);
        kv_hdr("SYSTEMS");
        row_add("Apple", "iOS / macOS entries, store files", 0);
        row_add("iOS boot options", "decoded command lines", 0);
        row_add("Android 17", "LineageOS 24.0: store, partitions, safety", 0);
        row_add("Linux", "postmarketOS, ChromiumOS, Starbite shell", 0);
        row_add("Windows 11", "firmware, last run report", 0);
        break;
    case 6:
        kv_hdr("LOGS");
        row_add("Engineer OS log", "this session's log ring", 0);
        row_add("Boot log (pmsg)", "ring at 0xFED14000", 0);
        row_add("Loader handoff", "the loader's log page 0x91FFF000", 0);
        row_add("Test history", "results kept in pmsg", 0);
        row_add("Audit log", "confirmations, writes, backups, refusals", 0);
        kv_hdr("DEBUG");
        row_add("Retained records", "RWD1, P3, latches (hex)", 0);
        row_add("Kernel command lines", "S-Boot bootargs, catalog command lines", 0);
        row_add("Registers", "PMU, watchdog, GPIO, UFS (live)", 0);
        break;
    default:
        kv_hdr("ENGINEER");
        row_add("Armed mode", "free-form writes; dry run; undo; tiers", R_WARN);
        row_add("Register peek / poke", "any address; writes tier 1 to 3", R_WARN);
        row_add("Memory dump / fill", "any DRAM; ECT and Engineer OS impossible", R_WARN);
        row_add("PMIC and charger writes", "S2MPS18 and MAX77705 registers", R_RED);
        row_add("Partitions and LUs", "hexdump, back up, restore, erase", R_WARN);
        row_add("OS store", "inspect, verify, copy, rename, remove", 0);
        row_add("Settings editor", "default OS, timeout, POST and log level", R_WARN);
        row_add("Save report to the OS store", "this session's results as text", R_WARN);
        kv_hdr("LEAVE");
        row_add("Exit to the boot menu", "reboot: S-Boot, then the Starbite menu", 0);
        row_add("Reboot to", "menu, Engineer OS, any OS, recovery, rescue, Odin, off", 0);
        kv_hdr("COMING");
        row_add("Radio", "modem bench (later)", 0);
        row_add("Installer", "make a drive, flash another S9 (EN8)", 0);
        kv_hdr("ABOUT");
        row_add("About Engineer OS", "version, build, keys, credits", 0);
        break;
    }
}

void tab_open(int t, int i)
{
    switch (t * 32 + i) {
    case 0x00: info_screen("Device information", fill_device_info, 0); break;
    case 0x01: info_screen("SoC and CPU", fill_soc, 0); break;
    case 0x02: info_screen("Memory map", fill_memory, 0); break;
    case 0x03: info_screen("Reset and boot reason", fill_reset, 0); break;
    case 0x04: dash_battery_page(); break;
    case 0x05: case 0x06: case 0x07: case 0x08: case 0x09: case 0x0a: case 0x0b:
        E.home = 1;
        E.tab_req = i - 4;
        break;
    case 0x0c: eng_exit_to_menu(); break;
    /* Tests */
    case 0x20: lab_health(); break;
    case 0x21: lab_cpu_stress(); break;
    case 0x22: lab_mem_stress(); break;
    case 0x23: lab_thermal(); break;
    case 0x24: lab_battery(); break;
    case 0x25: lab_storage_bench(); break;
    case 0x26: lab_display_stress(); break;
    case 0x27: lab_touch_grid(); break;
    case 0x28: run_cores_dialog(); break;
    case 0x29: run_ram_dialog(); break;
    case 0x2a: run_bw(); break;
    case 0x2b: test_display(); break;
    case 0x2c: info_screen("Display panel", fill_panel, 0); break;
    case 0x2d: bright_page(); break;
    case 0x2e: test_touch(); break;
    case 0x2f: test_keys(); break;
    case 0x30: run_speed(); break;
    case 0x31: tests_history(); break;
    case 0x32: lab_save_report(); break;
    /* Power */
    case 0x40: info_screen("Battery and charger", fill_battery, 2000); break;
    case 0x41: dash_battery_page(); break;
    case 0x42: lab_battery(); break;
    case 0x43: info_screen("PMIC regulators", fill_pmic, 0); break;
    case 0x44: dump_pmic(); break;
    case 0x45: touch_rails(); break;
    case 0x46: util_pmic(); break;
    case 0x47: reboot_page(); break;
    /* Clocks */
    case 0x60: info_screen("CPU", fill_cpu, 0); break;
    case 0x61: info_screen("Clocks", fill_clocks, 2000); break;
    case 0x62: info_screen("Temperatures", fill_thermal, 1000); break;
    case 0x63: lab_sensors(); break;
    case 0x64: info_screen("GPU (Mali-G72)", fill_gpu, 1000); break;
    case 0x65: info_screen("Real-time clock", fill_rtc, 500); break;
    case 0x66:
        measure_timer();
        info_screen("Timer", fill_timer, 500);
        break;
    /* Storage */
    case 0x80: info_screen("UFS host controller", fill_ufs, 1000); break;
    case 0x81: info_screen("UFS device and health", fill_ufs_dev, 0); break;
    case 0x82: info_screen("Partitions", fill_partitions, 0); break;
    case 0x83: sb_pages_gpt(); break;
    case 0x84: util_partitions(); break;
    case 0x85: util_sector(SB_MED_UFS, 0, 1); break;
    case 0x86: run_speed(); break;
    case 0x87: lab_storage_bench(); break;
    case 0x88: sd_page(); break;
    case 0x89: sdboot_page(); break;
    /* Starbite */
    case 0xa0: util_store(); break;
    case 0xa1: sb_pages_catalog(); break;
    case 0xa2: sb_pages_settings(); break;
    case 0xa3: util_settings(); break;
    case 0xa4: start_page(); break;
    case 0xa5: sb_pages_fence(); break;
    case 0xa6: info_screen("Apple", fill_ios, 0); break;
    case 0xa7: ios_opts_page(); break;
    case 0xa8: info_screen("Android 17", fill_android, 0); break;
    case 0xa9: info_screen("Linux", fill_linux, 0); break;
    case 0xaa: info_screen("Windows 11", fill_windows, 0); break;
    /* Logs */
    case 0xc0: logs_engineer(); break;
    case 0xc1: show_pmsg(); break;
    case 0xc2: logs_loader(); break;
    case 0xc3: tests_history(); break;
    case 0xc4: audit_page(); break;
    case 0xc5: show_records(); break;
    case 0xc6: show_cmdline(); break;
    case 0xc7: info_screen("Registers", fill_regs, 500); break;
    /* Tools */
    case 0xe0: armed_page(); break;
    case 0xe1: util_peek(); break;
    case 0xe2: util_memory(); break;
    case 0xe3: util_pmic(); break;
    case 0xe4: util_partitions(); break;
    case 0xe5: util_store(); break;
    case 0xe6: util_settings(); break;
    case 0xe7: lab_save_report(); break;
    case 0xe8: eng_exit_to_menu(); break;
    case 0xe9: util_reboot(); break;
    case 0xea: sb_pages_coming("Radio"); break;
    case 0xeb: sb_pages_coming("Installer"); break;
    case 0xec: sb_pages_about(); break;
    default: break;
    }
}
