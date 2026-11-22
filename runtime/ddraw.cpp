#include "display.h"
#include "win32.h"

#include <cstdio>
#include <map>
#include <vector>

namespace {

constexpr uint32_t DD_OK = 0;
constexpr uint32_t E_NOINTERFACE = 0x80004002u;
constexpr uint32_t DDERR_INVALIDPARAMS = 0x80070057u;
constexpr uint32_t DDERR_NOTFOUND = 0x887600ffu;

constexpr uint32_t DDSD_CAPS = 0x1;
constexpr uint32_t DDSD_HEIGHT = 0x2;
constexpr uint32_t DDSD_WIDTH = 0x4;
constexpr uint32_t DDSD_PITCH = 0x8;
constexpr uint32_t DDSD_BACKBUFFERCOUNT = 0x20;
constexpr uint32_t DDSD_LPSURFACE = 0x800;
constexpr uint32_t DDSD_PIXELFORMAT = 0x1000;

constexpr uint32_t DDSCAPS_BACKBUFFER = 0x4;
constexpr uint32_t DDSCAPS_COMPLEX = 0x8;
constexpr uint32_t DDSCAPS_FLIP = 0x10;
constexpr uint32_t DDSCAPS_FRONTBUFFER = 0x20;
constexpr uint32_t DDSCAPS_PRIMARYSURFACE = 0x200;

constexpr uint32_t DDBLT_COLORFILL = 0x400;
constexpr uint32_t DDBLT_KEYSRC = 0x8000;
constexpr uint32_t DDBLTFAST_SRCCOLORKEY = 0x1;

struct Surface {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t bpp = 32;
    uint32_t pitch = 0;
    uint32_t bits = 0;
    uint32_t caps = 0;
    uint32_t back = 0;
    uint32_t palette = 0;
    uint32_t colorKey = 0;
    bool hasColorKey = false;
    uint32_t refs = 1;
};

struct Palette {
    uint8_t entries[256][4] = {};
};

std::map<uint32_t, Surface> g_surfaces;
std::map<uint32_t, Palette> g_palettes;
uint32_t g_modeWidth = 640;
uint32_t g_modeHeight = 480;
uint32_t g_modeBpp = 32;
uint32_t g_primary = 0;

std::vector<uint32_t> g_convert;

uint32_t makeVtable(const char* prefix, std::initializer_list<const char*> methods) {
    uint32_t vt = guest_alloc(4 * uint32_t(methods.size()), 4);
    uint32_t i = 0;
    for (const char* name : methods) {
        wr32(g_mem, vt + 4 * i++, hle_thunk("ddraw", std::string(prefix) + "_" + name));
    }
    return vt;
}

uint32_t ddrawVtable() {
    static uint32_t vt = makeVtable("DD", {"QueryInterface", "AddRef", "Release", "Compact", "CreateClipper", "CreatePalette",
        "CreateSurface", "DuplicateSurface", "EnumDisplayModes", "EnumSurfaces", "FlipToGDISurface", "GetCaps", "GetDisplayMode",
        "GetFourCCCodes", "GetGDISurface", "GetMonitorFrequency", "GetScanLine", "GetVerticalBlankStatus", "Initialize",
        "RestoreDisplayMode", "SetCooperativeLevel", "SetDisplayMode", "WaitForVerticalBlank"});
    return vt;
}

uint32_t surfaceVtable() {
    static uint32_t vt = makeVtable("DDS", {"QueryInterface", "AddRef", "Release", "AddAttachedSurface", "AddOverlayDirtyRect",
        "Blt", "BltBatch", "BltFast", "DeleteAttachedSurface", "EnumAttachedSurfaces", "EnumOverlayZOrders", "Flip",
        "GetAttachedSurface", "GetBltStatus", "GetCaps", "GetClipper", "GetColorKey", "GetDC", "GetFlipStatus",
        "GetOverlayPosition", "GetPalette", "GetPixelFormat", "GetSurfaceDesc", "Initialize", "IsLost", "Lock", "ReleaseDC",
        "Restore", "SetClipper", "SetColorKey", "SetOverlayPosition", "SetPalette", "Unlock", "UpdateOverlay",
        "UpdateOverlayDisplay", "UpdateOverlayZOrder"});
    return vt;
}

uint32_t paletteVtable() {
    static uint32_t vt = makeVtable("DDP", {"QueryInterface", "AddRef", "Release", "GetCaps", "GetEntries", "Initialize", "SetEntries"});
    return vt;
}

uint32_t clipperVtable() {
    static uint32_t vt = makeVtable("DDC", {"QueryInterface", "AddRef", "Release", "GetClipList", "GetHWnd", "Initialize",
        "IsClipListChanged", "SetClipList", "SetHWnd"});
    return vt;
}

uint32_t newObject(uint32_t vtable) {
    uint32_t obj = guest_alloc(16, 4);
    wr32(g_mem, obj, vtable);
    return obj;
}

std::string guidString(uint32_t p) {
    char b[64];
    std::snprintf(b, sizeof b, "{%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}", rd32(g_mem, p), rd16(g_mem, p + 4), rd16(g_mem, p + 6),
                  g_mem[p + 8], g_mem[p + 9], g_mem[p + 10], g_mem[p + 11], g_mem[p + 12], g_mem[p + 13], g_mem[p + 14], g_mem[p + 15]);
    return b;
}

void writePixelFormat(uint32_t p, uint32_t bpp) {
    std::memset(g_mem + p, 0, 32);
    wr32(g_mem, p, 32);
    wr32(g_mem, p + 4, bpp == 8 ? 0x60 : 0x40);
    wr32(g_mem, p + 12, bpp);
    if (bpp == 16) {
        wr32(g_mem, p + 16, 0xf800);
        wr32(g_mem, p + 20, 0x07e0);
        wr32(g_mem, p + 24, 0x001f);
    } else if (bpp == 32 || bpp == 24) {
        wr32(g_mem, p + 16, 0xff0000);
        wr32(g_mem, p + 20, 0x00ff00);
        wr32(g_mem, p + 24, 0x0000ff);
    }
}

void writeSurfaceDesc(uint32_t p, const Surface& s, uint32_t bits) {
    uint32_t size = rd32(g_mem, p);
    if (size != 108 && size != 124) {
        size = 108;
    }
    std::memset(g_mem + p, 0, size);
    wr32(g_mem, p, size);
    wr32(g_mem, p + 4, DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH | DDSD_PIXELFORMAT | (bits ? DDSD_LPSURFACE : 0));
    wr32(g_mem, p + 8, s.height);
    wr32(g_mem, p + 12, s.width);
    wr32(g_mem, p + 16, s.pitch);
    wr32(g_mem, p + 36, bits);
    writePixelFormat(p + 72, s.bpp);
    wr32(g_mem, p + 104, s.caps);
}

uint32_t createSurface(uint32_t width, uint32_t height, uint32_t bpp, uint32_t caps) {
    Surface s;
    s.width = width;
    s.height = height;
    s.bpp = bpp;
    s.pitch = (width * (bpp / 8) + 3) & ~3u;
    s.caps = caps;
    s.bits = guest_alloc(s.pitch * height, 16);
    uint32_t obj = newObject(surfaceVtable());
    g_surfaces[obj] = s;
    return obj;
}

Surface* surface(uint32_t obj) {
    auto it = g_surfaces.find(obj);
    return it == g_surfaces.end() ? nullptr : &it->second;
}

uint32_t pixelAt(const Surface& s, uint32_t x, uint32_t y) {
    uint32_t off = s.bits + y * s.pitch;
    switch (s.bpp) {
    case 8: return g_mem[off + x];
    case 16: return rd16(g_mem, off + 2 * x);
    default: return rd32(g_mem, off + 4 * x);
    }
}

void setPixel(const Surface& s, uint32_t x, uint32_t y, uint32_t v) {
    uint32_t off = s.bits + y * s.pitch;
    switch (s.bpp) {
    case 8: g_mem[off + x] = uint8_t(v); break;
    case 16: wr16(g_mem, off + 2 * x, uint16_t(v)); break;
    default: wr32(g_mem, off + 4 * x, v); break;
    }
}

struct Rect {
    int32_t l, t, r, b;
};

Rect readRect(uint32_t p, const Surface& s) {
    if (!p) {
        return {0, 0, int32_t(s.width), int32_t(s.height)};
    }
    return {int32_t(rd32(g_mem, p)), int32_t(rd32(g_mem, p + 4)), int32_t(rd32(g_mem, p + 8)), int32_t(rd32(g_mem, p + 12))};
}

void present(const Surface& s) {
    if (s.bpp == 32) {
        display_present_bgrx(g_mem + s.bits, int(s.width), int(s.height), int(s.pitch));
        return;
    }
    g_convert.resize(size_t(s.width) * s.height);
    const Palette* pal = nullptr;
    if (auto it = g_palettes.find(s.palette); it != g_palettes.end()) {
        pal = &it->second;
    }
    for (uint32_t y = 0; y < s.height; y++) {
        for (uint32_t x = 0; x < s.width; x++) {
            uint32_t v = pixelAt(s, x, y);
            uint32_t rgb;
            if (s.bpp == 16) {
                uint32_t r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
                rgb = ((r * 255 / 31) << 16) | ((g * 255 / 63) << 8) | (b * 255 / 31);
            } else if (pal) {
                rgb = (uint32_t(pal->entries[v][0]) << 16) | (uint32_t(pal->entries[v][1]) << 8) | pal->entries[v][2];
            } else {
                rgb = v * 0x010101u;
            }
            g_convert[size_t(y) * s.width + x] = rgb;
        }
    }
    display_present_bgrx(g_convert.data(), int(s.width), int(s.height), int(s.width * 4));
}

void presentIfPrimary(uint32_t obj) {
    if (Surface* s = surface(obj); s && (s->caps & DDSCAPS_PRIMARYSURFACE)) {
        present(*s);
    }
}

}

HLE("ddraw", DirectDrawCreate, 3) {
    if (a[1]) {
        wr32(m, a[1], newObject(ddrawVtable()));
    }
    return DD_OK;
}

HLE("ddraw", DD_QueryInterface, 3) {
    std::fprintf(stderr, "[ddraw] IDirectDraw::QueryInterface(%s) -> not supported\n", guidString(a[1]).c_str());
    wr32(m, a[2], 0);
    return E_NOINTERFACE;
}

HLE("ddraw", DD_AddRef, 1) { return 2; }
HLE("ddraw", DD_Release, 1) { return 0; }

HLE("ddraw", DD_SetCooperativeLevel, 3) {
    std::fprintf(stderr, "[ddraw] SetCooperativeLevel(flags 0x%x)\n", a[2]);
    return DD_OK;
}

HLE("ddraw", DD_SetDisplayMode, 4) {
    g_modeWidth = a[1];
    g_modeHeight = a[2];
    g_modeBpp = a[3];
    std::fprintf(stderr, "[ddraw] SetDisplayMode(%u x %u x %u)\n", a[1], a[2], a[3]);
    return DD_OK;
}

HLE("ddraw", DD_RestoreDisplayMode, 1) { return DD_OK; }
HLE("ddraw", DD_WaitForVerticalBlank, 3) { return DD_OK; }

HLE("ddraw", DD_GetDisplayMode, 2) {
    Surface s;
    s.width = g_modeWidth;
    s.height = g_modeHeight;
    s.bpp = g_modeBpp;
    s.pitch = g_modeWidth * g_modeBpp / 8;
    writeSurfaceDesc(a[1], s, 0);
    return DD_OK;
}

HLE("ddraw", DD_GetCaps, 3) {
    for (uint32_t p : {a[1], a[2]}) {
        if (!p) {
            continue;
        }
        uint32_t size = rd32(m, p);
        std::memset(g_mem + p, 0, size);
        wr32(m, p, size);
        wr32(m, p + 4, 0x00000040u | 0x04000000u | 0x00400000u);
        wr32(m, p + 60, 64u << 20);
        wr32(m, p + 64, 64u << 20);
    }
    return DD_OK;
}

HLE("ddraw", DD_EnumDisplayModes, 5) {
    static const uint32_t modes[][2] = {{640, 480}, {800, 600}, {1024, 768}, {1280, 960}, {1600, 1200}};
    uint32_t desc = guest_alloc(108, 4);
    for (uint32_t bpp : {16u, 32u}) {
        for (auto& md : modes) {
            Surface s;
            s.width = md[0];
            s.height = md[1];
            s.bpp = bpp;
            s.pitch = md[0] * bpp / 8;
            wr32(m, desc, 108);
            writeSurfaceDesc(desc, s, 0);
            if (call_guest(c, m, a[4], {desc, a[3]}) == 0) {
                guest_free(desc);
                return DD_OK;
            }
        }
    }
    guest_free(desc);
    return DD_OK;
}

HLE("ddraw", DD_CreateSurface, 4) {
    uint32_t d = a[1];
    uint32_t flags = rd32(m, d + 4);
    uint32_t caps = (flags & DDSD_CAPS) ? rd32(m, d + 104) : 0;
    uint32_t w = (flags & DDSD_WIDTH) ? rd32(m, d + 12) : g_modeWidth;
    uint32_t h = (flags & DDSD_HEIGHT) ? rd32(m, d + 8) : g_modeHeight;
    uint32_t bpp = (flags & DDSD_PIXELFORMAT) ? rd32(m, d + 72 + 12) : g_modeBpp;
    if (caps & DDSCAPS_PRIMARYSURFACE) {
        w = g_modeWidth;
        h = g_modeHeight;
        bpp = g_modeBpp;
    }
    if (bpp != 8 && bpp != 16 && bpp != 32) {
        std::fprintf(stderr, "[ddraw] CreateSurface with %u bpp is not supported\n", bpp);
        return DDERR_INVALIDPARAMS;
    }
    uint32_t obj = createSurface(w, h, bpp, caps | (caps & DDSCAPS_PRIMARYSURFACE ? DDSCAPS_FRONTBUFFER : 0));
    uint32_t backs = (flags & DDSD_BACKBUFFERCOUNT) ? rd32(m, d + 20) : 0;
    if (backs) {
        uint32_t back = createSurface(w, h, bpp, DDSCAPS_BACKBUFFER | DDSCAPS_FLIP | DDSCAPS_COMPLEX);
        surface(obj)->back = back;
    }
    if (caps & DDSCAPS_PRIMARYSURFACE) {
        g_primary = obj;
    }
    std::fprintf(stderr, "[ddraw] CreateSurface(%ux%u, %u bpp, caps 0x%x, %u back buffers) -> 0x%08x\n", w, h, bpp, caps, backs, obj);
    wr32(m, a[2], obj);
    return DD_OK;
}

HLE("ddraw", DD_CreatePalette, 5) {
    uint32_t obj = newObject(paletteVtable());
    Palette& p = g_palettes[obj];
    if (a[2]) {
        std::memcpy(p.entries, g_mem + a[2], 256 * 4);
    }
    wr32(m, a[3], obj);
    return DD_OK;
}

HLE("ddraw", DD_CreateClipper, 4) {
    wr32(m, a[2], newObject(clipperVtable()));
    return DD_OK;
}

HLE("ddraw", DDC_AddRef, 1) { return 2; }
HLE("ddraw", DDC_Release, 1) { return 0; }
HLE("ddraw", DDC_SetHWnd, 3) { return DD_OK; }

HLE("ddraw", DDP_AddRef, 1) { return 2; }
HLE("ddraw", DDP_Release, 1) { return 0; }

HLE("ddraw", DDP_SetEntries, 5) {
    auto it = g_palettes.find(a[0]);
    if (it == g_palettes.end()) {
        return DDERR_INVALIDPARAMS;
    }
    uint32_t start = std::min<uint32_t>(a[2], 256);
    uint32_t count = std::min<uint32_t>(a[3], 256 - start);
    std::memcpy(it->second.entries[start], g_mem + a[4], count * 4);
    if (g_primary && surface(g_primary)->palette == a[0]) {
        presentIfPrimary(g_primary);
    }
    return DD_OK;
}

HLE("ddraw", DDP_GetEntries, 5) {
    auto it = g_palettes.find(a[0]);
    if (it == g_palettes.end()) {
        return DDERR_INVALIDPARAMS;
    }
    uint32_t start = std::min<uint32_t>(a[2], 256);
    uint32_t count = std::min<uint32_t>(a[3], 256 - start);
    std::memcpy(g_mem + a[4], it->second.entries[start], count * 4);
    return DD_OK;
}

HLE("ddraw", DDS_QueryInterface, 3) {
    std::fprintf(stderr, "[ddraw] IDirectDrawSurface::QueryInterface(%s) -> not supported\n", guidString(a[1]).c_str());
    wr32(m, a[2], 0);
    return E_NOINTERFACE;
}

HLE("ddraw", DDS_AddRef, 1) {
    Surface* s = surface(a[0]);
    return s ? ++s->refs : 0;
}

HLE("ddraw", DDS_Release, 1) {
    Surface* s = surface(a[0]);
    if (!s) {
        return 0;
    }
    if (--s->refs) {
        return s->refs;
    }
    if (s->bits) {
        guest_free(s->bits);
    }
    if (g_primary == a[0]) {
        g_primary = 0;
    }
    g_surfaces.erase(a[0]);
    return 0;
}

HLE("ddraw", DDS_IsLost, 1) { return DD_OK; }
HLE("ddraw", DDS_Restore, 1) { return DD_OK; }
HLE("ddraw", DDS_SetClipper, 2) { return DD_OK; }

HLE("ddraw", DDS_SetPalette, 2) {
    Surface* s = surface(a[0]);
    if (!s) {
        return DDERR_INVALIDPARAMS;
    }
    s->palette = a[1];
    return DD_OK;
}

HLE("ddraw", DDS_SetColorKey, 3) {
    Surface* s = surface(a[0]);
    if (!s) {
        return DDERR_INVALIDPARAMS;
    }
    s->hasColorKey = a[2] != 0;
    s->colorKey = a[2] ? rd32(m, a[2]) : 0;
    return DD_OK;
}

HLE("ddraw", DDS_GetAttachedSurface, 3) {
    Surface* s = surface(a[0]);
    if (!s || !s->back) {
        return DDERR_NOTFOUND;
    }
    surface(s->back)->refs++;
    wr32(m, a[2], s->back);
    return DD_OK;
}

HLE("ddraw", DDS_GetSurfaceDesc, 2) {
    Surface* s = surface(a[0]);
    if (!s) {
        return DDERR_INVALIDPARAMS;
    }
    writeSurfaceDesc(a[1], *s, 0);
    return DD_OK;
}

HLE("ddraw", DDS_GetPixelFormat, 2) {
    Surface* s = surface(a[0]);
    if (!s) {
        return DDERR_INVALIDPARAMS;
    }
    writePixelFormat(a[1], s->bpp);
    return DD_OK;
}

HLE("ddraw", DDS_GetCaps, 2) {
    Surface* s = surface(a[0]);
    if (!s) {
        return DDERR_INVALIDPARAMS;
    }
    wr32(m, a[1], s->caps);
    return DD_OK;
}

HLE("ddraw", DDS_Lock, 5) {
    Surface* s = surface(a[0]);
    if (!s) {
        return DDERR_INVALIDPARAMS;
    }
    uint32_t bits = s->bits;
    if (a[1]) {
        Rect r = readRect(a[1], *s);
        bits += uint32_t(r.t) * s->pitch + uint32_t(r.l) * (s->bpp / 8);
    }
    writeSurfaceDesc(a[2], *s, bits);
    return DD_OK;
}

HLE("ddraw", DDS_Unlock, 2) {
    presentIfPrimary(a[0]);
    return DD_OK;
}

HLE("ddraw", DDS_Flip, 3) {
    Surface* s = surface(a[0]);
    if (!s) {
        return DDERR_INVALIDPARAMS;
    }
    if (Surface* b = surface(s->back)) {
        std::swap(s->bits, b->bits);
    }
    present(*s);
    return DD_OK;
}

HLE("ddraw", DDS_Blt, 6) {
    Surface* dst = surface(a[0]);
    if (!dst) {
        return DDERR_INVALIDPARAMS;
    }
    Rect dr = readRect(a[1], *dst);
    uint32_t flags = a[4];
    if (flags & DDBLT_COLORFILL) {
        uint32_t color = rd32(m, a[5] + 80);
        for (int32_t y = std::max(0, dr.t); y < std::min<int32_t>(dr.b, int32_t(dst->height)); y++) {
            for (int32_t x = std::max(0, dr.l); x < std::min<int32_t>(dr.r, int32_t(dst->width)); x++) {
                setPixel(*dst, uint32_t(x), uint32_t(y), color);
            }
        }
    } else if (Surface* src = surface(a[2])) {
        Rect sr = readRect(a[3], *src);
        bool key = (flags & DDBLT_KEYSRC) && src->hasColorKey;
        int32_t dw = dr.r - dr.l, dh = dr.b - dr.t, sw = sr.r - sr.l, sh = sr.b - sr.t;
        for (int32_t y = 0; y < dh; y++) {
            int32_t ty = dr.t + y;
            if (ty < 0 || ty >= int32_t(dst->height)) {
                continue;
            }
            int32_t sy = sr.t + y * sh / dh;
            for (int32_t x = 0; x < dw; x++) {
                int32_t tx = dr.l + x;
                if (tx < 0 || tx >= int32_t(dst->width)) {
                    continue;
                }
                uint32_t v = pixelAt(*src, uint32_t(sr.l + x * sw / dw), uint32_t(sy));
                if (!key || v != src->colorKey) {
                    setPixel(*dst, uint32_t(tx), uint32_t(ty), v);
                }
            }
        }
    } else {
        std::fprintf(stderr, "[ddraw] Blt with flags 0x%x is not supported\n", flags);
    }
    presentIfPrimary(a[0]);
    return DD_OK;
}

HLE("ddraw", DDS_BltFast, 6) {
    Surface* dst = surface(a[0]);
    Surface* src = surface(a[3]);
    if (!dst || !src) {
        return DDERR_INVALIDPARAMS;
    }
    Rect sr = readRect(a[4], *src);
    bool key = (a[5] & DDBLTFAST_SRCCOLORKEY) && src->hasColorKey;
    for (int32_t y = sr.t; y < sr.b; y++) {
        int32_t ty = int32_t(a[2]) + (y - sr.t);
        if (ty < 0 || ty >= int32_t(dst->height)) {
            continue;
        }
        for (int32_t x = sr.l; x < sr.r; x++) {
            int32_t tx = int32_t(a[1]) + (x - sr.l);
            if (tx < 0 || tx >= int32_t(dst->width)) {
                continue;
            }
            uint32_t v = pixelAt(*src, uint32_t(x), uint32_t(y));
            if (!key || v != src->colorKey) {
                setPixel(*dst, uint32_t(tx), uint32_t(ty), v);
            }
        }
    }
    presentIfPrimary(a[0]);
    return DD_OK;
}
