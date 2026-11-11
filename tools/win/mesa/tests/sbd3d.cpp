// sbd3d.cpp (docs/WIN11-GPU-CAPS.md 5c): which adapter does a D3D11 program of THIS architecture get, and does
// it draw? Built native ARM64 and x64 (Windows' emulator, like most Steam games).
//   sbd3d.exe [LOG]
// Prints the DXGI adapters, creates a D3D11 device on the default hardware adapter, and reports the adapter the
// device really uses, the feature level, and whether our UMD (sbumd.dll) is loaded in this process (the proof
// that an emulated x64 process reached the Mali rather than WARP). Then clears an offscreen texture and draws a
// triangle (shaders from d3dcompiler_47.dll), reading back two pixels.
// Exit code: 0 = Mali (sbumd loaded) and both pixels right; 1 = drew right but not on our UMD (WARP / Basic
// Render); 2 = wrong pixels; 3 = no device.
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

static FILE *logf;
static void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    if (logf) { va_start(ap, fmt); vfprintf(logf, fmt, ap); va_end(ap); fflush(logf); }
}

typedef HRESULT (WINAPI *PFN_COMPILE)(LPCVOID, SIZE_T, LPCSTR, const void *, void *, LPCSTR, LPCSTR, UINT, UINT,
                                      ID3DBlob **, ID3DBlob **);

static const char *kShader =
    "float4 vs(float2 p : POS) : SV_Position { return float4(p, 0, 1); }\n"
    "float4 ps() : SV_Target { return float4(0, 1, 0, 1); }\n";

int main(int argc, char **argv)
{
    logf = fopen(argc > 1 ? argv[1] : "sbd3d.log", "a");
#if defined(_M_ARM64EC)
    const char *built = "ARM64EC";
#elif defined(_M_ARM64)
    const char *built = "ARM64";
#elif defined(_M_X64)
    const char *built = "x64";
#else
    const char *built = "x86";
#endif
    say("sbd3d start (built %s)\n", built);

    IDXGIFactory1 *fac = NULL;
    if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&fac))) {
        IDXGIAdapter1 *a;
        for (UINT i = 0; fac->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; i++) {
            DXGI_ADAPTER_DESC1 d; a->GetDesc1(&d);
            say("adapter %u: %ls vendor 0x%04x device 0x%04x flags 0x%x\n", i, d.Description, d.VendorId, d.DeviceId, d.Flags);
            a->Release();
        }
        fac->Release();
    }

    ID3D11Device *dev = NULL; ID3D11DeviceContext *ctx = NULL; D3D_FEATURE_LEVEL fl = (D3D_FEATURE_LEVEL)0;
    D3D_FEATURE_LEVEL want[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, want, 3, D3D11_SDK_VERSION, &dev, &fl, &ctx);
    if (FAILED(hr)) { say("FAIL D3D11CreateDevice(HARDWARE) 0x%08lx\n", hr); return 3; }
    IDXGIDevice *xd = NULL; IDXGIAdapter *used = NULL;
    dev->QueryInterface(__uuidof(IDXGIDevice), (void **)&xd);
    if (xd) xd->GetAdapter(&used);
    DXGI_ADAPTER_DESC ud = {0};
    if (used) used->GetDesc(&ud);
    HMODULE umd = GetModuleHandleW(L"sbumd.dll");
    wchar_t umdpath[MAX_PATH] = L"-";
    if (umd) GetModuleFileNameW(umd, umdpath, MAX_PATH);
    say("device: feature level 0x%x on \"%ls\" (vendor 0x%04x); sbumd.dll %s %ls\n", fl, ud.Description, ud.VendorId,
        umd ? "LOADED" : "not loaded", umdpath);

    // offscreen 64x64 target
    D3D11_TEXTURE2D_DESC td = {64, 64, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, 0};
    ID3D11Texture2D *rt = NULL; ID3D11RenderTargetView *rtv = NULL;
    dev->CreateTexture2D(&td, NULL, &rt);
    dev->CreateRenderTargetView(rt, NULL, &rtv);
    float blue[4] = {0, 0, 1, 1};
    ctx->ClearRenderTargetView(rtv, blue);

    bool drew = false;
    HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
    PFN_COMPILE compile = dc ? (PFN_COMPILE)GetProcAddress(dc, "D3DCompile") : NULL;
    ID3DBlob *vsb = NULL, *psb = NULL, *err = NULL;
    if (compile && SUCCEEDED(compile(kShader, strlen(kShader), "s", NULL, NULL, "vs", "vs_4_0", 0, 0, &vsb, &err)) &&
        SUCCEEDED(compile(kShader, strlen(kShader), "s", NULL, NULL, "ps", "ps_4_0", 0, 0, &psb, &err))) {
        ID3D11VertexShader *vs; ID3D11PixelShader *ps; ID3D11InputLayout *il; ID3D11Buffer *vb;
        dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), NULL, &vs);
        dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), NULL, &ps);
        D3D11_INPUT_ELEMENT_DESC ie = {"POS", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0};
        dev->CreateInputLayout(&ie, 1, vsb->GetBufferPointer(), vsb->GetBufferSize(), &il);
        float v[] = {-0.9f, -0.9f, 0.0f, 0.9f, 0.9f, -0.9f};
        D3D11_BUFFER_DESC bd = {sizeof v, D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0};
        D3D11_SUBRESOURCE_DATA sd = {v, 0, 0};
        dev->CreateBuffer(&bd, &sd, &vb);
        UINT stride = 8, off = 0;
        ctx->IASetInputLayout(il); ctx->IASetVertexBuffers(0, 1, &vb, &stride, &off);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(vs, NULL, 0); ctx->PSSetShader(ps, NULL, 0);
        D3D11_VIEWPORT vp = {0, 0, 64, 64, 0, 1};
        ctx->RSSetViewports(1, &vp); ctx->OMSetRenderTargets(1, &rtv, NULL);
        ctx->Draw(3, 0);
        drew = true;
    } else {
        say("shader compile unavailable (%s): checking the clear only\n", dc ? "D3DCompile failed" : "no d3dcompiler_47.dll");
    }

    td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D *st = NULL;
    dev->CreateTexture2D(&td, NULL, &st);
    ctx->CopyResource(st, rt);
    D3D11_MAPPED_SUBRESOURCE m;
    int bad = 0;
    if (SUCCEEDED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
        const unsigned *row0 = (const unsigned *)m.pData;
        const unsigned *mid = (const unsigned *)((const char *)m.pData + 40 * m.RowPitch);
        unsigned corner = row0[1], centre = mid[32];
        bool okc = corner == 0xffff0000u, okm = !drew || centre == 0xff00ff00u;
        say("%s clear: corner 0x%08x (want 0xffff0000)\n", okc ? "PASS" : "FAIL", corner);
        if (drew) say("%s triangle: centre 0x%08x (want 0xff00ff00)\n", okm ? "PASS" : "FAIL", centre);
        bad = !okc || !okm;
        ctx->Unmap(st, 0);
    } else {
        say("FAIL Map\n"); bad = 1;
    }
    int rc = bad ? 2 : (umd ? 0 : 1);
    say("sbd3d end: exit %d (%s)\n", rc, rc == 0 ? "on the Mali" : rc == 1 ? "drew, but NOT on our UMD" : "wrong pixels");
    return rc;
}
