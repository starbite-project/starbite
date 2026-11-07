/* SPDX-License-Identifier: GPL-2.0-only
 *
 * sbdevnode: create (or remove) a root-enumerated device node and install a driver on it, what devcon's
 * "install" did (the ARM64 WDK has no devcon). For the Starbite Display IddCx driver:
 *
 *   sbdevnode install C:\starbite\idd\StarbiteDisplay.inf Root\StarbiteDisplay
 *   sbdevnode remove  Root\StarbiteDisplay
 *   sbdevnode list    Root\StarbiteDisplay
 *
 * install: if no device with that hardware id exists yet, SetupDiCreateDeviceInfo(ROOT\STARBITEDISPLAY\NNNN)
 * + hardware id + DIF_REGISTERDEVICE; then UpdateDriverForPlugAndPlayDevices(INF, INSTALLFLAG_FORCE), which
 * stages the package in the DriverStore (catalog + trusted signer needed: INSTALL.md) and starts the device.
 * Idempotent. Exit code 0 = ok, 1 = failed (the Win32 error is printed), 2 = usage. Must run elevated
 * (LocalSystem from the one-shot install service is fine).
 */
#include <windows.h>
#include <setupapi.h>
#include <newdev.h>
#include <stdio.h>
#include <wchar.h>

static void fail(const char *what)
{
    DWORD e = GetLastError();
    printf("sbdevnode: %s failed: error %lu (0x%08lx)\n", what, e, e);
}

/* Does this device's hardware id list contain hwid (case-insensitive)? */
static int has_hwid(HDEVINFO set, SP_DEVINFO_DATA *d, const wchar_t *hwid)
{
    wchar_t buf[1024];
    DWORD type = 0, need = 0;
    if (!SetupDiGetDeviceRegistryPropertyW(set, d, SPDRP_HARDWAREID, &type, (BYTE *)buf, sizeof buf - 4, &need))
        return 0;
    buf[_countof(buf) - 1] = buf[_countof(buf) - 2] = 0;
    for (wchar_t *p = buf; *p; p += wcslen(p) + 1)
        if (!_wcsicmp(p, hwid))
            return 1;
    return 0;
}

/* Calls fn on every device (present or not) with this hardware id; returns how many matched. */
static int for_each(const wchar_t *hwid, int (*fn)(HDEVINFO, SP_DEVINFO_DATA *))
{
    HDEVINFO set = SetupDiGetClassDevsW(NULL, NULL, NULL, DIGCF_ALLCLASSES);
    if (set == INVALID_HANDLE_VALUE) {
        fail("SetupDiGetClassDevs");
        return -1;
    }
    int n = 0;
    SP_DEVINFO_DATA d = {sizeof d};
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &d); i++) {
        if (!has_hwid(set, &d, hwid))
            continue;
        n++;
        if (fn && !fn(set, &d))
            n = -1000;
    }
    SetupDiDestroyDeviceInfoList(set);
    return n;
}

static int print_dev(HDEVINFO set, SP_DEVINFO_DATA *d)
{
    wchar_t id[512];
    if (SetupDiGetDeviceInstanceIdW(set, d, id, _countof(id), NULL))
        printf("sbdevnode: device %ls\n", id);
    return 1;
}

static int remove_dev(HDEVINFO set, SP_DEVINFO_DATA *d)
{
    print_dev(set, d);
    if (!SetupDiCallClassInstaller(DIF_REMOVE, set, d)) {
        fail("DIF_REMOVE");
        return 0;
    }
    printf("sbdevnode: removed\n");
    return 1;
}

static int create(const wchar_t *inf, const wchar_t *hwid)
{
    GUID cls;
    wchar_t clsname[64];  /* MAX_CLASS_NAME_LEN is 32 */
    if (!SetupDiGetINFClassW(inf, &cls, clsname, _countof(clsname), NULL)) {
        fail("SetupDiGetINFClass");
        return 0;
    }
    HDEVINFO set = SetupDiCreateDeviceInfoList(&cls, NULL);
    if (set == INVALID_HANDLE_VALUE) {
        fail("SetupDiCreateDeviceInfoList");
        return 0;
    }
    /* Base name = the part after "Root\" -> instance ROOT\STARBITEDISPLAY\0000 */
    const wchar_t *base = wcschr(hwid, L'\\') ? wcschr(hwid, L'\\') + 1 : hwid;
    SP_DEVINFO_DATA d = {sizeof d};
    int ok = 0;
    wchar_t ids[256] = {0};
    size_t len = wcslen(hwid);
    if (len + 2 > _countof(ids)) {
        printf("sbdevnode: hardware id too long\n");
        goto out;
    }
    wcscpy_s(ids, _countof(ids), hwid);  /* REG_MULTI_SZ: hwid\0\0 */
    if (!SetupDiCreateDeviceInfoW(set, base, &cls, NULL, NULL, DICD_GENERATE_ID, &d)) {
        fail("SetupDiCreateDeviceInfo");
        goto out;
    }
    if (!SetupDiSetDeviceRegistryPropertyW(set, &d, SPDRP_HARDWAREID, (const BYTE *)ids,
                                           (DWORD)((len + 2) * sizeof(wchar_t)))) {
        fail("SPDRP_HARDWAREID");
        goto out;
    }
    if (!SetupDiCallClassInstaller(DIF_REGISTERDEVICE, set, &d)) {
        fail("DIF_REGISTERDEVICE");
        goto out;
    }
    print_dev(set, &d);
    printf("sbdevnode: device node created (class %ls)\n", clsname);
    ok = 1;
out:
    SetupDiDestroyDeviceInfoList(set);
    return ok;
}

int wmain(int argc, wchar_t **argv)
{
    if (argc == 4 && !_wcsicmp(argv[1], L"install")) {
        const wchar_t *hwid = argv[3];
        wchar_t inf[MAX_PATH];
        if (!GetFullPathNameW(argv[2], MAX_PATH, inf, NULL)) {
            fail("GetFullPathName");
            return 1;
        }
        int n = for_each(hwid, print_dev);
        if (n < 0)
            return 1;
        if (n == 0 && !create(inf, hwid))
            return 1;
        BOOL reboot = FALSE;
        if (!UpdateDriverForPlugAndPlayDevicesW(NULL, hwid, inf, INSTALLFLAG_FORCE, &reboot)) {
            fail("UpdateDriverForPlugAndPlayDevices");
            return 1;
        }
        printf("sbdevnode: driver installed from %ls%s\n", inf, reboot ? " (reboot needed)" : "");
        return 0;
    }
    if (argc == 3 && !_wcsicmp(argv[1], L"remove")) {
        int n = for_each(argv[2], remove_dev);
        if (n == 0)
            printf("sbdevnode: no device with hardware id %ls\n", argv[2]);
        return n < 0 ? 1 : 0;
    }
    if (argc == 3 && !_wcsicmp(argv[1], L"list")) {
        int n = for_each(argv[2], print_dev);
        printf("sbdevnode: %d device(s)\n", n < 0 ? 0 : n);
        return n < 0 ? 1 : 0;
    }
    printf("usage: sbdevnode install INF HWID | remove HWID | list HWID\n");
    return 2;
}
