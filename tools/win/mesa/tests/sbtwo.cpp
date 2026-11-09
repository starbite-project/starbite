// SPDX-License-Identifier: GPL-2.0-only
//
// sbtwo: several D3D devices in one process on the Mali, as Explorer and TextInputHost have (docs/WIN11-GPU.md 6.22).
// The user's session (2026-11-09): every second device's submissions were refused (Render 0xC0000001), because
// the UMD's kernel-driver shim had one device per process. sb-patches/0113 keeps one per device.
//
//   sbtwo.exe hw [LOG]
//
//   two-a, two-b      device A, then device B (A alive): each draws its colour into its own texture, read back
//   interleave        200 rounds: A draws red, B draws green, each with a new texture every 10 rounds; checked
//                     every 25 rounds (a refused submission shows as the wrong colour or a removed device)
//   after-a-gone      A released, then device C made: B and C still draw
//   six               six devices alive at once (each has a tiler heap), each draws and is checked
//   pace              device B: 300 frames of a 1440x2960 clear + draw, flushed; one readback at the end;
//                     ms per frame (no CPU wait per frame: the UMD's waits are per buffer now)
// Lines "TEST name: PASS|FAIL ..."; exit code = failures (100 + n: setup failed).
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

static const char kHlsl[] =
    "float4 vs(float2 p : POSITION) : SV_Position { return float4(p, 0, 1); }\n"
    "float4 ps_red() : SV_Target { return float4(1.0, 0.0, 0.0, 1.0); }\n"
    "float4 ps_green() : SV_Target { return float4(0.0, 1.0, 0.0, 1.0); }\n"
    "float4 ps_blue() : SV_Target { return float4(0.0, 0.0, 1.0, 1.0); }\n";

/* BGRA in memory */
#define RED   0xffff0000u
#define GREEN 0xff00ff00u
#define BLUE  0xff0000ffu

static FILE *logf;
static int failures;
static IDXGIAdapter1 *adapter;

static void out(const char *fmt, ...)
{
    va_list a;
    va_start(a, fmt);
    vprintf(fmt, a);
    va_end(a);
    fflush(stdout);
    if (logf) {
        va_start(a, fmt);
        vfprintf(logf, fmt, a);
        va_end(a);
        fflush(logf);
    }
}

struct Dev {
    ID3D11Device *dev;
    ID3D11DeviceContext *ctx;
    ID3D11VertexShader *vs;
    ID3D11PixelShader *ps[3];
    ID3D11InputLayout *il;
    ID3D11Buffer *vb;
    ID3D11Texture2D *tex;
    UINT w, h;
};

static int pick_adapter(void)
{
    IDXGIFactory1 *fac = nullptr;
    IDXGIAdapter1 *a = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&fac)))
        return 100;
    for (UINT i = 0; fac->EnumAdapters1(i, &a) == S_OK; i++) {
        DXGI_ADAPTER_DESC1 d;
        a->GetDesc1(&d);
        if (!adapter && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
            (d.VendorId == 0x42525453 || wcsstr(d.Description, L"Mali") || wcsstr(d.Description, L"Starbite")))
            adapter = a;
        else
            a->Release();
    }
    if (!adapter) {
        out("FAIL no Starbite/Mali adapter\n");
        return 101;
    }
    return 0;
}

static bool texture(Dev *d, UINT w, UINT h)
{
    if (d->tex)
        d->tex->Release();
    d->tex = nullptr;
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w;
    td.Height = h;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    HRESULT hr = d->dev->CreateTexture2D(&td, nullptr, &d->tex);
    if (FAILED(hr)) {
        out("CreateTexture2D %ux%u failed: 0x%08lx\n", w, h, (unsigned long)hr);
        return false;
    }
    d->w = w;
    d->h = h;
    return true;
}

static int make(Dev *d, const char *name)
{
    memset(d, 0, sizeof(*d));
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_10_0, got;
    HRESULT hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, &fl, 1, D3D11_SDK_VERSION,
                                   &d->dev, &got, &d->ctx);
    if (FAILED(hr)) {
        out("TEST %-12s: FAIL D3D11CreateDevice 0x%08lx\n", name, (unsigned long)hr);
        failures++;
        return 1;
    }
    ID3DBlob *b = nullptr, *err = nullptr;
    if (FAILED(D3DCompile(kHlsl, sizeof(kHlsl) - 1, "sbtwo", nullptr, nullptr, "vs", "vs_4_0", 0, 0, &b, &err)) ||
        FAILED(d->dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &d->vs)))
        return 1;
    const float verts[] = { -1.0f, 1.0f, 3.0f, 1.0f, -1.0f, -3.0f };
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = sizeof(verts);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA init = { verts, 0, 0 };
    D3D11_INPUT_ELEMENT_DESC ied = { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 };
    if (FAILED(d->dev->CreateBuffer(&bd, &init, &d->vb)) ||
        FAILED(d->dev->CreateInputLayout(&ied, 1, b->GetBufferPointer(), b->GetBufferSize(), &d->il)))
        return 1;
    const char *entry[3] = { "ps_red", "ps_green", "ps_blue" };
    for (int i = 0; i < 3; i++) {
        ID3DBlob *pb = nullptr;
        if (FAILED(D3DCompile(kHlsl, sizeof(kHlsl) - 1, "sbtwo", nullptr, nullptr, entry[i], "ps_4_0", 0, 0, &pb, &err)) ||
            FAILED(d->dev->CreatePixelShader(pb->GetBufferPointer(), pb->GetBufferSize(), nullptr, &d->ps[i])))
            return 1;
        pb->Release();
    }
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    ID3D11RasterizerState *rs = nullptr;
    d->dev->CreateRasterizerState(&rd, &rs);
    d->ctx->RSSetState(rs);
    return texture(d, 256, 256) ? 0 : 1;
}

static void release(Dev *d)
{
    if (d->ctx) {
        d->ctx->ClearState();
        d->ctx->Flush();
    }
    if (d->tex) d->tex->Release();
    for (int i = 0; i < 3; i++)
        if (d->ps[i]) d->ps[i]->Release();
    if (d->vs) d->vs->Release();
    if (d->il) d->il->Release();
    if (d->vb) d->vb->Release();
    if (d->ctx) d->ctx->Release();
    if (d->dev) d->dev->Release();
    memset(d, 0, sizeof(*d));
}

static void draw(Dev *d, int colour)
{
    ID3D11RenderTargetView *rtv = nullptr;
    if (FAILED(d->dev->CreateRenderTargetView(d->tex, nullptr, &rtv)))
        return;
    const float black[4] = { 0, 0, 0, 1 };
    d->ctx->OMSetRenderTargets(1, &rtv, nullptr);
    d->ctx->ClearRenderTargetView(rtv, black);
    D3D11_VIEWPORT vp = { 0.0f, 0.0f, (float)d->w, (float)d->h, 0.0f, 1.0f };
    d->ctx->RSSetViewports(1, &vp);
    UINT stride = 8, offset = 0;
    d->ctx->IASetVertexBuffers(0, 1, &d->vb, &stride, &offset);
    d->ctx->IASetInputLayout(d->il);
    d->ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    d->ctx->VSSetShader(d->vs, nullptr, 0);
    d->ctx->PSSetShader(d->ps[colour], nullptr, 0);
    d->ctx->Draw(3, 0);
    rtv->Release();
}

/* the texture's centre and a corner, through a staging copy */
static bool read(Dev *d, UINT *c, UINT *k)
{
    D3D11_TEXTURE2D_DESC td;
    d->tex->GetDesc(&td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.MiscFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D *st = nullptr;
    if (FAILED(d->dev->CreateTexture2D(&td, nullptr, &st)))
        return false;
    d->ctx->CopyResource(st, d->tex);
    D3D11_MAPPED_SUBRESOURCE m;
    HRESULT hr = d->ctx->Map(st, 0, D3D11_MAP_READ, 0, &m);
    if (FAILED(hr)) {
        st->Release();
        return false;
    }
    *c = *(const UINT *)((const BYTE *)m.pData + (td.Height / 2) * m.RowPitch + (td.Width / 2) * 4);
    *k = *(const UINT *)((const BYTE *)m.pData + 3 * m.RowPitch + 3 * 4);
    d->ctx->Unmap(st, 0);
    st->Release();
    return true;
}

static bool check(const char *name, Dev *d, UINT want, bool quiet_pass)
{
    UINT c = 0, k = 0;
    bool got = read(d, &c, &k);
    HRESULT removed = d->dev->GetDeviceRemovedReason();
    bool ok = got && c == want && k == want && removed == S_OK;
    if (!ok || !quiet_pass)
        out("TEST %-12s: %s  got centre 0x%08x corner 0x%08x  want 0x%08x%s (removed 0x%08lx)\n", name,
            ok ? "PASS" : "FAIL", c, k, want, got ? "" : " (no readback)", (unsigned long)removed);
    if (!ok)
        failures++;
    return ok;
}

int main(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "hw") != 0) {
        printf("usage: sbtwo hw [LOG]\n");
        return 2;
    }
    if (argc > 2)
        logf = fopen(argv[2], "w");
    int r = pick_adapter();
    if (r)
        return r;

    Dev a, b, c, six[6];
    if (make(&a, "two-a"))
        return 110;
    draw(&a, 0);
    check("two-a", &a, RED, false);
    if (make(&b, "two-b"))
        return 111;
    draw(&b, 1);
    check("two-b", &b, GREEN, false);
    check("two-a-again", &a, RED, false);

    int bad = failures;
    for (int i = 1; i <= 200; i++) {
        if (i % 10 == 0) {
            texture(&a, 256 + (i % 30), 256);
            texture(&b, 256, 256 + (i % 30));
        }
        draw(&a, 0);
        draw(&b, 1);
        a.ctx->Flush();
        b.ctx->Flush();
        if (i % 25 == 0) {
            char n[32];
            snprintf(n, sizeof(n), "interleave%d", i);
            check(n, &a, RED, true);
            check(n, &b, GREEN, true);
        }
    }
    out("TEST %-12s: %s  (200 rounds, %d bad checks)\n", "interleave", failures == bad ? "PASS" : "FAIL", failures - bad);

    release(&a);
    if (make(&c, "after-a-gone"))
        return 112;
    draw(&c, 2);
    draw(&b, 1);
    check("after-a-c", &c, BLUE, false);
    check("after-a-b", &b, GREEN, false);
    release(&c);

    bad = failures;
    int made = 0;
    for (int i = 0; i < 6; i++)
        if (make(&six[i], "six") == 0)
            made++;
    for (int i = 0; i < made; i++)
        draw(&six[i], i % 3);
    for (int i = 0; i < made; i++)
        check("six-each", &six[i], i % 3 == 0 ? RED : i % 3 == 1 ? GREEN : BLUE, true);
    out("TEST %-12s: %s  (%d of 6 devices made, %d bad checks)\n", "six", made == 6 && failures == bad ? "PASS" : "FAIL",
        made, failures - bad);
    if (made != 6 && failures == bad)
        failures++;
    for (int i = 0; i < made; i++)
        release(&six[i]);

    /* pace: what one full-screen app frame costs, with no per-frame CPU wait of our own */
    if (texture(&b, 1440, 2960)) {
        LARGE_INTEGER f, t0, t1;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&t0);
        for (int i = 0; i < 300; i++) {
            draw(&b, i & 1 ? 1 : 2);
            b.ctx->Flush();
        }
        draw(&b, 1);
        check("pace-last", &b, GREEN, false);
        QueryPerformanceCounter(&t1);
        double ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart;
        out("PACE 1440x2960 clear+draw: %.2f ms per frame (%.1f fps) over 300 frames\n", ms / 301.0, 301000.0 / ms);
    }
    release(&b);
    out("sbtwo: %d failure(s)\n", failures);
    if (logf)
        fclose(logf);
    return failures;
}
