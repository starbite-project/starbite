// SPDX-License-Identifier: GPL-2.0-only
//
// sbdwm: DWM's composition draw, rebuilt one feature at a time (docs/WIN11-GPU.md 6.18).
// T13u: each DWM frame is one DrawIndexed (16-bit indices, 6 or 48 per frame, triangle list, restart 0xffff) into
// the presented 1440x2960 B8G8R8A8 target, full viewport, no depth, no predication, and the target stays all zero
// (alpha too). sbtex (non-indexed Draw, 64x64, no constant buffer, no blending) passes 9/9. Each test here adds
// one of DWM's features; the first failure names the cause.
//
//   sbdwm.exe hw [LOG]           the Starbite/Mali adapter (sbumd)
//   sbdwm.exe PATH\TO.dll [LOG]  a software reference (softpipe)
//
//   draw        64x64 RGBA, Draw 3, solid blue                       (sbtex's "solid", the control)
//   idx16       64x64 RGBA, DrawIndexed 6 x 16-bit, solid blue       (+ index buffer)
//   idx16off    the same with the index buffer bound at offset 12    (+ index buffer offset)
//   idx32       64x64 RGBA, DrawIndexed 6 x 32-bit                   (32-bit indices, for contrast)
//   cb          64x64 RGBA, Draw, vertex shader transform from a constant buffer   (+ constant buffer)
//   blend       64x64 RGBA cleared to 0, premultiplied blend (ONE, INV_SRC_ALPHA) of green   (+ blend)
//   big         1440x2960 B8G8R8A8 cleared to 0, Draw, solid blue    (+ DWM's target size and format)
//   dwm         1440x2960 B8G8R8A8 cleared to 0; DrawIndexed 6 x 16-bit; constant-buffer transform;
//               sampled texture; premultiplied blend: DWM's composition draw
// Exit code = the number of failed tests (100+: setup failed).
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
    "cbuffer X : register(b0) { float4 xf; };\n"                      // scale.xy, offset.zw
    "Texture2D t : register(t0);\n"
    "SamplerState s : register(s0);\n"
    "struct V { float4 p : SV_Position; float2 uv : TEXCOORD0; };\n"
    "V vs(float2 p : POSITION) {\n"
    "  V o; o.uv = float2(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5); o.p = float4(p, 0, 1); return o; }\n"
    "V vs_cb(float2 p : POSITION) {\n"
    "  V o; o.uv = float2(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5); o.p = float4(p * xf.xy + xf.zw, 0, 1); return o; }\n"
    "float4 ps_blue(V i) : SV_Target { return float4(0.0, 0.0, 1.0, 1.0); }\n"
    "float4 ps_green(V i) : SV_Target { return float4(0.0, 1.0, 0.0, 1.0); }\n"
    "float4 ps_sample(V i) : SV_Target { return t.Sample(s, i.uv); }\n"
    /* T13w: what a shader reads from a constant buffer, as a colour (cb = 0.25, 0.5, 0.75, 1.0) */
    "cbuffer Y : register(b1) { float4 yv; };\n"
    "struct C { float4 p : SV_Position; float4 c : COLOR0; };\n"
    "C vs_c0(float2 p : POSITION) { C o; o.p = float4(p, 0, 1); o.c = xf; return o; }\n"
    "C vs_c1(float2 p : POSITION) { C o; o.p = float4(p, 0, 1); o.c = yv; return o; }\n"
    "float4 ps_c(C i) : SV_Target { return i.c; }\n"
    "V vs_pass(float2 p : POSITION) { V o; o.uv = float2(0, 0); o.p = float4(p, 0, 1); return o; }\n"
    "float4 ps_c0(V i) : SV_Target { return xf; }\n"
    "float4 ps_c1(V i) : SV_Target { return yv; }\n";

static ID3D11Device *dev;
static ID3D11DeviceContext *ctx;
static FILE *logf;
static int failures;

static void out(const char *fmt, ...)
{
    va_list a;
    va_start(a, fmt);
    vprintf(fmt, a);
    va_end(a);
    if (logf) {
        va_start(a, fmt);
        vfprintf(logf, fmt, a);
        va_end(a);
        fflush(logf);
    }
}

static ID3DBlob *compile(const char *entry, const char *target)
{
    ID3DBlob *b = nullptr, *err = nullptr;
    if (FAILED(D3DCompile(kHlsl, sizeof(kHlsl) - 1, "sbdwm", nullptr, nullptr, entry, target, 0, 0, &b, &err))) {
        out("compile %s failed: %s\n", entry, err ? (char *)err->GetBufferPointer() : "?");
        return nullptr;
    }
    return b;
}

static ID3D11Texture2D *texture(UINT w, UINT h, DXGI_FORMAT fmt, UINT bind, const void *init, UINT pitch)
{
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w;
    td.Height = h;
    td.MipLevels = td.ArraySize = 1;
    td.Format = fmt;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = bind;
    D3D11_SUBRESOURCE_DATA sd = { init, pitch, 0 };
    ID3D11Texture2D *t = nullptr;
    dev->CreateTexture2D(&td, init ? &sd : nullptr, &t);
    return t;
}

static ID3D11Buffer *buffer(UINT bind, const void *data, UINT size)
{
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = size;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = bind;
    D3D11_SUBRESOURCE_DATA init = { data, 0, 0 };
    ID3D11Buffer *b = nullptr;
    dev->CreateBuffer(&bd, &init, &b);
    return b;
}

/* Pixel (x, y) of the target, read back through staging. */
static bool pixels(ID3D11Texture2D *rt, int n, const int *xy, UINT *out_px)
{
    D3D11_TEXTURE2D_DESC td;
    rt->GetDesc(&td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D *st = nullptr;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &st)))
        return false;
    ctx->CopyResource(st, rt);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
        st->Release();
        return false;
    }
    for (int i = 0; i < n; i++)
        out_px[i] = *(const UINT *)((const BYTE *)m.pData + xy[2 * i + 1] * m.RowPitch + xy[2 * i] * 4);
    ctx->Unmap(st, 0);
    st->Release();
    return true;
}

struct Pass {
    const char *name;
    UINT w, h;
    DXGI_FORMAT fmt;
    const char *vs, *ps;
    int indexed;          /* 0 Draw, 16, 32 */
    UINT ib_offset;
    bool cb, blend, clear0, sample;
    UINT want;            /* the pixel as stored */
};

static ID3D11Buffer *vb, *ib16, *ib32, *cbuf;
static ID3D11ShaderResourceView *srv;
static ID3D11SamplerState *smp;
static ID3D11InputLayout *il;
static ID3D11BlendState *premul;

static void run(const Pass &p)
{
    ID3DBlob *vsb = compile(p.vs, "vs_4_0"), *psb = compile(p.ps, "ps_4_0");
    ID3D11VertexShader *vs = nullptr;
    ID3D11PixelShader *ps = nullptr;
    if (!vsb || !psb || FAILED(dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs)) ||
        FAILED(dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps))) {
        out("TEST %-9s: FAIL (shaders)\n", p.name);
        failures++;
        return;
    }
    ID3D11Texture2D *rt = texture(p.w, p.h, p.fmt, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, nullptr, 0);
    ID3D11RenderTargetView *rtv = nullptr;
    if (!rt || FAILED(dev->CreateRenderTargetView(rt, nullptr, &rtv))) {
        out("TEST %-9s: FAIL (target)\n", p.name);
        failures++;
        return;
    }
    const float grey[4] = { 0.5f, 0.5f, 0.5f, 1.0f }, zero[4] = { 0, 0, 0, 0 };
    ctx->ClearRenderTargetView(rtv, p.clear0 ? zero : grey);
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    const float bf[4] = { 0, 0, 0, 0 };
    ctx->OMSetBlendState(p.blend ? premul : nullptr, bf, 0xffffffff);
    D3D11_VIEWPORT vp = { 0.0f, 0.0f, (float)p.w, (float)p.h, 0.0f, 1.0f };
    ctx->RSSetViewports(1, &vp);
    UINT stride = 8, offset = 0;
    ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
    ctx->IASetInputLayout(il);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vs, nullptr, 0);
    ctx->VSSetConstantBuffers(0, 1, &cbuf);
    ctx->PSSetShader(ps, nullptr, 0);
    ctx->PSSetShaderResources(0, 1, &srv);
    ctx->PSSetSamplers(0, 1, &smp);
    if (p.indexed == 16) {
        ctx->IASetIndexBuffer(ib16, DXGI_FORMAT_R16_UINT, p.ib_offset);
        ctx->DrawIndexed(6, 0, 0);
    } else if (p.indexed == 32) {
        ctx->IASetIndexBuffer(ib32, DXGI_FORMAT_R32_UINT, 0);
        ctx->DrawIndexed(6, 0, 0);
    } else {
        ctx->Draw(6, 0);   /* the 4 quad corners as 2 triangles: vertices 0..5 of the list below */
    }
    ID3D11RenderTargetView *nrt = nullptr;
    ctx->OMSetRenderTargets(1, &nrt, nullptr);
    ctx->OMSetBlendState(nullptr, bf, 0xffffffff);
    int xy[6] = { (int)p.w / 4, (int)p.h / 2, (int)p.w / 2, (int)p.h / 2, (int)(3 * p.w / 4), (int)p.h / 4 };
    UINT px[3] = {};
    bool read = pixels(rt, 3, xy, px);
    bool ok = read && px[0] == p.want && px[1] == p.want && px[2] == p.want;
    out("TEST %-9s: %s  got 0x%08x 0x%08x 0x%08x  want 0x%08x\n", p.name, ok ? "PASS" : "FAIL", px[0], px[1], px[2],
        p.want);
    if (!ok)
        failures++;
    rtv->Release();
    rt->Release();
}

int main(int argc, char **argv)
{
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    const char *dll = argc > 1 ? argv[1] : "hw";
    bool hw = _stricmp(dll, "hw") == 0;
    logf = fopen(argc > 2 ? argv[2] : "sbdwm.log", "a");
    out("sbdwm %s\n", dll);
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_10_0, got;
    HRESULT hr;
    if (hw) {
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
        hr = D3D11CreateDevice(pick, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev, &got, &ctx);
    } else {
        HMODULE sw = LoadLibraryA(dll);
        if (!sw)
            return 103;
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_SOFTWARE, sw, 0, &fl, 1, D3D11_SDK_VERSION, &dev, &got, &ctx);
    }
    if (FAILED(hr)) {
        out("FAIL D3D11CreateDevice: 0x%08lx\n", (unsigned long)hr);
        return 102;
    }
    out("device created (%s), feature level 0x%x\n", hw ? "hardware" : "software", got);

    // a quad over the whole target: as a list of 6 vertices (Draw) and as 4 corners + indices (DrawIndexed)
    const float list[] = { -1, 1, 1, 1, -1, -1, -1, -1, 1, 1, 1, -1, /* the corners for the indexed draws: */
                           -1, 1, 1, 1, -1, -1, 1, -1 };
    vb = buffer(D3D11_BIND_VERTEX_BUFFER, list, sizeof(list));
    const USHORT i16[] = { 6, 7, 8, 8, 7, 9,      /* offset 0 */
                           6, 7, 8, 8, 7, 9 };    /* offset 12 (bytes) */
    const UINT i32[] = { 6, 7, 8, 8, 7, 9 };
    ib16 = buffer(D3D11_BIND_INDEX_BUFFER, i16, sizeof(i16));
    ib32 = buffer(D3D11_BIND_INDEX_BUFFER, i32, sizeof(i32));
    const float xf[4] = { 1.0f, 1.0f, 0.0f, 0.0f };   // identity: the quad must still cover everything
    cbuf = buffer(D3D11_BIND_CONSTANT_BUFFER, xf, sizeof(xf));
    ID3DBlob *vsb = compile("vs", "vs_4_0");
    D3D11_INPUT_ELEMENT_DESC ied = { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 };
    if (!vb || !ib16 || !ib32 || !cbuf || !vsb ||
        FAILED(dev->CreateInputLayout(&ied, 1, vsb->GetBufferPointer(), vsb->GetBufferSize(), &il)))
        return 104;
    static UINT tex[64 * 64];
    for (int i = 0; i < 64 * 64; i++)
        tex[i] = 0xff00ff00u;                         // opaque green, RGBA8 and BGRA8 alike
    ID3D11Texture2D *t = texture(64, 64, DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, tex, 64 * 4);
    if (!t || FAILED(dev->CreateShaderResourceView(t, nullptr, &srv)))
        return 105;
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    dev->CreateSamplerState(&sd, &smp);
    D3D11_BLEND_DESC bd = {};
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    dev->CreateBlendState(&bd, &premul);
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    ID3D11RasterizerState *rs = nullptr;
    dev->CreateRasterizerState(&rd, &rs);
    ctx->RSSetState(rs);

    const DXGI_FORMAT RGBA = DXGI_FORMAT_R8G8B8A8_UNORM, BGRA = DXGI_FORMAT_B8G8R8A8_UNORM;
    const UINT BLUE_RGBA = 0xffff0000u, BLUE_BGRA = 0xff0000ffu, GREEN = 0xff00ff00u;
    const Pass passes[] = {
        { "draw", 64, 64, RGBA, "vs", "ps_blue", 0, 0, false, false, false, false, BLUE_RGBA },
        { "idx16", 64, 64, RGBA, "vs", "ps_blue", 16, 0, false, false, false, false, BLUE_RGBA },
        { "idx16off", 64, 64, RGBA, "vs", "ps_blue", 16, 12, false, false, false, false, BLUE_RGBA },
        { "idx32", 64, 64, RGBA, "vs", "ps_blue", 32, 0, false, false, false, false, BLUE_RGBA },
        { "cb", 64, 64, RGBA, "vs_cb", "ps_blue", 0, 0, true, false, false, false, BLUE_RGBA },
        { "blend", 64, 64, RGBA, "vs", "ps_green", 0, 0, false, true, true, false, GREEN },
        { "big", 1440, 2960, BGRA, "vs", "ps_blue", 0, 0, false, false, true, false, BLUE_BGRA },
        { "dwm", 1440, 2960, BGRA, "vs_cb", "ps_sample", 16, 12, true, true, true, true, GREEN },
    };
    for (const Pass &p : passes)
        run(p);

    /* ---- T13w: the constant-buffer bisect. Each draws a full quad with the colour the shader read from a
     * constant buffer: want 0.25, 0.5, 0.75, 1.0 = RGBA8 0xffbf8040. */
    {
        const float cv[4] = { 0.25f, 0.5f, 0.75f, 1.0f };
        struct V { const char *name, *vs, *ps; int slot; int kind; } vars[] = {
            /* kind: 0 DEFAULT + initial data, 1 DEFAULT + UpdateSubresource, 2 DYNAMIC + Map(WRITE_DISCARD) */
            { "cb-vs-b0", "vs_c0", "ps_c", 0, 0 },
            { "cb-vs-b1", "vs_c1", "ps_c", 1, 0 },
            { "cb-ps-b0", "vs_pass", "ps_c0", 0, 0 },
            { "cb-ps-b1", "vs_pass", "ps_c1", 1, 0 },
            { "cb-vs-upd", "vs_c0", "ps_c", 0, 1 },
            { "cb-vs-dyn", "vs_c0", "ps_c", 0, 2 },
            { "cb-ps-dyn", "vs_pass", "ps_c0", 0, 2 },
        };
        for (const V &v : vars) {
            D3D11_BUFFER_DESC bd2 = {};
            bd2.ByteWidth = 16;
            bd2.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            bd2.Usage = v.kind == 2 ? D3D11_USAGE_DYNAMIC : D3D11_USAGE_DEFAULT;
            bd2.CPUAccessFlags = v.kind == 2 ? D3D11_CPU_ACCESS_WRITE : 0;
            D3D11_SUBRESOURCE_DATA init = { cv, 0, 0 };
            ID3D11Buffer *cb = nullptr;
            if (FAILED(dev->CreateBuffer(&bd2, v.kind == 0 ? &init : nullptr, &cb))) {
                out("TEST %-9s: FAIL (CreateBuffer)\n", v.name);
                failures++;
                continue;
            }
            if (v.kind == 1)
                ctx->UpdateSubresource(cb, 0, nullptr, cv, 0, 0);
            if (v.kind == 2) {
                D3D11_MAPPED_SUBRESOURCE m;
                if (SUCCEEDED(ctx->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
                    memcpy(m.pData, cv, 16);
                    ctx->Unmap(cb, 0);
                }
            }
            ID3DBlob *vsb2 = compile(v.vs, "vs_4_0"), *psb2 = compile(v.ps, "ps_4_0");
            ID3D11VertexShader *vs2 = nullptr;
            ID3D11PixelShader *ps2 = nullptr;
            ID3D11InputLayout *il2 = nullptr;
            ID3D11Texture2D *rt = texture(64, 64, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_RENDER_TARGET, nullptr, 0);
            ID3D11RenderTargetView *rtv = nullptr;
            if (!vsb2 || !psb2 || !rt ||
                FAILED(dev->CreateVertexShader(vsb2->GetBufferPointer(), vsb2->GetBufferSize(), nullptr, &vs2)) ||
                FAILED(dev->CreatePixelShader(psb2->GetBufferPointer(), psb2->GetBufferSize(), nullptr, &ps2)) ||
                FAILED(dev->CreateInputLayout(&ied, 1, vsb2->GetBufferPointer(), vsb2->GetBufferSize(), &il2)) ||
                FAILED(dev->CreateRenderTargetView(rt, nullptr, &rtv))) {
                out("TEST %-9s: FAIL (setup)\n", v.name);
                failures++;
                continue;
            }
            const float grey[4] = { 0.5f, 0.5f, 0.5f, 1.0f };
            ctx->ClearRenderTargetView(rtv, grey);
            ctx->OMSetRenderTargets(1, &rtv, nullptr);
            D3D11_VIEWPORT vp = { 0.0f, 0.0f, 64.0f, 64.0f, 0.0f, 1.0f };
            ctx->RSSetViewports(1, &vp);
            UINT stride = 8, offset = 0;
            ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
            ctx->IASetInputLayout(il2);
            ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ctx->VSSetShader(vs2, nullptr, 0);
            ctx->PSSetShader(ps2, nullptr, 0);
            ID3D11Buffer *none = nullptr;
            ctx->VSSetConstantBuffers(0, 1, &none);
            ctx->VSSetConstantBuffers(1, 1, &none);
            ctx->PSSetConstantBuffers(0, 1, &none);
            ctx->PSSetConstantBuffers(1, 1, &none);
            ctx->VSSetConstantBuffers(v.slot, 1, &cb);
            ctx->PSSetConstantBuffers(v.slot, 1, &cb);
            ctx->Draw(6, 0);
            ID3D11RenderTargetView *nrt = nullptr;
            ctx->OMSetRenderTargets(1, &nrt, nullptr);
            int xy[2] = { 32, 32 };
            UINT px = 0;
            bool ok = pixels(rt, 1, xy, &px) && (px == 0xffbf8040u || px == 0xffc08040u);
            out("TEST %-9s: %s  got 0x%08x  want 0xffbf8040 (grey 0xff808080 = nothing drawn)\n", v.name,
                ok ? "PASS" : "FAIL", px);
            if (!ok)
                failures++;
            rtv->Release();
            rt->Release();
            cb->Release();
        }
    }
    out("sbdwm %s: %d test(s) failed\n", dll, failures);
    if (logf)
        fclose(logf);
    return failures;
}
