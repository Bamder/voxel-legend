#pragma once
#include <cstdint>
#include <vector>

namespace mat {

// A 32-bit RGBA image (8 bits per channel, tightly packed).
struct Image {
    int w = 0;
    int h = 0;
    std::vector<uint8_t> rgba; // size == w*h*4

    bool ok() const { return w > 0 && h > 0 && (int)rgba.size() == w * h * 4; }
    void clear() { w = 0; h = 0; rgba.clear(); }
};

// Load an uncompressed 32-bit BMP (BGRA) file. Returns an empty image on failure.
Image loadBMP(const char* path);

// Save RGBA as an uncompressed 32-bit BMP (BGRA). Used to bootstrap material files.
bool saveBMP(const char* path, int w, int h, const uint8_t* rgba);

// Load a PNG file (RGBA). Returns an empty image on failure.
Image loadPNG(const char* path);

// Save RGBA as a PNG file.
bool savePNG(const char* path, int w, int h, const uint8_t* rgba);

} // namespace mat
