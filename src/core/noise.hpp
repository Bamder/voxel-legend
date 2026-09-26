#pragma once
#include <cstdint>
#include <cmath>

// Deterministic, self-contained value noise (no external tables, seed-driven).
namespace noise {

inline uint32_t hash2(int x, int z, uint32_t seed) {
    uint32_t h = (uint32_t)x * 0x27D4EB2Du ^ (uint32_t)z * 0x165667B1u ^ seed * 0x9E3779B9u;
    h ^= h >> 15; h *= 0x85EBCA6Bu; h ^= h >> 13; h *= 0xC2B2AE35u; h ^= h >> 16;
    return h;
}

inline uint32_t hash3(int x, int y, int z, uint32_t seed) {
    uint32_t h = (uint32_t)x * 0x27D4EB2Du ^ (uint32_t)y * 0x165667B1u ^ (uint32_t)z * 0x85EBCA6Bu ^ seed * 0x9E3779B9u;
    h ^= h >> 15; h *= 0x85EBCA6Bu; h ^= h >> 13; h *= 0xC2B2AE35u; h ^= h >> 16;
    return h;
}

inline float hash01(uint32_t h) { return (h & 0x00FFFFFFu) / 16777215.0f; }
inline float fade(float t) { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); }
inline float lerp(float a, float b, float t) { return a + (b - a) * t; }

inline float noise2(float x, float z, uint32_t seed) {
    int x0 = (int)std::floor(x), z0 = (int)std::floor(z);
    float fx = fade(x - (float)x0), fz = fade(z - (float)z0);
    float a = hash01(hash2(x0, z0, seed));
    float b = hash01(hash2(x0 + 1, z0, seed));
    float c = hash01(hash2(x0, z0 + 1, seed));
    float d = hash01(hash2(x0 + 1, z0 + 1, seed));
    return lerp(lerp(a, b, fx), lerp(c, d, fx), fz);
}

inline float fbm2(float x, float z, int octaves, uint32_t seed, float lacunarity = 2.0f, float gain = 0.5f) {
    float sum = 0.0f, amp = 1.0f, freq = 1.0f, norm = 0.0f;
    for (int i = 0; i < octaves; i++) {
        sum += noise2(x * freq, z * freq, seed + (uint32_t)(i * 1013 + 17)) * amp;
        norm += amp;
        amp *= gain; freq *= lacunarity;
    }
    return norm > 0.0f ? sum / norm : 0.0f;
}

inline float noise3(float x, float y, float z, uint32_t seed) {
    int x0 = (int)std::floor(x), y0 = (int)std::floor(y), z0 = (int)std::floor(z);
    float fx = fade(x - (float)x0), fy = fade(y - (float)y0), fz = fade(z - (float)z0);
    float c000 = hash01(hash3(x0, y0, z0, seed));
    float c100 = hash01(hash3(x0 + 1, y0, z0, seed));
    float c010 = hash01(hash3(x0, y0 + 1, z0, seed));
    float c110 = hash01(hash3(x0 + 1, y0 + 1, z0, seed));
    float c001 = hash01(hash3(x0, y0, z0 + 1, seed));
    float c101 = hash01(hash3(x0 + 1, y0, z0 + 1, seed));
    float c011 = hash01(hash3(x0, y0 + 1, z0 + 1, seed));
    float c111 = hash01(hash3(x0 + 1, y0 + 1, z0 + 1, seed));
    float x00 = lerp(c000, c100, fx), x10 = lerp(c010, c110, fx);
    float x01 = lerp(c001, c101, fx), x11 = lerp(c011, c111, fx);
    float y0v = lerp(x00, x10, fy), y1v = lerp(x01, x11, fy);
    return lerp(y0v, y1v, fz);
}

inline float fbm3(float x, float y, float z, int octaves, uint32_t seed, float lacunarity = 2.0f, float gain = 0.5f) {
    float sum = 0.0f, amp = 1.0f, freq = 1.0f, norm = 0.0f;
    for (int i = 0; i < octaves; i++) {
        sum += noise3(x * freq, y * freq, z * freq, seed + (uint32_t)(i * 797 + 31)) * amp;
        norm += amp;
        amp *= gain; freq *= lacunarity;
    }
    return norm > 0.0f ? sum / norm : 0.0f;
}

}
