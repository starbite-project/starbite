/* SPDX-License-Identifier: GPL-2.0-only
 * Engineer OS tabs: what each tab root lists and what each row opens. Every page of the loader's
 * EngineerMode (shim/starlte/em.c build_tab / open_item) is here, regrouped by subject.
 */
#include "eng.h"

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
        break;
    case 1:
        kv_hdr("PROCESSOR AND MEMORY");
        row_add("Core bring-up test", "PSCI CPU_ON each core, check-in, CPU_OFF", R_WARN);
        row_add("Memory test", "16 MiB of DRAM, non-cacheable window", 0);
        row_add("Memory bandwidth", "DRAM and cached, read, write, copy", 0);
        kv_hdr("DISPLAY AND INPUT");
        row_add("Display test", "colour fields, grey ramp, checkerboard, grid", 0);
        row_add("Display panel", "panel, resolution, framebuffer", 0);
        row_add("Brightness", "panel AOR via DSI, panel ID", 0);
        row_add("Touch screen", "S6SY761: ten-finger paint, controller info", 0);
        row_add("Keys", "live key states", 0);
        kv_hdr("STORAGE");
        row_add("Speed test", "UFS 16 MiB sequential read", 0);
        kv_hdr("RESULTS");
        row_add("Test history", "this session and earlier ones (pmsg)", 0);
        break;
    case 2:
        kv_hdr("BATTERY");
        row_add("Battery and charger", "MAX77705 fuel gauge + charger (live)", 0);
        row_add("Battery graph", "current and voltage over time", 0);
        kv_hdr("PMIC");
        row_add("PMIC regulators", "S2MPS18 LDO1-45 via ACPM", 0);
        row_add("PMIC register dump", "S2MPS18 PMIC + TOP, hex", 0);
        row_add("Touch rails", "LDO35 / LDO43: state, switch on (tier 1)", R_WARN);
        kv_hdr("SYSTEM");
        row_add("Reboot and power", "menu, recovery, download, power off", 0);
        break;
    case 3:
        kv_hdr("CPU AND GPU");
        row_add("CPU", "clusters and core map", 0);
        row_add("Clocks", "CPU, GPU, memory and bus PLLs (live)", 0);
        row_add("Temperatures", "TMU: big, little, GPU, ISP (live)", 0);
        row_add("GPU (Mali-G72)", "power, voltage, ID, cores (read only)", 0);
        kv_hdr("TIME");
        row_add("Real-time clock", "S2MPS18 RTC, live", 0);
        row_add("Timer", "generic timer vs RTC", 0);
        kv_note("Clock control (EN5) is not in v1: every clock here is read, never set.");
        break;
    case 4:
        kv_hdr("UFS");
        row_add("UFS host controller", "UFSHCI 2.1 + Exynos vs_hci, live", 0);
        row_add("UFS device and health", "model, firmware, capacity, wear", 0);
        row_add("Partitions", "live GPT: every partition with size", 0);
        row_add("GPT viewer", "both headers, CRCs, entries", 0);
        row_add("Sector viewer", "raw LU0 blocks, hex + ASCII", 0);
        row_add("Speed test", "16 MiB sequential read (read only)", 0);
        kv_hdr("MICROSD");
        row_add("microSD card", "the card and its OS store (read only)", 0);
        row_add("Card store at boot", "settings flag (tier 1, dry-run)", R_WARN);
        break;
    case 5:
        kv_hdr("STARBITE");
        row_add("OS store", "entries on UFS and card, sizes, CRCs", 0);
        row_add("Catalog", "every entry: method, files, ready", 0);
        row_add("Settings record", "default, last, next, timeout, flags", 0);
        row_add("Start an OS", "boot any installed system (boot-next)", 0);
        row_add("Write fence", "what a write would need (dry-run)", 0);
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
        row_add("Audit log", "armed mode and dry runs", 0);
        kv_hdr("DEBUG");
        row_add("Retained records", "RWD1, P3, latches (hex)", 0);
        row_add("Kernel command lines", "S-Boot bootargs, catalog command lines", 0);
        row_add("Registers", "PMU, watchdog, GPIO, UFS (live)", 0);
        break;
    default:
        kv_hdr("ENGINEER");
        row_add("Armed mode", "tier 0 read only; arm for dry runs", R_WARN);
        row_add("Register peek", "allowlisted blocks; poke is dry-run", R_WARN);
        row_add("Reboot and power", "menu, recovery, download, power off", 0);
        kv_hdr("COMING");
        row_add("Stress", "all cores, RAM, GPU with a thermal guard (v2)", 0);
        row_add("Radio", "modem bench (later)", 0);
        row_add("Installer", "make a drive, flash another S9 (EN8)", 0);
        kv_hdr("ABOUT");
        row_add("About Engineer OS", "version, build, keys, credits", 0);
        break;
    }
}

void tab_open(int t, int i)
{
    switch (t * 16 + i) {
    case 0x00: info_screen("Device information", fill_device_info, 0); break;
    case 0x01: info_screen("SoC and CPU", fill_soc, 0); break;
    case 0x02: info_screen("Memory map", fill_memory, 0); break;
    case 0x03: info_screen("Reset and boot reason", fill_reset, 0); break;
    case 0x04: dash_battery_page(); break;
    case 0x10: run_cores_dialog(); break;
    case 0x11: run_ram_dialog(); break;
    case 0x12: run_bw(); break;
    case 0x13: test_display(); break;
    case 0x14: info_screen("Display panel", fill_panel, 0); break;
    case 0x15: bright_page(); break;
    case 0x16: test_touch(); break;
    case 0x17: test_keys(); break;
    case 0x18: run_speed(); break;
    case 0x19: tests_history(); break;
    case 0x20: info_screen("Battery and charger", fill_battery, 2000); break;
    case 0x21: dash_battery_page(); break;
    case 0x22: info_screen("PMIC regulators", fill_pmic, 0); break;
    case 0x23: dump_pmic(); break;
    case 0x24: touch_rails(); break;
    case 0x25: reboot_page(); break;
    case 0x30: info_screen("CPU", fill_cpu, 0); break;
    case 0x31: info_screen("Clocks", fill_clocks, 2000); break;
    case 0x32: info_screen("Temperatures", fill_thermal, 1000); break;
    case 0x33: info_screen("GPU (Mali-G72)", fill_gpu, 1000); break;
    case 0x34: info_screen("Real-time clock", fill_rtc, 500); break;
    case 0x35:
        measure_timer();
        info_screen("Timer", fill_timer, 500);
        break;
    case 0x40: info_screen("UFS host controller", fill_ufs, 1000); break;
    case 0x41: info_screen("UFS device and health", fill_ufs_dev, 0); break;
    case 0x42: info_screen("Partitions", fill_partitions, 0); break;
    case 0x43: sb_pages_gpt(); break;
    case 0x44: sector_screen(); break;
    case 0x45: run_speed(); break;
    case 0x46: sd_page(); break;
    case 0x47: sdboot_page(); break;
    case 0x50: info_screen("OS store", fill_store, 0); break;
    case 0x51: sb_pages_catalog(); break;
    case 0x52: sb_pages_settings(); break;
    case 0x53: start_page(); break;
    case 0x54: sb_pages_fence(); break;
    case 0x55: info_screen("Apple", fill_ios, 0); break;
    case 0x56: ios_opts_page(); break;
    case 0x57: info_screen("Android 17", fill_android, 0); break;
    case 0x58: info_screen("Linux", fill_linux, 0); break;
    case 0x59: info_screen("Windows 11", fill_windows, 0); break;
    case 0x60: logs_engineer(); break;
    case 0x61: show_pmsg(); break;
    case 0x62: logs_loader(); break;
    case 0x63: tests_history(); break;
    case 0x64: audit_page(); break;
    case 0x65: show_records(); break;
    case 0x66: show_cmdline(); break;
    case 0x67: info_screen("Registers", fill_regs, 500); break;
    case 0x70: armed_page(); break;
    case 0x71: peek_screen(); break;
    case 0x72: reboot_page(); break;
    case 0x73: sb_pages_coming("Stress"); break;
    case 0x74: sb_pages_coming("Radio"); break;
    case 0x75: sb_pages_coming("Installer"); break;
    case 0x76: sb_pages_about(); break;
    default: break;
    }
}
