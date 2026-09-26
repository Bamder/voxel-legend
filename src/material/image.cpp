#include "image.hpp"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <gdiplus.h>

namespace mat {

// ---------------------------------------------------------------------------
// PNG support via GDI+ (Windows Imaging). Handles compressed PNGs from any
// editor, so material images can be viewed and edited externally.
// ---------------------------------------------------------------------------
static ULONG_PTR g_gdiToken = 0;
static bool ensureGdiplus() {
    if (g_gdiToken) return true;
    Gdiplus::GdiplusStartupInput si;
    if (Gdiplus::GdiplusStartup(&g_gdiToken, &si, nullptr) != Gdiplus::Ok) return false;
    return true;
}

static std::wstring toWide(const char* s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s, -1, &w[0], n);
    return w;
}

static int GetEncoderClsid(const WCHAR* format, CLSID* pClsid) {
    UINT num = 0, size = 0;
    Gdiplus::GetImageEncodersSize(&num, &size);
    if (size == 0) return -1;
    Gdiplus::ImageCodecInfo* info = (Gdiplus::ImageCodecInfo*)std::malloc(size);
    if (!info) return -1;
    Gdiplus::GetImageEncoders(num, size, info);
    for (UINT i = 0; i < num; i++) {
        if (wcscmp(info[i].MimeType, format) == 0) {
            *pClsid = info[i].Clsid;
            std::free(info);
            return (int)i;
        }
    }
    std::free(info);
    return -1;
}

Image loadPNG(const char* path) {
    Image img;
    if (!ensureGdiplus()) return img;
    Gdiplus::Bitmap bmp(toWide(path).c_str());
    if (bmp.GetLastStatus() != Gdiplus::Ok) return img;
    int w = bmp.GetWidth(), h = bmp.GetHeight();
    if (w <= 0 || h <= 0) return img;
    img.w = w; img.h = h;
    img.rgba.resize((size_t)w * h * 4);

    Gdiplus::Rect rc(0, 0, w, h);
    Gdiplus::BitmapData bd;
    if (bmp.LockBits(&rc, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &bd) == Gdiplus::Ok) {
        for (int y = 0; y < h; y++) {
            const uint8_t* row = (const uint8_t*)bd.Scan0 + (size_t)y * bd.Stride;
            for (int x = 0; x < w; x++) {
                uint8_t b = row[x * 4 + 0], g = row[x * 4 + 1], r = row[x * 4 + 2], a = row[x * 4 + 3];
                size_t d = ((size_t)y * w + x) * 4;
                img.rgba[d + 0] = r; img.rgba[d + 1] = g; img.rgba[d + 2] = b; img.rgba[d + 3] = a;
            }
        }
        bmp.UnlockBits(&bd);
    } else {
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
            Gdiplus::Color c; bmp.GetPixel(x, y, &c);
            size_t d = ((size_t)y * w + x) * 4;
            img.rgba[d + 0] = c.GetR(); img.rgba[d + 1] = c.GetG(); img.rgba[d + 2] = c.GetB(); img.rgba[d + 3] = c.GetA();
        }
    }
    return img;
}

bool savePNG(const char* path, int w, int h, const uint8_t* rgba) {
    if (w <= 0 || h <= 0 || !rgba || !ensureGdiplus()) return false;
    Gdiplus::Bitmap bmp(w, h, PixelFormat32bppARGB);
    Gdiplus::Rect rc(0, 0, w, h);
    Gdiplus::BitmapData bd;
    if (bmp.LockBits(&rc, Gdiplus::ImageLockModeWrite, PixelFormat32bppARGB, &bd) != Gdiplus::Ok) return false;
    for (int y = 0; y < h; y++) {
        uint8_t* row = (uint8_t*)bd.Scan0 + (size_t)y * bd.Stride;
        for (int x = 0; x < w; x++) {
            const uint8_t* s = rgba + ((size_t)y * w + x) * 4;
            row[x * 4 + 0] = s[2]; // B
            row[x * 4 + 1] = s[1]; // G
            row[x * 4 + 2] = s[0]; // R
            row[x * 4 + 3] = s[3]; // A
        }
    }
    bmp.UnlockBits(&bd);
    CLSID clsid;
    if (GetEncoderClsid(L"image/png", &clsid) < 0) return false;
    return bmp.Save(toWide(path).c_str(), &clsid, nullptr) == Gdiplus::Ok;
}

// ---------------------------------------------------------------------------
// Minimal BMP reader/writer (kept for compatibility).
// ---------------------------------------------------------------------------

namespace {
struct BMPHeader {
    uint16_t bfType;       // 'BM'
    uint32_t bfSize;
    uint16_t bfReserved1;
    uint16_t bfReserved2;
    uint32_t bfOffBits;
    uint32_t biSize;       // 40
    int32_t  biWidth;
    int32_t  biHeight;     // positive => bottom-up
    uint16_t biPlanes;
    uint16_t biBitCount;   // 32
    uint32_t biCompression;// 0 = BI_RGB
    uint32_t biSizeImage;
    int32_t  biXPelsPerMeter;
    int32_t  biYPelsPerMeter;
    uint32_t biClrUsed;
    uint32_t biClrImportant;
};
} // namespace

Image loadBMP(const char* path) {
    Image img;
    FILE* f = std::fopen(path, "rb");
    if (!f) return img;

    BMPHeader h;
    if (std::fread(&h, sizeof(h), 1, f) != 1) { std::fclose(f); return img; }
    if (h.bfType != 0x4D42) { std::fclose(f); return img; } // 'BM'
    if (h.biBitCount != 32 || h.biCompression != 0 || h.biPlanes != 1) { std::fclose(f); return img; }

    int w = h.biWidth;
    int hh = h.biHeight;
    if (h.biHeight < 0) hh = -h.biHeight; // top-down BMP (rare); we handle as bottom-up below
    if (w <= 0 || hh <= 0) { std::fclose(f); return img; }

    img.w = w;
    img.h = hh;
    img.rgba.resize((size_t)w * hh * 4);

    std::fseek(f, h.bfOffBits, SEEK_SET);
    // Each row is 4-byte aligned (32bpp => already aligned).
    const int rowBytes = w * 4;
    for (int row = 0; row < hh; row++) {
        // BMP is bottom-up: source row index = hh-1-row.
        int srcRow = (h.biHeight > 0) ? (hh - 1 - row) : row;
        uint8_t* dst = &img.rgba[(size_t)row * w * 4];
        uint8_t buf[4096 * 4];
        // read one row of BGRA
        std::fseek(f, h.bfOffBits + (size_t)srcRow * rowBytes, SEEK_SET);
        size_t got = std::fread(buf, 1, (size_t)rowBytes, f);
        if (got != (size_t)rowBytes) { img.clear(); std::fclose(f); return img; }
        for (int i = 0; i < w; i++) {
            uint8_t b = buf[i * 4 + 0];
            uint8_t g = buf[i * 4 + 1];
            uint8_t r = buf[i * 4 + 2];
            uint8_t a = buf[i * 4 + 3];
            dst[i * 4 + 0] = r;
            dst[i * 4 + 1] = g;
            dst[i * 4 + 2] = b;
            dst[i * 4 + 3] = a;
        }
    }
    std::fclose(f);
    return img;
}

bool saveBMP(const char* path, int w, int h, const uint8_t* rgba) {
    if (w <= 0 || h <= 0 || !rgba) return false;
    FILE* f = std::fopen(path, "wb");
    if (!f) return false;

    const int rowBytes = w * 4;
    const uint32_t dataSize = (uint32_t)rowBytes * h;
    const uint32_t offBits = sizeof(BMPHeader);
    const uint32_t fileSize = offBits + dataSize;

    BMPHeader hd;
    std::memset(&hd, 0, sizeof(hd));
    hd.bfType = 0x4D42;
    hd.bfSize = fileSize;
    hd.bfOffBits = offBits;
    hd.biSize = 40;
    hd.biWidth = w;
    hd.biHeight = h; // bottom-up
    hd.biPlanes = 1;
    hd.biBitCount = 32;
    hd.biCompression = 0;
    hd.biSizeImage = dataSize;

    std::fwrite(&hd, sizeof(hd), 1, f);

    for (int row = h - 1; row >= 0; row--) {
        const uint8_t* src = rgba + (size_t)row * w * 4;
        for (int i = 0; i < w; i++) {
            uint8_t r = src[i * 4 + 0];
            uint8_t g = src[i * 4 + 1];
            uint8_t b = src[i * 4 + 2];
            uint8_t a = src[i * 4 + 3];
            uint8_t bgra[4] = { b, g, r, a };
            std::fwrite(bgra, 1, 4, f);
        }
    }
    std::fclose(f);
    return true;
}

} // namespace mat
