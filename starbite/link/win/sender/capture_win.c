/* SPDX-License-Identifier: GPL-2.0-only
 * sblinkd capture on Windows: DXGI Desktop Duplication (IDXGIOutputDuplication) with the OS's dirty and
 * move rects, falling back to GDI BitBlt when duplication is not available.
 *
 * Output choice: the monitor called "Starbite Display" (the IddCx virtual monitor) when present, else
 * the primary; --output overrides. Changed areas only: each OS frame's dirty/move rects are copied out of
 * the desktop image (system memory images through MapDesktopSurface, GPU images through a staging
 * texture) into our own buffer, and handed to the encoder as its regions of interest. Without a viewer
 * or with 2 frames in flight the capture loop does not call sbl_cap_next at all: DXGI keeps
 * accumulating the dirty rects meanwhile, so the next frame covers everything.
 *
 * The alpha byte of the desktop image is undefined (DXGI_ALPHA_MODE_IGNORE): where DWM composed
 * translucent layers (the quick settings / Control Center flyout, Start, acrylic) it is not 0xFF, and a
 * viewer that composites the BGRA image shows those areas see-through. Every copied pixel gets alpha 0xFF
 * (same cost as the plain copy), which also keeps invisible alpha changes out of the tile compare.
 * A present that comes without metadata is copied whole (the tile compare still sends only what
 * changed) instead of being dropped. sbl_cap_get_stats() counts all of it; --diag logs every OS frame.
 *
 * Rotated outputs (Display Settings > orientation portrait / landscape flipped / portrait flipped): the
 * desktop is rotated, but DXGI hands over the image in the panel's own orientation (sw x sh; for 90/270
 * the desktop's w x h swapped) and its dirty/move rects in that image's coordinates. Each copied rect is
 * turned upright (sbl_rot_copy, pipeline.c) into our buffer, which always has the desktop's orientation,
 * and its rect is turned the same way (sbl_rot_rect) for the encoder. So the viewer gets an upright
 * frame of the desktop's size, and the pointer mapping (desktop coordinates) needs nothing extra. The
 * first frame after an open checks the texture's real size: one already upright is copied as is.
 * A rotation change is a mode change: DXGI reports ACCESS_LOST, the capture re-opens with the new
 * size, and session.c announces it (MODE APPLIED) and sends a keyframe.
 */
#define COBJMACROS
#include "sender.h"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct sbl_cap {
    sbl_cfg  cfg;
    int      gdi;
    WCHAR    dev[32];          /* \\.\DISPLAYn */
    RECT     desk;             /* desktop coordinates of the output */
    HMONITOR mon;
    int      starbite;
    uint32_t w, h;             /* the desktop's (upright) size: what the viewer gets */
    int      rot;              /* SBL_ROT_*: how the duplicated image is turned upright */
    uint32_t sw, sh;           /* the duplicated image's size (w x h swapped for 90/270) */
    int      tex_checked;      /* the first frame's texture size was compared with sw x sh */
    uint8_t *cur;              /* our copy of the desktop image, stride w*4 */
    size_t   stride;
    int      valid;

    /* DXGI */
    ID3D11Device *d3d;
    ID3D11DeviceContext *ctx;
    IDXGIOutputDuplication *dup;
    ID3D11Texture2D *staging;
    DXGI_OUTDUPL_DESC dd;
    uint8_t *meta;
    UINT     metacap;
    sbl_cap_stats st;

    /* GDI */
    HDC      screen, mem;
    HBITMAP  bmp, oldbmp;
    uint8_t *bits;
    uint64_t last_gdi;
};

/* ---- output selection ------------------------------------------------------------------------------ */

/* set after a failed open: the retries (every 2-10 s, e.g. all night on the lock screen) do not log the
 * same output list and error again each time; cleared by the next successful open */
static int g_quiet;

static void to_utf8(const WCHAR *w, char *out, size_t cap)
{
    if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)cap, NULL, NULL)) out[0] = 0;
    out[cap - 1] = 0;
}

static int has_starbite(const char *s)
{
    char low[160];
    size_t i;
    for (i = 0; s[i] && i < sizeof low - 1; i++) low[i] = (char)(s[i] >= 'A' && s[i] <= 'Z' ? s[i] + 32 : s[i]);
    low[i] = 0;
    return strstr(low, "starbite") != NULL;
}

/* friendly monitor name (EDID name, as Display Settings shows it) of a GDI device, plus the adapter's
 * and monitor's device strings; any of them containing "Starbite" marks the Starbite Display */
static void monitor_names(const WCHAR *gdi, char *name, size_t cap, int *starbite)
{
    name[0] = 0;
    *starbite = 0;
    UINT32 np = 0, nm = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) == ERROR_SUCCESS && np) {
        DISPLAYCONFIG_PATH_INFO *p = calloc(np, sizeof *p);
        DISPLAYCONFIG_MODE_INFO *m = calloc(nm ? nm : 1, sizeof *m);
        if (p && m && QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, p, &nm, m, NULL) == ERROR_SUCCESS) {
            for (UINT32 i = 0; i < np; i++) {
                DISPLAYCONFIG_SOURCE_DEVICE_NAME sn;
                memset(&sn, 0, sizeof sn);
                sn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
                sn.header.size = sizeof sn;
                sn.header.adapterId = p[i].sourceInfo.adapterId;
                sn.header.id = p[i].sourceInfo.id;
                if (DisplayConfigGetDeviceInfo(&sn.header) != ERROR_SUCCESS || wcscmp(sn.viewGdiDeviceName, gdi)) continue;
                DISPLAYCONFIG_TARGET_DEVICE_NAME tn;
                memset(&tn, 0, sizeof tn);
                tn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
                tn.header.size = sizeof tn;
                tn.header.adapterId = p[i].targetInfo.adapterId;
                tn.header.id = p[i].targetInfo.id;
                if (DisplayConfigGetDeviceInfo(&tn.header) == ERROR_SUCCESS) {
                    to_utf8(tn.monitorFriendlyDeviceName, name, cap);
                    if (has_starbite(name)) *starbite = 1;
                }
                break;
            }
        }
        free(p);
        free(m);
    }
    char tmp[160];
    DISPLAY_DEVICEW dd;
    memset(&dd, 0, sizeof dd);
    dd.cb = sizeof dd;
    if (EnumDisplayDevicesW(gdi, 0, &dd, 0)) {          /* the monitor */
        to_utf8(dd.DeviceString, tmp, sizeof tmp);
        if (has_starbite(tmp)) *starbite = 1;
        if (!name[0]) snprintf(name, cap, "%s", tmp);
    }
    for (DWORD i = 0;; i++) {                             /* the adapter */
        memset(&dd, 0, sizeof dd);
        dd.cb = sizeof dd;
        if (!EnumDisplayDevicesW(NULL, i, &dd, 0)) break;
        if (wcscmp(dd.DeviceName, gdi)) continue;
        to_utf8(dd.DeviceString, tmp, sizeof tmp);
        if (has_starbite(tmp)) *starbite = 1;
        break;
    }
}

typedef struct pick {
    IDXGIAdapter1 *ad;
    IDXGIOutput *out;
    DXGI_OUTPUT_DESC d;
    char name[96];
    int starbite, primary, score;
} pick;

static int score_output(const sbl_cfg *cfg, const pick *p)
{
    const char *o = cfg->output;
    if (!o[0]) return p->starbite ? 3 : p->primary ? 2 : 1;
    if (!_stricmp(o, "primary")) return p->primary ? 3 : 0;
    char dev[64];
    to_utf8(p->d.DeviceName, dev, sizeof dev);
    if (!_stricmp(o, dev)) return 3;
    return strstr(p->name, o) ? 3 : 0;
}

/* finds the output to capture; on success *best holds referenced adapter + output */
static int find_output(const sbl_cfg *cfg, pick *best)
{
    IDXGIFactory1 *f = NULL;
    HRESULT hr = CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&f);
    if (FAILED(hr)) { sbl_log("capture: CreateDXGIFactory1 failed 0x%08lx", (unsigned long)hr); return -1; }
    memset(best, 0, sizeof *best);
    IDXGIAdapter1 *ad;
    for (UINT a = 0; IDXGIFactory1_EnumAdapters1(f, a, &ad) != DXGI_ERROR_NOT_FOUND; a++) {
        DXGI_ADAPTER_DESC1 adesc;
        IDXGIAdapter1_GetDesc1(ad, &adesc);
        IDXGIOutput *out;
        for (UINT o = 0; IDXGIAdapter1_EnumOutputs(ad, o, &out) != DXGI_ERROR_NOT_FOUND; o++) {
            pick p;
            memset(&p, 0, sizeof p);
            IDXGIOutput_GetDesc(out, &p.d);
            MONITORINFO mi;
            mi.cbSize = sizeof mi;
            p.primary = GetMonitorInfoW(p.d.Monitor, &mi) && (mi.dwFlags & MONITORINFOF_PRIMARY);
            monitor_names(p.d.DeviceName, p.name, sizeof p.name, &p.starbite);
            p.score = p.d.AttachedToDesktop ? score_output(cfg, &p) : 0;
            char an[128], dn[64];
            to_utf8(adesc.Description, an, sizeof an);
            to_utf8(p.d.DeviceName, dn, sizeof dn);
            if (!g_quiet)
                sbl_log("capture: output %s \"%s\" on \"%s\" (%ld,%ld)-(%ld,%ld)%s%s%s", dn, p.name, an,
                        p.d.DesktopCoordinates.left, p.d.DesktopCoordinates.top, p.d.DesktopCoordinates.right,
                        p.d.DesktopCoordinates.bottom, p.primary ? " primary" : "", p.starbite ? " STARBITE" : "",
                        p.d.AttachedToDesktop ? "" : " (not attached)");
            if (p.score > best->score) {
                if (best->out) { IDXGIOutput_Release(best->out); IDXGIAdapter1_Release(best->ad); }
                p.out = out;
                p.ad = ad;
                IDXGIAdapter1_AddRef(ad);
                *best = p;
            } else IDXGIOutput_Release(out);
        }
        IDXGIAdapter1_Release(ad);
    }
    IDXGIFactory1_Release(f);
    if (!best->out) { if (!g_quiet) sbl_log("capture: no output matches \"%s\"", cfg->output); return -1; }
    return 0;
}

/* ---- DXGI duplication --------------------------------------------------------------------------- */

/* DXGI_MODE_ROTATION_ROTATE90 = the desktop is shown turned 90 degrees clockwise relative to the panel
 * (the DesktopDuplication sample's reading; sender.h has the pixel mapping). If 90/270 come out upside
 * down on the phone, swap the SBL_ROT_90 and SBL_ROT_270 here: nothing else depends on the direction. */
static int sbl_rot_from_dxgi(DXGI_MODE_ROTATION r)
{
    switch (r) {
    case DXGI_MODE_ROTATION_ROTATE90:  return SBL_ROT_90;
    case DXGI_MODE_ROTATION_ROTATE180: return SBL_ROT_180;
    case DXGI_MODE_ROTATION_ROTATE270: return SBL_ROT_270;
    default:                           return SBL_ROT_0;
    }
}

static int dxgi_open(sbl_cap *c, pick *p)
{
    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDevice((IDXGIAdapter *)p->ad, D3D_DRIVER_TYPE_UNKNOWN, NULL, 0, NULL, 0,
                                   D3D11_SDK_VERSION, &c->d3d, &fl, &c->ctx);
    if (FAILED(hr)) { sbl_log("capture: D3D11CreateDevice failed 0x%08lx", (unsigned long)hr); return -1; }
    IDXGIOutput1 *o1 = NULL;
    hr = IDXGIOutput_QueryInterface(p->out, &IID_IDXGIOutput1, (void **)&o1);
    if (FAILED(hr)) { sbl_log("capture: no IDXGIOutput1 0x%08lx", (unsigned long)hr); return -1; }
    hr = IDXGIOutput1_DuplicateOutput(o1, (IUnknown *)c->d3d, &c->dup);
    IDXGIOutput1_Release(o1);
    if (FAILED(hr)) {
        if (!g_quiet)
            sbl_log("capture: DuplicateOutput failed 0x%08lx%s", (unsigned long)hr,
                    hr == E_ACCESSDENIED ? " (secure desktop / lock screen?)" :
                    hr == DXGI_ERROR_UNSUPPORTED ? " (unsupported on this adapter)" :
                    hr == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE ? " (too many duplications)" : "");
        return hr == E_ACCESSDENIED ? -2 : -1;
    }
    IDXGIOutputDuplication_GetDesc(c->dup, &c->dd);
    c->rot = sbl_rot_from_dxgi(c->dd.Rotation);
    /* the upright size: the output's desktop rect (always in the desktop's orientation, physical pixels:
     * the thread is per-monitor DPI aware) when it is ModeDesc either way round, else ModeDesc as it is
     * (landscape for a 90/270 output on Windows 11) */
    uint32_t mw = c->dd.ModeDesc.Width, mh = c->dd.ModeDesc.Height;
    uint32_t kw = (uint32_t)(c->desk.right - c->desk.left), kh = (uint32_t)(c->desk.bottom - c->desk.top);
    if (kw && kh && ((kw == mw && kh == mh) || (kw == mh && kh == mw))) { c->w = kw; c->h = kh; }
    else { c->w = mw; c->h = mh; }
    sbl_rot_size(c->rot, c->w, c->h, &c->sw, &c->sh);   /* a quarter turn is its own size swap */
    if (c->rot)
        sbl_log("capture: output rotated %d degrees (DXGI rotation %d): duplicated image %ux%u expected, sent "
                "upright as %ux%u (ModeDesc %ux%u, desktop rect %ux%u)", c->rot * 90, (int)c->dd.Rotation,
                c->sw, c->sh, c->w, c->h, mw, mh, kw, kh);
    return 0;
}

/* copies a rect and makes it opaque (alpha 0xFF): a plain loop the compiler vectorizes (NEON) */
static void copy_rows(uint8_t *dst, size_t dstride, const uint8_t *src, size_t sstride, const RECT *r)
{
    size_t n = (size_t)(r->right - r->left);
    for (LONG y = r->top; y < r->bottom; y++) {
        uint32_t *d = (uint32_t *)(dst + (size_t)y * dstride + (size_t)r->left * 4);
        const uint32_t *sp = (const uint32_t *)(src + (size_t)y * sstride + (size_t)r->left * 4);
        for (size_t i = 0; i < n; i++) d[i] = sp[i] | 0xFF000000u;
    }
}

/* clips a rect of the duplicated image (sw x sh; = w x h unless rotated 90/270) */
static int clip_rect(const sbl_cap *c, RECT *r)
{
    if (r->left < 0) r->left = 0;
    if (r->top < 0) r->top = 0;
    if (r->right > (LONG)c->sw) r->right = (LONG)c->sw;
    if (r->bottom > (LONG)c->sh) r->bottom = (LONG)c->sh;
    return r->left < r->right && r->top < r->bottom;
}

/* a clipped rect of the duplicated image into our upright copy */
static void put_rect(sbl_cap *c, const uint8_t *src, size_t pitch, const RECT *r)
{
    if (!c->rot) { copy_rows(c->cur, c->stride, src, pitch, r); return; }
    sbl_irect s = { r->left, r->top, r->right, r->bottom };
    sbl_rot_copy(c->rot, c->cur, c->stride, src, pitch, c->sw, c->sh, s);
}

/* First frame after an open: is the duplicated image the size the rotation implies? Windows has been
 * seen on the S9 to hand a 90/270 output's image over in the panel orientation (the case handled); one that is
 * already upright is copied as is, and any other size is clipped to what both agree on. */
static void check_texture(sbl_cap *c, IDXGIResource *res)
{
    c->tex_checked = 1;
    ID3D11Texture2D *t = NULL;
    if (!res || FAILED(IDXGIResource_QueryInterface(res, &IID_ID3D11Texture2D, (void **)&t))) return;
    D3D11_TEXTURE2D_DESC td;
    ID3D11Texture2D_GetDesc(t, &td);
    ID3D11Texture2D_Release(t);
    if (td.Width == c->sw && td.Height == c->sh) {
        if (c->rot) sbl_log("capture: duplicated image %ux%u as expected: turning it upright", td.Width, td.Height);
        return;
    }
    if (c->rot && td.Width == c->w && td.Height == c->h) {
        sbl_log("capture: duplicated image %ux%u is already upright: copied without turning", td.Width, td.Height);
        c->rot = SBL_ROT_0;
        c->sw = c->w;
        c->sh = c->h;
        return;
    }
    sbl_log("capture: duplicated image %ux%u, expected %ux%u (rotation %d): clipping to both",
            td.Width, td.Height, c->sw, c->sh, c->rot * 90);
    /* only ever shrink: reads stay inside the texture, sbl_rot_rect's output inside w x h */
    if (td.Width < c->sw) c->sw = td.Width;
    if (td.Height < c->sh) c->sh = td.Height;
}

static int dxgi_next(sbl_cap *c, int timeout_ms, sbl_region *dirty, uint64_t *capture_us)
{
    DXGI_OUTDUPL_FRAME_INFO fi;
    IDXGIResource *res = NULL;
    HRESULT hr = IDXGIOutputDuplication_AcquireNextFrame(c->dup, (UINT)timeout_ms, &fi, &res);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) return 0;
    if (hr == DXGI_ERROR_ACCESS_LOST || hr == DXGI_ERROR_INVALID_CALL) return SBL_CAP_RESET;
    if (FAILED(hr)) { sbl_log("capture: AcquireNextFrame 0x%08lx", (unsigned long)hr); return SBL_CAP_RESET; }
    *capture_us = plat_now_us();
    if (!c->tex_checked) check_texture(c, res);

    /* metadata: move rects then dirty rects, all in desktop-image coordinates (the duplicated image's:
     * the panel's orientation on a rotated output; --diag's bbox is in those too) */
    RECT *rects = NULL;
    UINT nrects = 0, nmove = 0;
    int full = !c->valid;
    c->st.os_frames++;
    c->st.accumulated += fi.AccumulatedFrames;
    if (!full && fi.LastPresentTime.QuadPart != 0 && fi.TotalMetadataBufferSize == 0) {
        /* a new image without a word about what changed: copy it all rather than miss it */
        c->st.no_meta++;
        full = 1;
    }
    if (!full && fi.TotalMetadataBufferSize) {
        if (fi.TotalMetadataBufferSize > c->metacap) {
            free(c->meta);
            c->metacap = fi.TotalMetadataBufferSize + 4096;
            c->meta = malloc(c->metacap);
            if (!c->meta) c->metacap = 0;
        }
        UINT used = 0, need = 0;
        if (c->meta && SUCCEEDED(IDXGIOutputDuplication_GetFrameMoveRects(c->dup, c->metacap, (DXGI_OUTDUPL_MOVE_RECT *)c->meta, &used))) {
            /* the destination of a move changed; the desktop image already shows the result */
            nmove = used / sizeof(DXGI_OUTDUPL_MOVE_RECT);
            DXGI_OUTDUPL_MOVE_RECT *mv = (DXGI_OUTDUPL_MOVE_RECT *)c->meta;
            RECT *dst = (RECT *)c->meta;   /* compact in place: a RECT is smaller than a MOVE_RECT */
            for (UINT i = 0; i < nmove; i++) { RECT t = mv[i].DestinationRect; dst[i] = t; }
            nrects = nmove;
            used = nmove * sizeof(RECT);
            if (SUCCEEDED(IDXGIOutputDuplication_GetFrameDirtyRects(c->dup, c->metacap - used, (RECT *)(c->meta + used), &need)))
                nrects += need / sizeof(RECT);
            else full = 1;
            rects = (RECT *)c->meta;
        } else full = 1;
    }
    if (c->cfg.diag) {
        RECT bb = { 0, 0, 0, 0 };
        for (UINT i = 0; !full && i < nrects; i++) {
            if (i == 0) bb = rects[0];
            else UnionRect(&bb, &bb, &rects[i]);
        }
        sbl_log("diag: os frame acc %u present %d ptr %d meta %u: %u move + %u dirty rects, bbox (%ld,%ld)-(%ld,%ld)%s",
                fi.AccumulatedFrames, fi.LastPresentTime.QuadPart != 0, fi.LastMouseUpdateTime.QuadPart != 0,
                fi.TotalMetadataBufferSize, nmove, nrects - nmove, bb.left, bb.top, bb.right, bb.bottom,
                full ? " FULL" : "");
    }
    if (!full && fi.LastPresentTime.QuadPart == 0 && nrects == 0) {   /* only the pointer moved */
        c->st.ptr_only++;
        if (res) IDXGIResource_Release(res);
        IDXGIOutputDuplication_ReleaseFrame(c->dup);
        return 0;
    }
    RECT all = { 0, 0, (LONG)c->sw, (LONG)c->sh };
    if (full) { rects = &all; nrects = 1; nmove = 0; c->st.full++; }
    c->st.move_rects += nmove;
    c->st.dirty_rects += nrects - nmove;

    int ret = 1;
    if (c->dd.DesktopImageInSystemMemory) {
        DXGI_MAPPED_RECT mr;
        hr = IDXGIOutputDuplication_MapDesktopSurface(c->dup, &mr);
        if (SUCCEEDED(hr)) {
            for (UINT i = 0; i < nrects; i++) {
                RECT r = rects[i];
                if (clip_rect(c, &r)) put_rect(c, mr.pBits, (size_t)mr.Pitch, &r);
            }
            IDXGIOutputDuplication_UnMapDesktopSurface(c->dup);
        } else { sbl_log("capture: MapDesktopSurface 0x%08lx", (unsigned long)hr); ret = SBL_CAP_RESET; }
    } else {
        ID3D11Texture2D *tex = NULL;
        hr = IDXGIResource_QueryInterface(res, &IID_ID3D11Texture2D, (void **)&tex);
        if (SUCCEEDED(hr) && !c->staging) {
            D3D11_TEXTURE2D_DESC td;
            ID3D11Texture2D_GetDesc(tex, &td);
            if (td.Format != DXGI_FORMAT_B8G8R8A8_UNORM)
                sbl_log("capture: desktop format %d is not BGRA8 (colours will be wrong)", (int)td.Format);
            td.Usage = D3D11_USAGE_STAGING;
            td.BindFlags = 0;
            td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            td.MiscFlags = 0;
            td.MipLevels = 1;
            td.ArraySize = 1;
            hr = ID3D11Device_CreateTexture2D(c->d3d, &td, NULL, &c->staging);
        }
        if (SUCCEEDED(hr)) {
            for (UINT i = 0; i < nrects; i++) {
                RECT r = rects[i];
                if (!clip_rect(c, &r)) continue;
                D3D11_BOX b = { (UINT)r.left, (UINT)r.top, 0, (UINT)r.right, (UINT)r.bottom, 1 };
                ID3D11DeviceContext_CopySubresourceRegion(c->ctx, (ID3D11Resource *)c->staging, 0, (UINT)r.left,
                                                          (UINT)r.top, 0, (ID3D11Resource *)tex, 0, &b);
            }
            D3D11_MAPPED_SUBRESOURCE ms;
            hr = ID3D11DeviceContext_Map(c->ctx, (ID3D11Resource *)c->staging, 0, D3D11_MAP_READ, 0, &ms);
            if (SUCCEEDED(hr)) {
                for (UINT i = 0; i < nrects; i++) {
                    RECT r = rects[i];
                    if (clip_rect(c, &r)) put_rect(c, ms.pData, ms.RowPitch, &r);
                }
                ID3D11DeviceContext_Unmap(c->ctx, (ID3D11Resource *)c->staging, 0);
            }
        }
        if (FAILED(hr)) { sbl_log("capture: staging copy 0x%08lx", (unsigned long)hr); ret = SBL_CAP_RESET; }
        if (tex) ID3D11Texture2D_Release(tex);
    }
    if (res) IDXGIResource_Release(res);
    IDXGIOutputDuplication_ReleaseFrame(c->dup);
    if (ret < 0) return ret;

    for (UINT i = 0; i < nrects; i++) {
        RECT r = rects[i];
        if (!clip_rect(c, &r)) continue;
        sbl_irect u = { r.left, r.top, r.right, r.bottom };
        u = sbl_rot_rect(c->rot, c->sw, c->sh, u);   /* identity when not rotated */
        sbl_region_add(dirty, u.l, u.t, u.r - u.l, u.b - u.t, c->w, c->h);
    }
    c->valid = 1;
    return 1;
}

/* ---- GDI fallback -------------------------------------------------------------------------------- */

static int gdi_open(sbl_cap *c)
{
    c->w = (uint32_t)(c->desk.right - c->desk.left);
    c->h = (uint32_t)(c->desk.bottom - c->desk.top);
    c->screen = GetDC(NULL);
    c->mem = CreateCompatibleDC(c->screen);
    BITMAPINFO bi;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = (LONG)c->w;
    bi.bmiHeader.biHeight = -(LONG)c->h;   /* top-down */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void *bits = NULL;
    c->bmp = CreateDIBSection(c->mem, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!c->screen || !c->mem || !c->bmp) { sbl_log("capture: GDI setup failed (%lu)", GetLastError()); return -1; }
    c->bits = bits;
    c->oldbmp = SelectObject(c->mem, c->bmp);
    return 0;
}

static int gdi_next(sbl_cap *c, int timeout_ms, sbl_region *dirty, uint64_t *capture_us)
{
    uint64_t period = 1000000u / (uint64_t)(c->cfg.gdi_fps > 0 ? c->cfg.gdi_fps : 10);
    uint64_t now = plat_now_us();
    if (timeout_ms > 0 && c->valid && now - c->last_gdi < period) {   /* timeout 0 = capture now */
        uint64_t left = (period - (now - c->last_gdi)) / 1000u;
        if (left > (uint64_t)timeout_ms) { plat_sleep_ms(timeout_ms); return 0; }
        plat_sleep_ms((int)left);
    }
    MONITORINFO mi;
    mi.cbSize = sizeof mi;
    if (!GetMonitorInfoW(c->mon, &mi) || !EqualRect(&mi.rcMonitor, &c->desk)) return SBL_CAP_RESET;
    c->last_gdi = plat_now_us();
    if (!BitBlt(c->mem, 0, 0, (int)c->w, (int)c->h, c->screen, c->desk.left, c->desk.top, SRCCOPY)) {
        sbl_log("capture: BitBlt failed (%lu)", GetLastError());
        return SBL_CAP_RESET;
    }
    GdiFlush();
    /* BitBlt leaves alpha 0: opaque, like the DXGI path */
    uint32_t *px = (uint32_t *)c->bits;
    for (size_t i = 0, np = (size_t)c->w * c->h; i < np; i++) px[i] |= 0xFF000000u;
    c->st.os_frames++;
    c->st.full++;
    /* no OS dirty rects with GDI: the encoder compares the whole frame (tiles) */
    sbl_region_full(dirty, c->w, c->h);
    *capture_us = c->last_gdi;
    c->valid = 1;
    return 1;
}

/* ---- interface ------------------------------------------------------------------------------------ */

static int cap_open(sbl_cap **cp, const sbl_cfg *cfg, sbl_cap_info *info);

int sbl_cap_open(sbl_cap **cp, const sbl_cfg *cfg, sbl_cap_info *info)
{
    int r = cap_open(cp, cfg, info);
    g_quiet = r < 0;
    return r;
}

static int cap_open(sbl_cap **cp, const sbl_cfg *cfg, sbl_cap_info *info)
{
    pick p;
    /* duplicate the desktop that has the input now (Winlogon for UAC / lock screen when the session
     * helper runs as SYSTEM); DXGI reports ACCESS_LOST when it switches, which re-opens here */
    sbl_follow_input_desktop(1);
    if (find_output(cfg, &p) < 0) return -1;
    sbl_cap *c = calloc(1, sizeof *c);
    if (!c) { IDXGIOutput_Release(p.out); IDXGIAdapter1_Release(p.ad); return -1; }
    c->cfg = *cfg;
    wcsncpy(c->dev, p.d.DeviceName, 31);
    c->desk = p.d.DesktopCoordinates;
    c->mon = p.d.Monitor;
    c->starbite = p.starbite;

    int r = -1;
    if (!cfg->force_gdi) {
        r = dxgi_open(c, &p);
        if (r == -2) {   /* secure desktop / session starting: try again soon (session.c) rather than GDI */
            IDXGIOutput_Release(p.out); IDXGIAdapter1_Release(p.ad);
            sbl_cap_close(c);
            return -2;
        }
        if (r < 0) {
            if (c->dup) IDXGIOutputDuplication_Release(c->dup);
            if (c->ctx) ID3D11DeviceContext_Release(c->ctx);
            if (c->d3d) ID3D11Device_Release(c->d3d);
            c->dup = NULL; c->ctx = NULL; c->d3d = NULL;
            sbl_log("capture: falling back to GDI");
        }
    }
    if (r < 0) {
        c->gdi = 1;
        r = gdi_open(c);
    }
    IDXGIOutput_Release(p.out);
    IDXGIAdapter1_Release(p.ad);
    if (r < 0 || c->w == 0 || c->h == 0 || c->w > 0xFFFF || c->h > 0xFFFF) { sbl_cap_close(c); return -1; }
    c->stride = (size_t)c->w * 4;
    c->cur = c->gdi ? c->bits : calloc(c->stride, c->h);
    if (!c->cur) { sbl_cap_close(c); return -1; }

    memset(info, 0, sizeof *info);
    info->width = c->w;
    info->height = c->h;
    if (!c->gdi && c->dd.ModeDesc.RefreshRate.Denominator)
        info->refresh_mhz = (uint32_t)((uint64_t)c->dd.ModeDesc.RefreshRate.Numerator * 1000u /
                                       c->dd.ModeDesc.RefreshRate.Denominator);
    info->is_starbite = c->starbite;
    info->rotation = c->gdi ? SBL_ROT_0 : c->rot;   /* GDI's BitBlt is already upright */
    info->can_mode = c->starbite;   /* never resize the phone's own panel */
    info->geom.out_x = c->desk.left;
    info->geom.out_y = c->desk.top;
    info->geom.out_w = c->desk.right - c->desk.left;
    info->geom.out_h = c->desk.bottom - c->desk.top;
    info->geom.disp_w = c->w;
    info->geom.disp_h = c->h;
    snprintf(info->name, sizeof info->name, "%s", p.name);
    info->method = c->gdi ? "gdi" : c->dd.DesktopImageInSystemMemory ? "dxgi (system memory)" : "dxgi";
    *cp = c;
    return 0;
}

void sbl_cap_close(sbl_cap *c)
{
    if (!c) return;
    if (c->staging) ID3D11Texture2D_Release(c->staging);
    if (c->dup) IDXGIOutputDuplication_Release(c->dup);
    if (c->ctx) ID3D11DeviceContext_Release(c->ctx);
    if (c->d3d) ID3D11Device_Release(c->d3d);
    if (c->mem) { if (c->oldbmp) SelectObject(c->mem, c->oldbmp); DeleteDC(c->mem); }
    if (c->bmp) DeleteObject(c->bmp);
    if (c->screen) ReleaseDC(NULL, c->screen);
    if (!c->gdi) free(c->cur);
    free(c->meta);
    free(c);
}

int sbl_cap_next(sbl_cap *c, int timeout_ms, sbl_region *dirty, uint64_t *capture_us)
{
    return c->gdi ? gdi_next(c, timeout_ms, dirty, capture_us) : dxgi_next(c, timeout_ms, dirty, capture_us);
}

int sbl_cap_valid(sbl_cap *c) { return c && c->valid; }

int sbl_cap_map(sbl_cap *c, const uint8_t **px, size_t *stride)
{
    if (!c->valid) return -1;
    *px = c->cur;
    *stride = c->stride;
    return 0;
}

void sbl_cap_unmap(sbl_cap *c) { (void)c; }

void sbl_cap_get_stats(sbl_cap *c, sbl_cap_stats *st)
{
    if (c) *st = c->st;
    else memset(st, 0, sizeof *st);
}

int sbl_cap_set_mode(sbl_cap *c, uint32_t w, uint32_t h, uint32_t refresh_mhz)
{
    if (!c->starbite) return -1;
    DEVMODEW best, dm;
    int have = 0;
    uint64_t bestd = ~0ull;
    uint32_t want_hz = refresh_mhz ? (refresh_mhz + 500) / 1000 : 60;
    for (DWORD i = 0;; i++) {
        memset(&dm, 0, sizeof dm);
        dm.dmSize = sizeof dm;
        if (!EnumDisplaySettingsW(c->dev, i, &dm)) break;
        if (dm.dmBitsPerPel != 32) continue;
        int64_t dw = (int64_t)dm.dmPelsWidth - w, dh = (int64_t)dm.dmPelsHeight - h;
        int64_t dr = (int64_t)dm.dmDisplayFrequency - want_hz;
        uint64_t d = (uint64_t)((dw < 0 ? -dw : dw) + (dh < 0 ? -dh : dh)) * 1000u + (uint64_t)(dr < 0 ? -dr : dr);
        if (d < bestd) { bestd = d; best = dm; have = 1; }
    }
    if (!have) return -1;
    if (best.dmPelsWidth == c->w && best.dmPelsHeight == c->h) return 0;
    best.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL | DM_DISPLAYFREQUENCY;
    LONG r = ChangeDisplaySettingsExW(c->dev, &best, NULL, 0, NULL);   /* dynamic, not saved */
    sbl_log("capture: ChangeDisplaySettingsEx %lux%lu @ %lu Hz: %ld", best.dmPelsWidth, best.dmPelsHeight,
            best.dmDisplayFrequency, r);
    return r == DISP_CHANGE_SUCCESSFUL ? 1 : -1;
}
