// hello-umdf (C++: the IddCx headers are C++ only): build check for the UMDF 2 (+ IddCx) path of tools/win/build.sh. A UMDF 2.33 driver that
// creates a device for whatever it is bound to and does nothing else. Built with IDDCX=1.9 it also
// pulls in IddCx.h and the IddCx stub, as the Starbite Display driver (step L4) will. Not installed anywhere.
#include <windows.h>
#include <wdf.h>
#ifdef IDDCX_VERSION_MAJOR
#include <iddcx.h>
#endif

extern "C" DRIVER_INITIALIZE DriverEntry;
static EVT_WDF_DRIVER_DEVICE_ADD HelloDeviceAdd;

static NTSTATUS HelloDeviceAdd(_In_ WDFDRIVER driver, _Inout_ PWDFDEVICE_INIT init)
{
    WDFDEVICE dev;
    UNREFERENCED_PARAMETER(driver);
#ifdef IDDCX_VERSION_MAJOR
    IDD_CX_CLIENT_CONFIG cfg;
    IDD_CX_CLIENT_CONFIG_INIT(&cfg);
    NTSTATUS st = IddCxDeviceInitConfig(init, &cfg);   // callbacks left empty: build check only
    if (!NT_SUCCESS(st))
        return st;
#endif
    return WdfDeviceCreate(&init, WDF_NO_OBJECT_ATTRIBUTES, &dev);
}

NTSTATUS DriverEntry(_In_ PDRIVER_OBJECT driverObject, _In_ PUNICODE_STRING registryPath)
{
    WDF_DRIVER_CONFIG config;
    WDF_DRIVER_CONFIG_INIT(&config, HelloDeviceAdd);
    return WdfDriverCreate(driverObject, registryPath, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
}
