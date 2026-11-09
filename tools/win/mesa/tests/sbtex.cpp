// SPDX-License-Identifier: GPL-2.0-only
//
// sbtex: does texturing work through our D3D10 UMD (sbumd: d3d10umd + Panfrost, docs/WIN11-GPU.md 6.18)?
// DWM composes the desktop by sampling window surfaces, and T13k's DWM produced only black frames on the Mali.
// sb-patches/0102 sends d3d10umd's D3D10 texture opcodes down a legacy TEX path for NIR-only drivers, which
// nothing has checked on the real GPU yet. wddm-mali-g52's triangle samples nothing.
//
//   sbtex.exe hw            the adapter whose description names the Mali (the hardware UMD, sbumd.dll)
//   sbtex.exe PATH\TO.dll   that DLL as a software adapter (a reference: softpipe takes TGSI directly)
//
// Each test draws a full-screen triangle into a 64x64 target and checks pixels:
//   clear       ClearRenderTargetView, RGBA8                                      (no shader)
//   clearbgra   the same into a B8G8R8A8 target (DWM's format)                    (no shader)
//   solid       a constant colour from the pixel shader                           (no texture)
//   sample      Texture2D.Sample, point sampler     (D3D10 SAMPLE   -> TEX)
//   level       Texture2D.SampleLevel(.., 0)        (SAMPLE_L       -> TXL)
//   load        Texture2D.Load                      (LD             -> TXF)
//   dims        Texture2D.GetDimensions             (RESINFO/SVIEWINFO -> TXQ)
//   bgra        Sample of a B8G8R8A8 texture        (DWM's surfaces)
//   linear      Sample with a bilinear sampler, inside a flat area
//   twotex      t0 and t1 through one sampler, s0 (Direct2D's text shaders; T16, sb-patches/0114: the legacy path
//               read t1 at the sampler's index, t0, and text never drew in the user's T15 session)
//   swizzle     Sample(..).gbra (the view swizzle the legacy path dropped)
//   a8          an A8_UNORM texture's alpha (glyph-atlas style)
//   uintattr    a colour packed in an R32_UINT vertex attribute, unpacked with integer ops in the vertex shader
//               (Direct2D's way; T18, sb-patches/0115: Bifrost loaded it converted to a float value)
//   instattr    the same as a per-instance attribute, DrawInstanced
//   int-*       uint4 vertex inputs from narrower integer formats (R8G8B8A8_UINT, R16G16_UINT, R16G16B16A16_UINT,
//               R32G32B32A32_UINT) and an R8G8B8A8_UNORM float control (T20: Direct2D uses integer attributes)
// The texture: left half green, right half red (RGBA8 0xff00ff00 / 0xff0000ff). Output lines "TEST name: PASS|FAIL
// got ... want ..."; exit code = the number of failed tests (100 + n if the device could not be made).
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
    "Texture2D t : register(t0);\n"
    "SamplerState s : register(s0);\n"
    "struct V { float4 p : SV_Position; float2 uv : TEXCOORD0; };\n"
    /* T13n: positions from a vertex buffer. T13m's SV_VertexID version crashed the hardware UMD: Panfrost's
     * Bifrost backend has no load_vertex_id_zero_base ("Unhandled intrinsic"), the value stayed undefined */
    "V vs(float2 p : POSITION) {\n"
    "  V o; o.uv = float2(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5); o.p = float4(p, 0, 1); return o; }\n"
    "V vs_id(uint id : SV_VertexID) {\n"
    "  V o; float2 uv = float2((id << 1) & 2, id & 2);\n"
    "  o.uv = uv; o.p = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); return o; }\n"
    "Texture2D t1 : register(t1);\n"
    "float4 ps_twotex(V i) : SV_Target { return float4(0, t.Sample(s, i.uv).g, t1.Sample(s, i.uv).b, 1); }\n"
    "float4 ps_swizzle(V i) : SV_Target { return t.Sample(s, i.uv).gbra; }\n"
    "float4 ps_alpha(V i) : SV_Target { return float4(t.Sample(s, i.uv).a, 0, 0, 1); }\n"
    "struct VC { float4 p : SV_Position; float4 c : COLOR0; };\n"
    "VC vs_uint(float2 p : POSITION, uint c : COLOR) {\n"
    "  VC o; o.p = float4(p, 0, 1);\n"
    "  o.c = float4((c >> 16) & 255, (c >> 8) & 255, c & 255, c >> 24) / 255.0; return o; }\n"
    "float4 ps_col(VC i) : SV_Target { return i.c; }\n"
    "VC vs_u4(float2 p : POSITION, uint4 c : COLOR) { VC o; o.p = float4(p, 0, 1); o.c = float4(c) / 255.0; return o; }\n"
    "VC vs_f4(float2 p : POSITION, float4 c : COLOR) { VC o; o.p = float4(p, 0, 1); o.c = c; return o; }\n"
    "float4 ps_solid(V i) : SV_Target { return float4(0.0, 0.0, 1.0, 1.0); }\n"
    "float4 ps_sample(V i) : SV_Target { return t.Sample(s, i.uv); }\n"
    "float4 ps_level(V i) : SV_Target { return t.SampleLevel(s, i.uv, 0); }\n"
    "float4 ps_load(V i) : SV_Target { return t.Load(int3(i.p.xy, 0)); }\n"
    "float4 ps_dims(V i) : SV_Target { uint w, h; t.GetDimensions(w, h); return float4(w / 255.0, h / 255.0, 0, 1); }\n";

static ID3D11Device *dev;
static ID3D11DeviceContext *ctx;
static ID3D11VertexShader *vs;
static ID3D11InputLayout *il;
static ID3D11Buffer *vb;
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
    HRESULT hr = D3DCompile(kHlsl, sizeof(kHlsl) - 1, "sbtex", nullptr, nullptr, entry, target, 0, 0, &b, &err);
    if (FAILED(hr)) {
        out("compile %s failed: %s\n", entry, err ? (char *)err->GetBufferPointer() : "?");
        return nullptr;
    }
    return b;
}

static ID3D11Texture2D *texture(DXGI_FORMAT fmt, UINT bind, const void *init, UINT pitch)
{
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = td.Height = 64;
    td.MipLevels = td.ArraySize = 1;
    td.Format = fmt;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = bind;
    D3D11_SUBRESOURCE_DATA sd = { init, pitch, 0 };
    ID3D11Texture2D *t = nullptr;
    if (FAILED(dev->CreateTexture2D(&td, init ? &sd : nullptr, &t)))
        return nullptr;
    return t;
}

/* Reads the 64x64 target back. px[] receives the pixels as stored (32-bit). */
static bool readback(ID3D11Texture2D *rt, DXGI_FORMAT fmt, UINT *px)
{
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = td.Height = 64;
    td.MipLevels = td.ArraySize = 1;
    td.Format = fmt;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_STAGING;
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
    for (int y = 0; y < 64; y++)
        memcpy(px + y * 64, (const BYTE *)m.pData + y * m.RowPitch, 64 * 4);
    ctx->Unmap(st, 0);
    st->Release();
    return true;
}

static void check(const char *name, const UINT *px, int x1, UINT want1, int x2, UINT want2, UINT mask)
{
    UINT a = px[32 * 64 + x1], b = px[32 * 64 + x2];
    bool ok = (a & mask) == (want1 & mask) && (b & mask) == (want2 & mask);
    UINT distinct[4] = {}, n = 0;
    for (int i = 0; i < 64 * 64 && n < 4; i++) {
        UINT j;
        for (j = 0; j < n && distinct[j] != px[i]; j++)
            ;
        if (j == n)
            distinct[n++] = px[i];
    }
    out("TEST %-9s: %s  got (%d,32)=0x%08x (%d,32)=0x%08x  want 0x%08x 0x%08x  colours:", name, ok ? "PASS" : "FAIL",
        x1, a, x2, b, want1, want2);
    for (UINT i = 0; i < n; i++)
        out(" 0x%08x", distinct[i]);
    out("\n");
    if (!ok)
        failures++;
}

/* One full-screen pass with pixel shader `ps` sampling `srv` through `smp`, into a fresh RGBA8 target. */
static bool pass(const char *ps_entry, ID3D11ShaderResourceView *srv, ID3D11SamplerState *smp, UINT *px,
                 ID3D11ShaderResourceView *srv1 = nullptr)
{
    ID3DBlob *b = compile(ps_entry, "ps_4_0");
    if (!b)
        return false;
    ID3D11PixelShader *ps = nullptr;
    if (FAILED(dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &ps)))
        return false;
    ID3D11Texture2D *rt = texture(DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_RENDER_TARGET, nullptr, 0);
    ID3D11RenderTargetView *rtv = nullptr;
    if (!rt || FAILED(dev->CreateRenderTargetView(rt, nullptr, &rtv)))
        return false;
    const float grey[4] = { 0.5f, 0.5f, 0.5f, 1.0f };
    ctx->ClearRenderTargetView(rtv, grey);
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    D3D11_VIEWPORT vp = { 0.0f, 0.0f, 64.0f, 64.0f, 0.0f, 1.0f };
    ctx->RSSetViewports(1, &vp);
    UINT stride = 8, offset = 0;
    ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
    ctx->IASetInputLayout(il);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vs, nullptr, 0);
    ctx->PSSetShader(ps, nullptr, 0);
    ID3D11ShaderResourceView *srvs[2] = { srv, srv1 };
    ctx->PSSetShaderResources(0, 2, srvs);
    ctx->PSSetSamplers(0, 1, &smp);
    ctx->Draw(3, 0);
    bool ok = readback(rt, DXGI_FORMAT_R8G8B8A8_UNORM, px);
    ID3D11ShaderResourceView *none[2] = { nullptr, nullptr };
    ctx->PSSetShaderResources(0, 2, none);
    rtv->Release();
    rt->Release();
    ps->Release();
    return ok;
}

int main(int argc, char **argv)
{
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    const char *dll = argc > 1 ? argv[1] : "hw";
    bool hw = _stricmp(dll, "hw") == 0;
    logf = fopen(argc > 2 ? argv[2] : "sbtex.log", "a");
    SYSTEMTIME st;
    GetLocalTime(&st);
    out("sbtex %s at %02u:%02u:%02u\n", dll, st.wHour, st.wMinute, st.wSecond);
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_10_0, got;
    if (hw) {
        IDXGIFactory1 *fac = nullptr;
        IDXGIAdapter1 *pick = nullptr, *a = nullptr;
        if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&fac))) {
            out("FAIL CreateDXGIFactory1\n");
            return 100;
        }
        for (UINT i = 0; fac->EnumAdapters1(i, &a) == S_OK; i++) {
            DXGI_ADAPTER_DESC1 d;
            a->GetDesc1(&d);
            out("adapter %u: %ls (vendor 0x%04x device 0x%04x flags 0x%x)\n", i, d.Description, d.VendorId,
                d.DeviceId, d.Flags);
            /* S9 (T13m): our adapter is ACPI\\STRB0072, vendor "STRB" (0x42525453); its description is the device's
             * DeviceDesc ("Starbite Display (Galaxy S9 DECON)" on the phone), so the name alone misses it */
            if (!pick && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
                (d.VendorId == 0x42525453 || wcsstr(d.Description, L"Mali") || wcsstr(d.Description, L"Starbite")))
                pick = a;
            else
                a->Release();
        }
        if (!pick) {
            out("FAIL no Starbite/Mali adapter (vendor 0x42525453)\n");
            return 101;
        }
        HRESULT hr = D3D11CreateDevice(pick, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev,
                                       &got, &ctx);
        if (FAILED(hr)) {
            out("FAIL D3D11CreateDevice (hardware): 0x%08lx\n", (unsigned long)hr);
            return 102;
        }
    } else {
        HMODULE sw = LoadLibraryA(dll);
        if (!sw) {
            out("FAIL LoadLibrary(%s): %lu\n", dll, GetLastError());
            return 103;
        }
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_SOFTWARE, sw, 0, &fl, 1, D3D11_SDK_VERSION, &dev,
                                       &got, &ctx);
        if (FAILED(hr)) {
            out("FAIL D3D11CreateDevice (software): 0x%08lx\n", (unsigned long)hr);
            return 104;
        }
    }
    out("device created (%s), feature level 0x%x\n", hw ? "hardware" : "software", got);

    bool vid = argc > 3 && strcmp(argv[3], "vertexid") == 0;   /* the SV_VertexID shader instead (T13m's crash) */
    ID3DBlob *vsb = compile(vid ? "vs_id" : "vs", "vs_4_0");
    if (vid)
        out("vertex shader: SV_VertexID (expect a crash on the Mali until Panfrost handles it)\n");
    if (!vsb || FAILED(dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs))) {
        out("FAIL vertex shader\n");
        return 105;
    }
    {   // one triangle covering the 64x64 target (clip space), as the g52 triangle feeds its vertices
        const float verts[] = { -1.0f, 1.0f, 3.0f, 1.0f, -1.0f, -3.0f };
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = sizeof(verts);
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        D3D11_SUBRESOURCE_DATA init = { verts, 0, 0 };
        D3D11_INPUT_ELEMENT_DESC ied = { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 };
        if (FAILED(dev->CreateBuffer(&bd, &init, &vb)) ||
            (!vid && FAILED(dev->CreateInputLayout(&ied, 1, vsb->GetBufferPointer(), vsb->GetBufferSize(), &il)))) {
            out("FAIL vertex buffer / input layout\n");
            return 107;
        }
    }
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    ID3D11RasterizerState *rs = nullptr;
    dev->CreateRasterizerState(&rd, &rs);
    ctx->RSSetState(rs);

    // The texture: left half green, right half red. RGBA8 in memory R,G,B,A -> little-endian 0xAABBGGRR.
    static UINT rgba[64 * 64], bgra[64 * 64];
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {
            rgba[y * 64 + x] = x < 32 ? 0xff00ff00u : 0xff0000ffu;
            bgra[y * 64 + x] = x < 32 ? 0xff00ff00u : 0xffff0000u;   // BGRA in memory: red = B 0, G 0, R ff
        }
    ID3D11Texture2D *tex = texture(DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, rgba, 64 * 4);
    ID3D11Texture2D *texb = texture(DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, bgra, 64 * 4);
    ID3D11ShaderResourceView *srv = nullptr, *srvb = nullptr;
    if (!tex || !texb || FAILED(dev->CreateShaderResourceView(tex, nullptr, &srv)) ||
        FAILED(dev->CreateShaderResourceView(texb, nullptr, &srvb))) {
        out("FAIL textures\n");
        return 106;
    }
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    ID3D11SamplerState *point = nullptr, *linear = nullptr;
    dev->CreateSamplerState(&sd, &point);
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    dev->CreateSamplerState(&sd, &linear);

    static UINT px[64 * 64];
    const UINT green = 0xff00ff00u, red = 0xff0000ffu, blue = 0xffff0000u;

    {   // clears, no shader
        for (int f = 0; f < 2; f++) {
            DXGI_FORMAT fmt = f ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
            ID3D11Texture2D *rt = texture(fmt, D3D11_BIND_RENDER_TARGET, nullptr, 0);
            ID3D11RenderTargetView *rtv = nullptr;
            if (rt && SUCCEEDED(dev->CreateRenderTargetView(rt, nullptr, &rtv))) {
                const float c[4] = { 0.0f, 0.0f, 1.0f, 1.0f };   // blue
                ctx->ClearRenderTargetView(rtv, c);
                if (readback(rt, fmt, px))
                    check(f ? "clearbgra" : "clear", px, 16, f ? 0xff0000ffu : blue, 48, f ? 0xff0000ffu : blue,
                          0xffffffffu);
                rtv->Release();
                rt->Release();
            } else {
                out("TEST %s: FAIL (no target)\n", f ? "clearbgra" : "clear");
                failures++;
            }
        }
    }
    if (pass("ps_solid", srv, point, px)) check("solid", px, 16, blue, 48, blue, 0xffffffffu);
    else out("TEST solid    : FAIL (pass)\n"), failures++;
    if (pass("ps_sample", srv, point, px)) check("sample", px, 16, green, 48, red, 0xffffffffu);
    else out("TEST sample   : FAIL (pass)\n"), failures++;
    if (pass("ps_level", srv, point, px)) check("level", px, 16, green, 48, red, 0xffffffffu);
    else out("TEST level    : FAIL (pass)\n"), failures++;
    if (pass("ps_load", srv, point, px)) check("load", px, 16, green, 48, red, 0xffffffffu);
    else out("TEST load     : FAIL (pass)\n"), failures++;
    // 64/255 in R and G, rounded: 0x40; compare R, G, A only (B 0)
    if (pass("ps_dims", srv, point, px)) check("dims", px, 16, 0xff004040u, 48, 0xff004040u, 0xffffffffu);
    else out("TEST dims     : FAIL (pass)\n"), failures++;
    // the BGRA texture read through the shader returns RGBA: green and red as in the RGBA one
    if (pass("ps_sample", srvb, point, px)) check("bgra", px, 16, green, 48, red, 0xffffffffu);
    else out("TEST bgra     : FAIL (pass)\n"), failures++;
    if (pass("ps_sample", srv, linear, px)) check("linear", px, 8, green, 56, red, 0xffffffffu);
    else out("TEST linear   : FAIL (pass)\n"), failures++;

    {   // T16: what Direct2D's text and images need (sb-patches/0114)
        static UINT solid[64 * 64], alpha[64 * 16];
        for (int i = 0; i < 64 * 64; i++)
            solid[i] = blue;
        for (int y = 0; y < 64; y++)
            for (int x = 0; x < 64; x++)
                ((BYTE *)alpha)[y * 64 + x] = x < 32 ? 0xff : 0x40;
        ID3D11Texture2D *tb = texture(DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, solid, 64 * 4);
        ID3D11Texture2D *ta = texture(DXGI_FORMAT_A8_UNORM, D3D11_BIND_SHADER_RESOURCE, alpha, 64);
        ID3D11ShaderResourceView *srvblue = nullptr, *srva = nullptr;
        if (tb && SUCCEEDED(dev->CreateShaderResourceView(tb, nullptr, &srvblue)) &&
            pass("ps_twotex", srv, point, px, srvblue))
            check("twotex", px, 16, 0xffffff00u, 48, 0xffff0000u, 0xffffffffu);   // (0, g of t0, b of t1, 1)
        else out("TEST twotex   : FAIL (pass)\n"), failures++;
        if (pass("ps_swizzle", srv, point, px))
            check("swizzle", px, 16, red, 48, blue, 0xffffffffu);                  // green.gbra = red; red.gbra = blue
        else out("TEST swizzle  : FAIL (pass)\n"), failures++;
        if (ta && SUCCEEDED(dev->CreateShaderResourceView(ta, nullptr, &srva)) && pass("ps_alpha", srva, point, px))
            check("a8", px, 16, 0xff0000ffu, 48, 0xff000040u, 0xffffffffu);
        else out("TEST a8       : FAIL (pass or A8_UNORM unsupported)\n"), failures++;
    }

    {   // T18: packed integer vertex attributes (sb-patches/0115)
        ID3DBlob *vb2 = compile("vs_uint", "vs_4_0"), *pb2 = compile("ps_col", "ps_4_0");
        ID3D11VertexShader *vsu = nullptr;
        ID3D11PixelShader *psc = nullptr;
        ID3D11InputLayout *ilv = nullptr, *ili = nullptr;
        ID3D11Buffer *colv = nullptr, *coli = nullptr;
        const UINT packed[3] = { 0xff2060c0u, 0xff2060c0u, 0xff2060c0u };   /* A R G B: (0x20, 0x60, 0xc0) */
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = sizeof(packed);
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        D3D11_SUBRESOURCE_DATA init = { packed, 0, 0 };
        D3D11_INPUT_ELEMENT_DESC iv[2] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "COLOR", 0, DXGI_FORMAT_R32_UINT, 1, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 } };
        D3D11_INPUT_ELEMENT_DESC ii[2] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "COLOR", 0, DXGI_FORMAT_R32_UINT, 1, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1 } };
        bool setup = vb2 && pb2 &&
            SUCCEEDED(dev->CreateVertexShader(vb2->GetBufferPointer(), vb2->GetBufferSize(), nullptr, &vsu)) &&
            SUCCEEDED(dev->CreatePixelShader(pb2->GetBufferPointer(), pb2->GetBufferSize(), nullptr, &psc)) &&
            SUCCEEDED(dev->CreateInputLayout(iv, 2, vb2->GetBufferPointer(), vb2->GetBufferSize(), &ilv)) &&
            SUCCEEDED(dev->CreateInputLayout(ii, 2, vb2->GetBufferPointer(), vb2->GetBufferSize(), &ili)) &&
            SUCCEEDED(dev->CreateBuffer(&bd, &init, &colv)) && SUCCEEDED(dev->CreateBuffer(&bd, &init, &coli));
        for (int inst = 0; inst < 2; inst++) {
            const char *name = inst ? "instattr" : "uintattr";
            ID3D11Texture2D *rt = setup ? texture(DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_RENDER_TARGET, nullptr, 0) : nullptr;
            ID3D11RenderTargetView *rtv = nullptr;
            if (!rt || FAILED(dev->CreateRenderTargetView(rt, nullptr, &rtv))) {
                out("TEST %-9s: FAIL (setup)\n", name);
                failures++;
                continue;
            }
            const float grey[4] = { 0.5f, 0.5f, 0.5f, 1.0f };
            ctx->ClearRenderTargetView(rtv, grey);
            ctx->OMSetRenderTargets(1, &rtv, nullptr);
            D3D11_VIEWPORT vp = { 0.0f, 0.0f, 64.0f, 64.0f, 0.0f, 1.0f };
            ctx->RSSetViewports(1, &vp);
            ID3D11Buffer *bufs[2] = { vb, inst ? coli : colv };
            UINT strides[2] = { 8, 4 }, offs[2] = { 0, 0 };
            ctx->IASetVertexBuffers(0, 2, bufs, strides, offs);
            ctx->IASetInputLayout(inst ? ili : ilv);
            ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ctx->VSSetShader(vsu, nullptr, 0);
            ctx->PSSetShader(psc, nullptr, 0);
            if (inst)
                ctx->DrawInstanced(3, 1, 0, 0);
            else
                ctx->Draw(3, 0);
            if (readback(rt, DXGI_FORMAT_R8G8B8A8_UNORM, px))
                check(name, px, 16, 0xffc06020u, 48, 0xffc06020u, 0xffffffffu);   /* R 0x20 G 0x60 B 0xc0 A 0xff */
            else
                out("TEST %-9s: FAIL (readback)\n", name), failures++;
            rtv->Release();
            rt->Release();
        }
        /* T20: narrower integer formats; components 0x20, 0x60, 0xc0, 0xff (missing ones read as 0, 0, 1) */
        struct { const char *name; DXGI_FORMAT fmt; UINT stride; BYTE data[16]; UINT want; bool flt; } nf[] = {
            { "int-rgba8", DXGI_FORMAT_R8G8B8A8_UINT, 4, { 0x20, 0x60, 0xc0, 0xff }, 0xffc06020u, false },
            { "int-rg16", DXGI_FORMAT_R16G16_UINT, 4, { 0x20, 0, 0x60, 0 }, 0x01006020u, false },
            { "int-rgba16", DXGI_FORMAT_R16G16B16A16_UINT, 8, { 0x20, 0, 0x60, 0, 0xc0, 0, 0xff, 0 }, 0xffc06020u, false },
            { "int-rgba32", DXGI_FORMAT_R32G32B32A32_UINT, 16, { 0x20, 0, 0, 0, 0x60, 0, 0, 0, 0xc0, 0, 0, 0, 0xff, 0, 0, 0 },
              0xffc06020u, false },
            { "unorm-rgba8", DXGI_FORMAT_R8G8B8A8_UNORM, 4, { 0x20, 0x60, 0xc0, 0xff }, 0xffc06020u, true },
        };
        ID3DBlob *bu4 = compile("vs_u4", "vs_4_0"), *bf4 = compile("vs_f4", "vs_4_0");
        ID3D11VertexShader *vu4 = nullptr, *vf4 = nullptr;
        if (bu4) dev->CreateVertexShader(bu4->GetBufferPointer(), bu4->GetBufferSize(), nullptr, &vu4);
        if (bf4) dev->CreateVertexShader(bf4->GetBufferPointer(), bf4->GetBufferSize(), nullptr, &vf4);
        for (auto &f : nf) {
            ID3DBlob *sig = f.flt ? bf4 : bu4;
            BYTE vdat[3 * 16];
            for (int v = 0; v < 3; v++)
                memcpy(vdat + v * f.stride, f.data, f.stride);
            D3D11_BUFFER_DESC nbd = {};
            nbd.ByteWidth = 3 * f.stride;
            nbd.Usage = D3D11_USAGE_DEFAULT;
            nbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
            D3D11_SUBRESOURCE_DATA nin = { vdat, 0, 0 };
            D3D11_INPUT_ELEMENT_DESC ne[2] = {
                { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
                { "COLOR", 0, f.fmt, 1, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 } };
            ID3D11Buffer *nb = nullptr;
            ID3D11InputLayout *nl = nullptr;
            ID3D11Texture2D *rt = texture(DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_RENDER_TARGET, nullptr, 0);
            ID3D11RenderTargetView *rtv = nullptr;
            if (!sig || !(f.flt ? vf4 : vu4) || !psc || FAILED(dev->CreateBuffer(&nbd, &nin, &nb)) ||
                FAILED(dev->CreateInputLayout(ne, 2, sig->GetBufferPointer(), sig->GetBufferSize(), &nl)) || !rt ||
                FAILED(dev->CreateRenderTargetView(rt, nullptr, &rtv))) {
                out("TEST %-9s: FAIL (setup)\n", f.name);
                failures++;
                continue;
            }
            const float grey[4] = { 0.5f, 0.5f, 0.5f, 1.0f };
            ctx->ClearRenderTargetView(rtv, grey);
            ctx->OMSetRenderTargets(1, &rtv, nullptr);
            D3D11_VIEWPORT vp = { 0.0f, 0.0f, 64.0f, 64.0f, 0.0f, 1.0f };
            ctx->RSSetViewports(1, &vp);
            ID3D11Buffer *bufs[2] = { vb, nb };
            UINT strides[2] = { 8, f.stride }, offs[2] = { 0, 0 };
            ctx->IASetVertexBuffers(0, 2, bufs, strides, offs);
            ctx->IASetInputLayout(nl);
            ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ctx->VSSetShader(f.flt ? vf4 : vu4, nullptr, 0);
            ctx->PSSetShader(psc, nullptr, 0);
            ctx->Draw(3, 0);
            if (readback(rt, DXGI_FORMAT_R8G8B8A8_UNORM, px))
                check(f.name, px, 16, f.want, 48, f.want, 0xffffffffu);
            else
                out("TEST %-9s: FAIL (readback)\n", f.name), failures++;
            rtv->Release(); rt->Release(); nl->Release(); nb->Release();
        }
        ID3D11Buffer *one[2] = { vb, nullptr };
        UINT st1[2] = { 8, 0 }, of1[2] = { 0, 0 };
        ctx->IASetVertexBuffers(0, 2, one, st1, of1);
    }

    out("sbtex %s: %d test(s) failed\n", dll, failures);
    if (logf)
        fclose(logf);
    return failures;
}
