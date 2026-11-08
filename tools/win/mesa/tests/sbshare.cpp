// SPDX-License-Identifier: GPL-2.0-only
//
// sbshare: do shared surfaces work through our D3D10 UMD (sbumd) on the Mali (docs/WIN11-GPU.md 6.18)?
// T13n: plain textures render and sample correctly on the Mali (sbtex 9/9). DWM's frames are still black, and
// everything DWM shows lives in surfaces of a special kind: shared window surfaces it opens from other processes,
// and its presented primaries. sbumd backs those with runtime allocations (sb-patches/0103), unlike plain
// textures. These tests isolate that path, the way DWM uses it.
//
//   sbshare.exe hw [LOG]           producer: the local tests, then starts the consumer and waits for it
//   sbshare.exe consume H [LOG]    consumer (started by the producer): opens shared handle H (hex)
//
// Producer (one process):
//   local-clear   a SHARED 256x256 B8G8R8A8 texture (bind SRV|RT), cleared green, copied to staging, read
//   local-draw    the same texture, a solid-blue full-screen draw into it, read
//   plain-draw    the same draw into a plain (not shared) texture, as the control
// Consumer (second process, as DWM): OpenSharedResource on the producer's texture (now blue), then
//   open-copy     CopyResource to staging, read                (the UMD's OpenResource import, read by the GPU)
//   open-sample   sample it in a shader into its own target    (what DWM's composition does)
// Then the producer clears its texture red, flushes, and the consumer (waiting on an event) reads again:
//   open-update   the producer's later GPU write seen by the consumer (CopyResource)
//   open-samp-red the same, sampled by the consumer's GPU
// Lines "TEST name: PASS|FAIL got ... want ..."; exit code = failures (100 + n: setup failed).
//
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxgi.lib")

#define W 256
#define H 256

static const char kHlsl[] =
    "Texture2D t : register(t0);\n"
    "SamplerState s : register(s0);\n"
    "struct V { float4 p : SV_Position; float2 uv : TEXCOORD0; };\n"
    "V vs(float2 p : POSITION) {\n"
    "  V o; o.uv = float2(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5); o.p = float4(p, 0, 1); return o; }\n"
    "float4 ps_blue(V i) : SV_Target { return float4(0.0, 0.0, 1.0, 1.0); }\n"
    "float4 ps_sample(V i) : SV_Target { return t.Sample(s, i.uv); }\n";

static ID3D11Device *dev;
static ID3D11DeviceContext *ctx;
static ID3D11VertexShader *vs;
static ID3D11InputLayout *il;
static ID3D11Buffer *vb;
static ID3D11SamplerState *smp;
static FILE *logf;
static int failures;
static const char *role = "producer";

static void out(const char *fmt, ...)
{
    va_list a;
    va_start(a, fmt);
    printf("[%s] ", role);
    vprintf(fmt, a);
    va_end(a);
    fflush(stdout);
    if (logf) {
        va_start(a, fmt);
        fprintf(logf, "[%s] ", role);
        vfprintf(logf, fmt, a);
        va_end(a);
        fflush(logf);
    }
}

static int make_device(void)
{
    IDXGIFactory1 *fac = nullptr;
    IDXGIAdapter1 *pick = nullptr, *a = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&fac)))
        return 100;
    for (UINT i = 0; fac->EnumAdapters1(i, &a) == S_OK; i++) {
        DXGI_ADAPTER_DESC1 d;
        a->GetDesc1(&d);
        if (!pick && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
            (d.VendorId == 0x42525453 || wcsstr(d.Description, L"Mali") || wcsstr(d.Description, L"Starbite")))
            pick = a;
        else
            a->Release();
    }
    if (!pick) {
        out("FAIL no Starbite/Mali adapter\n");
        return 101;
    }
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_10_0, got;
    HRESULT hr = D3D11CreateDevice(pick, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev, &got, &ctx);
    if (FAILED(hr)) {
        out("FAIL D3D11CreateDevice: 0x%08lx\n", (unsigned long)hr);
        return 102;
    }
    ID3DBlob *b = nullptr, *err = nullptr;
    if (FAILED(D3DCompile(kHlsl, sizeof(kHlsl) - 1, "sbshare", nullptr, nullptr, "vs", "vs_4_0", 0, 0, &b, &err)) ||
        FAILED(dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &vs)))
        return 103;
    const float verts[] = { -1.0f, 1.0f, 3.0f, 1.0f, -1.0f, -3.0f };
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = sizeof(verts);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA init = { verts, 0, 0 };
    D3D11_INPUT_ELEMENT_DESC ied = { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 };
    if (FAILED(dev->CreateBuffer(&bd, &init, &vb)) ||
        FAILED(dev->CreateInputLayout(&ied, 1, b->GetBufferPointer(), b->GetBufferSize(), &il)))
        return 104;
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    ID3D11RasterizerState *rs = nullptr;
    dev->CreateRasterizerState(&rd, &rs);
    ctx->RSSetState(rs);
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    dev->CreateSamplerState(&sd, &smp);
    out("device created (hardware)\n");
    return 0;
}

static ID3D11Texture2D *texture(UINT misc)
{
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = W;
    td.Height = H;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    td.MiscFlags = misc;
    ID3D11Texture2D *t = nullptr;
    HRESULT hr = dev->CreateTexture2D(&td, nullptr, &t);
    if (FAILED(hr))
        out("CreateTexture2D(misc 0x%x) failed: 0x%08lx\n", misc, (unsigned long)hr);
    return t;
}

/* Read the texture back through a staging copy; check the centre and a corner. BGRA in memory: blue = 0xff0000ff
 * (B=ff), green = 0xff00ff00, red = 0xffff0000. */
static void check(const char *name, ID3D11Texture2D *t, UINT want)
{
    D3D11_TEXTURE2D_DESC td;
    t->GetDesc(&td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.MiscFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D *st = nullptr;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &st))) {
        out("TEST %-11s: FAIL (no staging)\n", name);
        failures++;
        return;
    }
    ctx->CopyResource(st, t);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
        out("TEST %-11s: FAIL (Map)\n", name);
        failures++;
        st->Release();
        return;
    }
    UINT c = *(const UINT *)((const BYTE *)m.pData + (H / 2) * m.RowPitch + (W / 2) * 4);
    UINT k = *(const UINT *)((const BYTE *)m.pData + 3 * m.RowPitch + 3 * 4);
    ctx->Unmap(st, 0);
    st->Release();
    bool ok = c == want && k == want;
    out("TEST %-11s: %s  got centre 0x%08x corner 0x%08x  want 0x%08x\n", name, ok ? "PASS" : "FAIL", c, k, want);
    if (!ok)
        failures++;
}

static void draw(ID3D11Texture2D *target, const char *ps_entry, ID3D11ShaderResourceView *srv)
{
    ID3DBlob *b = nullptr, *err = nullptr;
    ID3D11PixelShader *ps = nullptr;
    ID3D11RenderTargetView *rtv = nullptr;
    if (FAILED(D3DCompile(kHlsl, sizeof(kHlsl) - 1, "sbshare", nullptr, nullptr, ps_entry, "ps_4_0", 0, 0, &b, &err)) ||
        FAILED(dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &ps)) ||
        FAILED(dev->CreateRenderTargetView(target, nullptr, &rtv))) {
        out("draw setup failed\n");
        return;
    }
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    D3D11_VIEWPORT vp = { 0.0f, 0.0f, (float)W, (float)H, 0.0f, 1.0f };
    ctx->RSSetViewports(1, &vp);
    UINT stride = 8, offset = 0;
    ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
    ctx->IASetInputLayout(il);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vs, nullptr, 0);
    ctx->PSSetShader(ps, nullptr, 0);
    ctx->PSSetShaderResources(0, 1, &srv);
    ctx->PSSetSamplers(0, 1, &smp);
    ctx->Draw(3, 0);
    ID3D11ShaderResourceView *none = nullptr;
    ctx->PSSetShaderResources(0, 1, &none);
    ID3D11RenderTargetView *nrt = nullptr;
    ctx->OMSetRenderTargets(1, &nrt, nullptr);
    rtv->Release();
    ps->Release();
}

static void clear(ID3D11Texture2D *t, float r, float g, float b)
{
    ID3D11RenderTargetView *rtv = nullptr;
    if (FAILED(dev->CreateRenderTargetView(t, nullptr, &rtv)))
        return;
    const float c[4] = { r, g, b, 1.0f };
    ctx->ClearRenderTargetView(rtv, c);
    rtv->Release();
}

static const UINT BLUE = 0xff0000ffu, GREEN = 0xff00ff00u, RED = 0xffff0000u;

static int consumer(HANDLE h)
{
    ID3D11Texture2D *t = nullptr;
    HRESULT hr = dev->OpenSharedResource(h, __uuidof(ID3D11Texture2D), (void **)&t);
    if (FAILED(hr)) {
        out("TEST open       : FAIL OpenSharedResource(0x%p): 0x%08lx\n", h, (unsigned long)hr);
        return 1;
    }
    out("opened the shared texture\n");
    check("open-copy", t, BLUE);
    ID3D11ShaderResourceView *srv = nullptr;
    ID3D11Texture2D *mine = texture(0);
    if (mine && SUCCEEDED(dev->CreateShaderResourceView(t, nullptr, &srv))) {
        clear(mine, 0.5f, 0.5f, 0.5f);
        draw(mine, "ps_sample", srv);
        check("open-sample", mine, BLUE);
    } else {
        out("TEST open-sample: FAIL (no SRV)\n");
        failures++;
    }
    // the producer's later write: it clears red, flushes and signals
    HANDLE go = OpenEventA(SYNCHRONIZE, FALSE, "Local\\sbshare-red"), done = OpenEventA(EVENT_MODIFY_STATE, FALSE, "Local\\sbshare-next");
    if (done)
        SetEvent(done);
    if (go && WaitForSingleObject(go, 20000) == WAIT_OBJECT_0) {
        check("open-update", t, RED);
        /* T13p: the same update seen by the GPU (DWM samples window surfaces apps keep redrawing) */
        if (mine && srv) {
            clear(mine, 0.5f, 0.5f, 0.5f);
            draw(mine, "ps_sample", srv);
            check("open-samp-red", mine, RED);
        }
    } else {
        out("TEST open-update: FAIL (no signal from the producer)\n");
        failures++;
    }
    return failures;
}

int main(int argc, char **argv)
{
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    bool cons = argc > 2 && strcmp(argv[1], "consume") == 0;
    const char *logname = cons ? (argc > 3 ? argv[3] : "sbshare.log") : (argc > 2 ? argv[2] : "sbshare.log");
    logf = fopen(logname, "a");
    if (cons)
        role = "consumer";
    int r = make_device();
    if (r)
        return r;
    if (cons)
        return consumer((HANDLE)(ULONG_PTR)_strtoui64(argv[2], nullptr, 16));

    // ---- producer: local tests ----
    ID3D11Texture2D *plain = texture(0), *shared = texture(D3D11_RESOURCE_MISC_SHARED);
    if (!plain || !shared)
        return 105;
    clear(plain, 0.5f, 0.5f, 0.5f);
    draw(plain, "ps_blue", nullptr);
    check("plain-draw", plain, BLUE);
    clear(shared, 0.0f, 1.0f, 0.0f);
    check("local-clear", shared, GREEN);
    draw(shared, "ps_blue", nullptr);
    check("local-draw", shared, BLUE);
    ctx->Flush();

    IDXGIResource *xr = nullptr;
    HANDLE h = nullptr;
    if (FAILED(shared->QueryInterface(__uuidof(IDXGIResource), (void **)&xr)) || FAILED(xr->GetSharedHandle(&h)) || !h) {
        out("TEST share      : FAIL GetSharedHandle\n");
        return 106;
    }
    out("shared handle 0x%p; starting the consumer\n", h);
    HANDLE red = CreateEventA(nullptr, TRUE, FALSE, "Local\\sbshare-red"), next = CreateEventA(nullptr, TRUE, FALSE, "Local\\sbshare-next");
    char self[MAX_PATH], cmd[2 * MAX_PATH];
    GetModuleFileNameA(nullptr, self, sizeof(self));
    snprintf(cmd, sizeof(cmd), "\"%s\" consume %llx %s", self, (unsigned long long)(ULONG_PTR)h, logname);
    if (logf) {
        fclose(logf);                     // the consumer appends to the same file
        logf = nullptr;
    }
    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessA(nullptr, cmd, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        logf = fopen(logname, "a");
        out("TEST consumer   : FAIL CreateProcess %lu\n", GetLastError());
        return 107;
    }
    // after its first two tests: clear red, flush, signal
    if (WaitForSingleObject(next, 30000) == WAIT_OBJECT_0) {
        clear(shared, 1.0f, 0.0f, 0.0f);
        ctx->Flush();
        Sleep(500);
        SetEvent(red);
    }
    DWORD code = 0;
    if (WaitForSingleObject(pi.hProcess, 60000) != WAIT_OBJECT_0) {
        TerminateProcess(pi.hProcess, 99);
        code = 99;
    } else {
        GetExitCodeProcess(pi.hProcess, &code);
    }
    logf = fopen(logname, "a");
    out("consumer exit %ld (0x%lx)\n", (long)code, code);
    check("local-red", shared, RED);
    out("sbshare: producer %d failure(s), consumer exit %ld\n", failures, (long)code);
    return failures + (code ? 1 : 0);
}
