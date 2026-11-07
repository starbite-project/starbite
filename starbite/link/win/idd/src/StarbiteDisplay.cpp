// SPDX-License-Identifier: GPL-2.0-only
//
// Starbite Display: a UMDF 2 IddCx indirect display driver for Windows 11 ARM64 on the Galaxy S9 (Starbite Link,
// starbite/link/PROTOCOL.md). It gives Windows one extra monitor, "Starbite Display", that the desktop extends onto.
// The Starbite Link sender captures that output with Desktop Duplication and ships it to the Mac, so this driver
// does not move pixels itself: its swap-chain thread acquires each frame DWM presents and hands it straight back.
//
// Written for Starbite from the IddCx documentation and headers (the design follows the shape of Microsoft's
// IddSampleDriver: adapter in D0Entry, monitor after AdapterInitFinished, one thread per swap chain; no code from it).
//
// CPU budget (no GPU driver, 4 A55 cores): nothing is polled. The thread sleeps on hNextSurfaceAvailable; DWM only
// presents when something on this output changed (StaticDesktopReencodeFrameCount = 0), and a frame costs one
// acquire + one release, no copy, no map. Status goes to the registry only on state changes and every 1024 frames.
//
// Status for offline diagnosis (read from the pulled SYSTEM hive; INSTALL.md): values Sb* in the device's writable
// hardware subkey (Enum\Root\StarbiteDisplay\0000\Device Parameters\...).
#include <windows.h>
#include <wdf.h>
#include <iddcx.h>
#include <d3d11.h>
#include <dxgi1_5.h>
#include <avrt.h>
#include <new>
#include <stdio.h>
#include <stdarg.h>
#include <wchar.h>

#include "sbmodes.h"

#ifndef SB_BUILD_ID
#define SB_BUILD_ID "dev"
#endif
#define SB_WIDEN2(s) L##s
#define SB_WIDEN(s) SB_WIDEN2(s)

using namespace sbidd;

// WDF context types (at global scope: the WDF macros declare extern type-info objects).
class SwapChainProc;

struct DeviceCtx {
    WDFDEVICE device;
    WDFKEY status;            // may be NULL (status is best effort)
    IDDCX_ADAPTER adapter;
    IDDCX_MONITOR monitor;
    bool adapterStarted;
    volatile LONG swapchains; // assigns since load
};
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DeviceCtx, GetDeviceCtx);


struct MonitorCtx {
    DeviceCtx *dev;
    SwapChainProc *proc;
};
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(MonitorCtx, GetMonitorCtx);

// {8E4F2B6A-5D0C-4C1B-9B6E-53424954ED15}: fixed container id, so Windows groups the monitor with the adapter.
const GUID kContainerId = {0x8e4f2b6a, 0x5d0c, 0x4c1b, {0x9b, 0x6e, 0x53, 0x42, 0x49, 0x54, 0xed, 0x15}};

// IddCx keeps a pointer to the description; it must be writable memory (PVOID) and live forever.
uint8_t g_edid[128];

// The one Starbite Display device (root-enumerated, single instance). Set before IddCxAdapterInitAsync, because
// EvtAdapterInitFinished may run on another thread before that call returns.
DeviceCtx *g_dev;

void Log(const wchar_t *fmt, ...)
{
    wchar_t buf[256];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    OutputDebugStringW(L"StarbiteDisplay: ");
    OutputDebugStringW(buf);
    OutputDebugStringW(L"\n");
}

// ---- status values ---------------------------------------------------------------------------------------------

void PutValue(DeviceCtx *d, const wchar_t *name, ULONG type, const void *data, ULONG len)
{
    if (!d || !d->status)
        return;
    UNICODE_STRING n;
    n.Buffer = const_cast<PWSTR>(name);
    n.Length = (USHORT)(wcslen(name) * sizeof(wchar_t));
    n.MaximumLength = n.Length + sizeof(wchar_t);
    WdfRegistryAssignValue(d->status, &n, type, len, const_cast<void *>(data));
}

void PutDword(DeviceCtx *d, const wchar_t *name, ULONG v) { PutValue(d, name, REG_DWORD, &v, sizeof v); }
void PutQword(DeviceCtx *d, const wchar_t *name, ULONG64 v) { PutValue(d, name, REG_QWORD, &v, sizeof v); }
void PutString(DeviceCtx *d, const wchar_t *name, const wchar_t *s)
{
    PutValue(d, name, REG_SZ, s, (ULONG)((wcslen(s) + 1) * sizeof(wchar_t)));
}
void PutState(DeviceCtx *d, const wchar_t *state)
{
    Log(L"%s", state);
    PutString(d, L"SbState", state);
}

// ---- modes -----------------------------------------------------------------------------------------------------

DISPLAYCONFIG_VIDEO_SIGNAL_INFO SignalInfo(const Mode &m)
{
    DISPLAYCONFIG_VIDEO_SIGNAL_INFO s = {};
    s.pixelRate = PixelRate(m);
    s.hSyncFreq.Numerator = HSyncHz(m);
    s.hSyncFreq.Denominator = 1;
    s.vSyncFreq.Numerator = m.hz;
    s.vSyncFreq.Denominator = 1;
    s.activeSize.cx = m.w;
    s.activeSize.cy = m.h;
    s.totalSize.cx = m.tw;
    s.totalSize.cy = m.th;
    s.AdditionalSignalInfo.videoStandard = 255;   // D3DKMDT_VSS_OTHER
    s.AdditionalSignalInfo.vSyncFreqDivider = 1;  // progressive: one vsync per frame
    s.scanLineOrdering = DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;
    return s;
}

void FillMonitorModes(IDDCX_MONITOR_MODE *out, IDDCX_MONITOR_MODE_ORIGIN origin)
{
    for (unsigned i = 0; i < kModeCount; i++) {
        out[i] = {};
        out[i].Size = sizeof(IDDCX_MONITOR_MODE);
        out[i].Origin = origin;
        out[i].MonitorVideoSignalInfo = SignalInfo(kModes[i]);
        // IddCx.h: a monitor mode's vSyncFreqDivider "has to have a zero value" (only target modes carry 1);
        // a nonzero one makes the OS reject the mode list from EvtParseMonitorDescription.
        out[i].MonitorVideoSignalInfo.AdditionalSignalInfo.vSyncFreqDivider = 0;
    }
}

// ---- swap chain: acquire, release, repeat ----------------------------------------------------------------------

class SwapChainProc {
public:
    static SwapChainProc *Start(DeviceCtx *dev, const IDARG_IN_SETSWAPCHAIN *in)
    {
        SwapChainProc *p = new (std::nothrow) SwapChainProc();
        if (!p)
            return nullptr;
        p->m_dev = dev;
        p->m_swap = in->hSwapChain;
        p->m_luid = in->RenderAdapterLuid;
        p->m_newFrame = in->hNextSurfaceAvailable;
        p->m_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (p->m_stop)
            p->m_thread = CreateThread(nullptr, 0, ThreadMain, p, 0, nullptr);
        if (!p->m_thread) {
            if (p->m_stop)
                CloseHandle(p->m_stop);
            delete p;
            return nullptr;
        }
        return p;
    }

    ~SwapChainProc()
    {
        SetEvent(m_stop);
        WaitForSingleObject(m_thread, INFINITE);
        CloseHandle(m_thread);
        CloseHandle(m_stop);
    }

private:
    SwapChainProc() = default;

    static DWORD WINAPI ThreadMain(void *arg)
    {
        static_cast<SwapChainProc *>(arg)->Run();
        return 0;
    }

    // A D3D11 device on the render adapter the OS chose (on the phone: Microsoft Basic Render Driver, WARP).
    HRESULT MakeDevice(ID3D11Device **dev)
    {
        IDXGIFactory4 *f = nullptr;
        IDXGIAdapter1 *a = nullptr;
        HRESULT hr = CreateDXGIFactory2(0, __uuidof(IDXGIFactory4), reinterpret_cast<void **>(&f));
        if (SUCCEEDED(hr))
            hr = f->EnumAdapterByLuid(m_luid, __uuidof(IDXGIAdapter1), reinterpret_cast<void **>(&a));
        if (SUCCEEDED(hr))
            hr = D3D11CreateDevice(a, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                   D3D11_SDK_VERSION, dev, nullptr, nullptr);
        if (a)
            a->Release();
        if (f)
            f->Release();
        return hr;
    }

    void Run()
    {
        DWORD task = 0;
        // MMCSS: DWM waits for our release, so a busy phone must not delay this tiny loop.
        HANDLE av = AvSetMmThreadCharacteristicsW(L"Distribution", &task);

        ID3D11Device *d3d = nullptr;
        IDXGIDevice *dxgi = nullptr;
        HRESULT hr = MakeDevice(&d3d);
        if (SUCCEEDED(hr))
            hr = d3d->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void **>(&dxgi));
        if (SUCCEEDED(hr)) {
            IDARG_IN_SWAPCHAINSETDEVICE sd = {};
            sd.pDevice = dxgi;
            hr = IddCxSwapChainSetDevice(m_swap, &sd);
        }
        PutDword(m_dev, L"SbSetDeviceHr", (ULONG)hr);
        PutQword(m_dev, L"SbRenderLuid", ((ULONG64)(ULONG)m_luid.HighPart << 32) | m_luid.LowPart);
        if (FAILED(hr)) {
            PutState(m_dev, L"swapchain: SetDevice failed");
        } else {
            PutState(m_dev, L"swapchain: running");
            Loop();
        }
        // The driver owns the swap chain it was assigned and deletes it whenever processing ends, also after an
        // Unassign (as IddSampleDriver does): an undeleted one keeps its surfaces (three desktop-sized buffers)
        // alive until the monitor goes, on every mode change; after a failure, deleting it makes the OS assign
        // a new one.
        WdfObjectDelete(m_swap);
        m_swap = nullptr;
        if (dxgi)
            dxgi->Release();
        if (d3d)
            d3d->Release();
        if (av)
            AvRevertMmThreadCharacteristics(av);
    }

    // Returns true when stopped by Unassign, false when the swap chain failed.
    bool Loop()
    {
        HANDLE waits[2] = {m_newFrame, m_stop};
        ULONG frames = 0;
        HRESULT hr;
        bool stopped = false;
        for (;;) {
            IDARG_OUT_RELEASEANDACQUIREBUFFER buf = {};
            hr = IddCxSwapChainReleaseAndAcquireBuffer(m_swap, &buf);
            if (hr == E_PENDING) {
                // Nothing new: sleep until DWM presents (or we are told to stop). The timeout only bounds a missed
                // signal; it is not a poll.
                DWORD w = WaitForMultipleObjects(2, waits, FALSE, 1000);
                if (w == WAIT_OBJECT_0 + 1 || w == WAIT_FAILED) {
                    stopped = w != WAIT_FAILED;
                    break;
                }
                continue;
            }
            if (FAILED(hr))
                break;  // swap chain abandoned (mode change, monitor gone): the OS unassigns + reassigns
            // The acquired surface carries a reference that belongs to us. The sender reads this output through
            // Desktop Duplication, so the frame is handed back untouched.
            if (buf.MetaData.pSurface)
                buf.MetaData.pSurface->Release();
            hr = IddCxSwapChainFinishedProcessingFrame(m_swap);
            if (FAILED(hr))
                break;
            if ((++frames & 1023) == 0)
                PutDword(m_dev, L"SbFrames", frames);
            if (WaitForSingleObject(m_stop, 0) == WAIT_OBJECT_0) {
                stopped = true;
                break;
            }
        }
        PutDword(m_dev, L"SbFrames", frames);
        PutDword(m_dev, L"SbLoopExitHr", (ULONG)hr);
        PutState(m_dev, stopped ? L"swapchain: stopped" : L"swapchain: lost");
        return stopped;
    }

    DeviceCtx *m_dev = nullptr;
    IDDCX_SWAPCHAIN m_swap = nullptr;
    LUID m_luid = {};
    HANDLE m_newFrame = nullptr;
    HANDLE m_stop = nullptr;
    HANDLE m_thread = nullptr;
};

// ---- IddCx callbacks -------------------------------------------------------------------------------------------

NTSTATUS CreateMonitor(DeviceCtx *d, IDDCX_ADAPTER adapter)
{
    WDF_OBJECT_ATTRIBUTES attr;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, MonitorCtx);
    attr.EvtCleanupCallback = [](WDFOBJECT obj) {
        MonitorCtx *m = GetMonitorCtx(obj);
        delete m->proc;
        m->proc = nullptr;
    };

    IDDCX_MONITOR_INFO info = {};
    info.Size = sizeof info;
    info.MonitorType = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INDIRECT_WIRED;
    info.ConnectorIndex = 0;
    info.MonitorDescription.Size = sizeof info.MonitorDescription;
    info.MonitorDescription.Type = IDDCX_MONITOR_DESCRIPTION_TYPE_EDID;
    info.MonitorDescription.DataSize = sizeof g_edid;
    info.MonitorDescription.pData = g_edid;
    info.MonitorContainerId = kContainerId;

    IDARG_IN_MONITORCREATE in = {};
    in.ObjectAttributes = &attr;
    in.pMonitorInfo = &info;
    IDARG_OUT_MONITORCREATE out = {};
    NTSTATUS st = IddCxMonitorCreate(adapter, &in, &out);
    PutDword(d, L"SbMonitorCreateStatus", (ULONG)st);
    if (!NT_SUCCESS(st))
        return st;
    MonitorCtx *m = GetMonitorCtx(out.MonitorObject);
    m->dev = d;
    m->proc = nullptr;
    d->monitor = out.MonitorObject;

    IDARG_OUT_MONITORARRIVAL arr = {};
    st = IddCxMonitorArrival(out.MonitorObject, &arr);
    PutDword(d, L"SbMonitorArrivalStatus", (ULONG)st);
    if (NT_SUCCESS(st)) {
        PutDword(d, L"SbOsTargetId", arr.OsTargetId);
        PutQword(d, L"SbOsAdapterLuid", ((ULONG64)(ULONG)arr.OsAdapterLuid.HighPart << 32) | arr.OsAdapterLuid.LowPart);
        PutState(d, L"monitor arrived");
    }
    return st;
}

NTSTATUS EvtAdapterInitFinished(IDDCX_ADAPTER adapter, const IDARG_IN_ADAPTER_INIT_FINISHED *in)
{
    DeviceCtx *d = g_dev;
    d->adapter = adapter;
    PutDword(d, L"SbAdapterInitStatus", (ULONG)in->AdapterInitStatus);
    if (!NT_SUCCESS(in->AdapterInitStatus)) {
        PutState(d, L"adapter init failed");
        return STATUS_SUCCESS;
    }
    return CreateMonitor(d, adapter);
}

NTSTATUS EvtAdapterCommitModes(IDDCX_ADAPTER, const IDARG_IN_COMMITMODES *in)
{
    DeviceCtx *d = g_dev;
    wchar_t mode[64] = L"inactive";
    for (UINT i = 0; i < in->PathCount; i++) {
        const IDDCX_PATH &p = in->pPaths[i];
        if (p.Flags & IDDCX_PATH_FLAGS_ACTIVE) {
            const DISPLAYCONFIG_VIDEO_SIGNAL_INFO &s = p.TargetVideoSignalInfo;
            UINT hz = s.vSyncFreq.Denominator ? s.vSyncFreq.Numerator / s.vSyncFreq.Denominator : 0;
            _snwprintf_s(mode, _countof(mode), _TRUNCATE, L"%ux%u@%u", s.activeSize.cx, s.activeSize.cy, hz);
        }
    }
    PutString(d, L"SbMode", mode);
    return STATUS_SUCCESS;
}

NTSTATUS EvtParseMonitorDescription(const IDARG_IN_PARSEMONITORDESCRIPTION *in, IDARG_OUT_PARSEMONITORDESCRIPTION *out)
{
    // Only our own EDID is ever handed to IddCx, so the modes come from the table, not from parsing.
    out->MonitorModeBufferOutputCount = kModeCount;
    if (in->MonitorModeBufferInputCount == 0)
        return STATUS_SUCCESS;  // the OS asks for the count first
    if (in->MonitorModeBufferInputCount < kModeCount)
        return STATUS_BUFFER_TOO_SMALL;
    FillMonitorModes(in->pMonitorModes, IDDCX_MONITOR_MODE_ORIGIN_MONITORDESCRIPTOR);
    out->PreferredMonitorModeIdx = kPreferredMode;
    return STATUS_SUCCESS;
}

NTSTATUS EvtMonitorGetDefaultModes(IDDCX_MONITOR, const IDARG_IN_GETDEFAULTDESCRIPTIONMODES *in,
                                   IDARG_OUT_GETDEFAULTDESCRIPTIONMODES *out)
{
    // Only called for a monitor without a description; ours has an EDID. Same list, for completeness.
    out->DefaultMonitorModeBufferOutputCount = kModeCount;
    if (in->DefaultMonitorModeBufferInputCount == 0)
        return STATUS_SUCCESS;
    if (in->DefaultMonitorModeBufferInputCount < kModeCount)
        return STATUS_BUFFER_TOO_SMALL;
    FillMonitorModes(in->pDefaultMonitorModes, IDDCX_MONITOR_MODE_ORIGIN_DRIVER);
    out->PreferredMonitorModeIdx = kPreferredMode;
    return STATUS_SUCCESS;
}

NTSTATUS EvtMonitorQueryTargetModes(IDDCX_MONITOR, const IDARG_IN_QUERYTARGETMODES *in, IDARG_OUT_QUERYTARGETMODES *out)
{
    out->TargetModeBufferOutputCount = kModeCount;
    if (in->TargetModeBufferInputCount == 0)
        return STATUS_SUCCESS;
    if (in->TargetModeBufferInputCount < kModeCount)
        return STATUS_BUFFER_TOO_SMALL;
    for (unsigned i = 0; i < kModeCount; i++) {
        IDDCX_TARGET_MODE &t = in->pTargetModes[i];
        t = {};
        t.Size = sizeof t;
        t.TargetVideoSignalInfo.targetVideoSignalInfo = SignalInfo(kModes[i]);
        // RequiredBandwidth stays 0: the adapter reports no MaxDisplayPipelineRate either.
    }
    return STATUS_SUCCESS;
}

NTSTATUS EvtMonitorAssignSwapChain(IDDCX_MONITOR monitor, const IDARG_IN_SETSWAPCHAIN *in)
{
    MonitorCtx *m = GetMonitorCtx(monitor);
    delete m->proc;
    m->proc = SwapChainProc::Start(m->dev, in);
    PutDword(m->dev, L"SbSwapChains", (ULONG)InterlockedIncrement(&m->dev->swapchains));
    return m->proc ? STATUS_SUCCESS : STATUS_INSUFFICIENT_RESOURCES;
}

NTSTATUS EvtMonitorUnassignSwapChain(IDDCX_MONITOR monitor)
{
    MonitorCtx *m = GetMonitorCtx(monitor);
    delete m->proc;
    m->proc = nullptr;
    return STATUS_SUCCESS;
}

VOID EvtDeviceIoControl(WDFDEVICE, WDFREQUEST request, size_t, size_t, ULONG)
{
    WdfRequestComplete(request, STATUS_NOT_SUPPORTED);
}

// ---- WDF device ------------------------------------------------------------------------------------------------

NTSTATUS EvtD0Entry(WDFDEVICE device, WDF_POWER_DEVICE_STATE)
{
    DeviceCtx *d = GetDeviceCtx(device);
    if (d->adapterStarted)
        return STATUS_SUCCESS;  // the adapter (and its monitor) outlive D0 transitions

    static IDDCX_ENDPOINT_VERSION ver = {sizeof(IDDCX_ENDPOINT_VERSION), 0, 1, 0, 0};
    IDDCX_ADAPTER_CAPS caps = {};
    caps.Size = sizeof caps;
    caps.Flags = IDDCX_ADAPTER_FLAGS_NONE;
    caps.MaxMonitorsSupported = 1;
    caps.EndPointDiagnostics.Size = sizeof caps.EndPointDiagnostics;
    caps.EndPointDiagnostics.GammaSupport = IDDCX_FEATURE_IMPLEMENTATION_NONE;
    caps.EndPointDiagnostics.TransmissionType = IDDCX_TRANSMISSION_TYPE_WIRED_USB;  // Starbite Link (USB, or TCP)
    caps.EndPointDiagnostics.pEndPointFriendlyName = L"Starbite Display";
    caps.EndPointDiagnostics.pEndPointManufacturerName = L"Starbite";
    caps.EndPointDiagnostics.pEndPointModelName = L"Starbite Link";
    caps.EndPointDiagnostics.pFirmwareVersion = &ver;
    caps.EndPointDiagnostics.pHardwareVersion = &ver;
    caps.StaticDesktopReencodeFrameCount = 0;  // no re-sends of a static desktop

    WDF_OBJECT_ATTRIBUTES attr;
    WDF_OBJECT_ATTRIBUTES_INIT(&attr);
    IDARG_IN_ADAPTER_INIT in = {};
    in.WdfDevice = device;
    in.pCaps = &caps;
    in.ObjectAttributes = &attr;
    d->adapterStarted = true;
    PutState(d, L"adapter init started");  // before the call: InitFinished may report first
    IDARG_OUT_ADAPTER_INIT out = {};
    NTSTATUS st = IddCxAdapterInitAsync(&in, &out);
    PutDword(d, L"SbAdapterInitAsyncStatus", (ULONG)st);
    if (!NT_SUCCESS(st)) {
        d->adapterStarted = false;
        PutState(d, L"adapter init refused");
    }
    // Returning the failure would stop the device; keep it up so the status values stay readable.
    return STATUS_SUCCESS;
}

NTSTATUS EvtDeviceAdd(WDFDRIVER, PWDFDEVICE_INIT init)
{
    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDeviceD0Entry = EvtD0Entry;
    WdfDeviceInitSetPnpPowerEventCallbacks(init, &pnp);

    IDD_CX_CLIENT_CONFIG cfg;
    IDD_CX_CLIENT_CONFIG_INIT(&cfg);
    cfg.EvtIddCxDeviceIoControl = EvtDeviceIoControl;
    cfg.EvtIddCxParseMonitorDescription = EvtParseMonitorDescription;
    cfg.EvtIddCxAdapterInitFinished = EvtAdapterInitFinished;
    cfg.EvtIddCxAdapterCommitModes = EvtAdapterCommitModes;
    cfg.EvtIddCxMonitorGetDefaultDescriptionModes = EvtMonitorGetDefaultModes;
    cfg.EvtIddCxMonitorQueryTargetModes = EvtMonitorQueryTargetModes;
    cfg.EvtIddCxMonitorAssignSwapChain = EvtMonitorAssignSwapChain;
    cfg.EvtIddCxMonitorUnassignSwapChain = EvtMonitorUnassignSwapChain;
    NTSTATUS st = IddCxDeviceInitConfig(init, &cfg);
    if (!NT_SUCCESS(st))
        return st;

    WDF_OBJECT_ATTRIBUTES attr;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, DeviceCtx);
    WDFDEVICE device;
    st = WdfDeviceCreate(&init, &attr, &device);
    if (!NT_SUCCESS(st))
        return st;
    DeviceCtx *d = GetDeviceCtx(device);
    d->device = device;
    g_dev = d;
    // Writable per-device status key (UMDF drivers may write only below the hardware key's driver subkey).
    if (!NT_SUCCESS(WdfDeviceOpenRegistryKey(device, PLUGPLAY_REGKEY_DEVICE | WDF_REGKEY_DEVICE_SUBKEY,
                                             KEY_READ | KEY_SET_VALUE, WDF_NO_OBJECT_ATTRIBUTES, &d->status)))
        d->status = nullptr;
    PutString(d, L"SbBuild", SB_WIDEN(SB_BUILD_ID));
    PutState(d, L"device added");
    st = IddCxDeviceInitialize(device);
    PutDword(d, L"SbDeviceInitStatus", (ULONG)st);
    return st;
}

extern "C" DRIVER_INITIALIZE DriverEntry;

extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT driverObject, PUNICODE_STRING registryPath)
{
    for (unsigned i = 0; i < 128; i++)
        g_edid[i] = kEdid.b[i];
    Log(L"build %s", SB_WIDEN(SB_BUILD_ID));
    WDF_DRIVER_CONFIG config;
    WDF_DRIVER_CONFIG_INIT(&config, EvtDeviceAdd);
    return WdfDriverCreate(driverObject, registryPath, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
}
