#include "win32.h"

#include <cstdio>
#include <map>

namespace {

enum class GdiType { Brush, Font, Bitmap, Dc, Stock };

struct GdiObject {
    GdiType type;
    uint32_t color = 0;
    int32_t fontHeight = 16;
    int32_t width = 0;
    int32_t height = 0;
    uint32_t bpp = 0;
    uint32_t bits = 0;
    uint32_t bitmap = 0;
    uint32_t font = 0;
    uint32_t brush = 0;
    uint32_t textColor = 0;
    uint32_t bkColor = 0xffffff;
    uint32_t bkMode = 2;
};

std::mutex g_gdiMutex;
std::map<uint32_t, GdiObject> g_objects;
uint32_t g_nextObject = 0x30000;
unsigned g_textLogged = 0;

uint32_t addObject(GdiObject o) {
    std::lock_guard lock(g_gdiMutex);
    uint32_t h = g_nextObject;
    g_nextObject += 4;
    g_objects[h] = o;
    return h;
}

GdiObject* object(uint32_t h) {
    auto it = g_objects.find(h);
    return it == g_objects.end() ? nullptr : &it->second;
}

}

HLE("gdi32", GetStockObject, 1) {
    GdiObject o{GdiType::Stock};
    o.color = a[0] == 4 ? 0 : 0xffffff;
    return addObject(o);
}

HLE("gdi32", CreateBrushIndirect, 1) {
    GdiObject o{GdiType::Brush};
    o.color = rd32(m, a[0] + 4);
    return addObject(o);
}

HLE("gdi32", CreateFontA, 14) {
    GdiObject o{GdiType::Font};
    o.fontHeight = int32_t(a[0]) < 0 ? -int32_t(a[0]) : int32_t(a[0]);
    if (o.fontHeight == 0) {
        o.fontHeight = 16;
    }
    std::fprintf(stderr, "[gdi32] CreateFontA(height %d, weight %u, face \"%s\")\n", int32_t(a[0]), a[4], gstring(a[13]).c_str());
    return addObject(o);
}

HLE("gdi32", CreateCompatibleDC, 1) { return addObject(GdiObject{GdiType::Dc}); }

HLE("gdi32", CreateDIBSection, 6) {
    uint32_t bmi = a[1];
    int32_t w = int32_t(rd32(m, bmi + 4));
    int32_t h = int32_t(rd32(m, bmi + 8));
    uint32_t bpp = rd16(m, bmi + 14);
    uint32_t stride = ((uint32_t(w) * bpp + 31) / 32) * 4;
    uint32_t rows = uint32_t(h < 0 ? -h : h);
    uint32_t bits = guest_alloc(stride * rows, 16);
    if (!bits) {
        return 0;
    }
    if (a[3]) {
        wr32(m, a[3], bits);
    }
    GdiObject o{GdiType::Bitmap};
    o.width = w;
    o.height = h;
    o.bpp = bpp;
    o.bits = bits;
    std::fprintf(stderr, "[gdi32] CreateDIBSection(%dx%d, %u bpp) -> bits 0x%08x\n", w, h, bpp, bits);
    return addObject(o);
}

HLE("gdi32", SelectObject, 2) {
    std::lock_guard lock(g_gdiMutex);
    GdiObject* dc = object(a[0]);
    GdiObject* obj = object(a[1]);
    if (!dc || !obj) {
        return 0;
    }
    uint32_t prev = 0;
    switch (obj->type) {
    case GdiType::Bitmap: prev = dc->bitmap; dc->bitmap = a[1]; break;
    case GdiType::Font: prev = dc->font; dc->font = a[1]; break;
    default: prev = dc->brush; dc->brush = a[1]; break;
    }
    return prev ? prev : 0x30000 - 4;
}

HLE("gdi32", DeleteDC, 1) {
    std::lock_guard lock(g_gdiMutex);
    return g_objects.erase(a[0]) ? 1 : 0;
}

HLE("gdi32", DeleteObject, 1) {
    std::lock_guard lock(g_gdiMutex);
    GdiObject* o = object(a[0]);
    if (!o) {
        return 0;
    }
    if (o->type == GdiType::Bitmap && o->bits) {
        guest_free(o->bits);
    }
    g_objects.erase(a[0]);
    return 1;
}

HLE("gdi32", SetTextColor, 2) {
    std::lock_guard lock(g_gdiMutex);
    GdiObject* dc = object(a[0]);
    if (!dc) {
        return 0xffffffffu;
    }
    uint32_t prev = dc->textColor;
    dc->textColor = a[1];
    return prev;
}

HLE("gdi32", SetBkColor, 2) {
    std::lock_guard lock(g_gdiMutex);
    GdiObject* dc = object(a[0]);
    if (!dc) {
        return 0xffffffffu;
    }
    uint32_t prev = dc->bkColor;
    dc->bkColor = a[1];
    return prev;
}

HLE("gdi32", SetBkMode, 2) {
    std::lock_guard lock(g_gdiMutex);
    GdiObject* dc = object(a[0]);
    if (!dc) {
        return 0;
    }
    uint32_t prev = dc->bkMode;
    dc->bkMode = a[1];
    return prev;
}

HLE("gdi32", Rectangle, 5) {
    std::lock_guard lock(g_gdiMutex);
    GdiObject* dc = object(a[0]);
    GdiObject* bmp = dc ? object(dc->bitmap) : nullptr;
    GdiObject* brush = dc ? object(dc->brush) : nullptr;
    if (!bmp || bmp->bpp != 32) {
        return 1;
    }
    uint32_t color = brush ? brush->color : 0xffffff;
    uint32_t pixel = 0xff000000u | ((color & 0xff) << 16) | (color & 0xff00) | ((color >> 16) & 0xff);
    int32_t rows = bmp->height < 0 ? -bmp->height : bmp->height;
    for (int32_t y = std::max<int32_t>(0, int32_t(a[2])); y < std::min<int32_t>(rows, int32_t(a[4])); y++) {
        int32_t row = bmp->height < 0 ? y : rows - 1 - y;
        for (int32_t x = std::max<int32_t>(0, int32_t(a[1])); x < std::min<int32_t>(bmp->width, int32_t(a[3])); x++) {
            wr32(m, bmp->bits + uint32_t(row * bmp->width + x) * 4, pixel);
        }
    }
    return 1;
}

HLE("gdi32", GetTextExtentPoint32A, 4) {
    std::lock_guard lock(g_gdiMutex);
    GdiObject* dc = object(a[0]);
    GdiObject* font = dc ? object(dc->font) : nullptr;
    int32_t h = font ? font->fontHeight : 16;
    wr32(m, a[3], uint32_t(int32_t(a[2]) * h / 2));
    wr32(m, a[3] + 4, uint32_t(h));
    return 1;
}

HLE("gdi32", TextOutA, 5) {
    if (g_textLogged < 40) {
        g_textLogged++;
        std::fprintf(stderr, "[gdi32] TextOutA(%d, %d, \"%.*s\") is not drawn yet\n", int32_t(a[1]), int32_t(a[2]), int(a[4]), gstr(a[3]));
    }
    return 1;
}
