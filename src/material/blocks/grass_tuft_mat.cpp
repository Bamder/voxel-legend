#include "grass_tuft_mat.hpp"
#include "../../world/world.hpp" // Vertex
#include "../../core/noise.hpp"
#include <cmath>

namespace mat {

void buildGrassTuftMesh(const Material& m, std::vector<Vertex>& out,
                        int bx, int by, int bz, int wx, int wz,
                        float u0, float v0, float u1, float v1,
                        float heightScale) {
    if (!m.model.ok()) return;
    const RandParams& r = m.rand;

    const int R = (int)r.get("R", 16.0f);
    if (R < 2) return;
    const float P = 1.0f / (float)R;

    // Per-tuft tower density: normal distribution (Box-Muller), clamped.
    const float dMean = r.get("density_mean", 0.60f);
    const float dStd = r.get("density_std", 0.08f);
    const float dMin = r.get("density_min", 0.50f);
    const float dMax = r.get("density_max", 0.70f);
    float du1 = noise::hash01(noise::hash2(wx, wz, 0x5EEDu));
    float du2 = noise::hash01(noise::hash2(wz, wx, 0x5EEDu ^ 0x1u));
    if (du1 < 1e-4f) du1 = 1e-4f;
    float zz = std::sqrt(-2.0f * std::log(du1)) * std::cos(6.2831853f * du2);
    float threshold = dMean + zz * dStd;
    if (threshold < dMin) threshold = dMin;
    if (threshold > dMax) threshold = dMax;

    const float cf = r.get("cluster_freq", 0.22f);
    uint32_t cseed = 0xC157E2u + (uint32_t)(wx * 31) + (uint32_t)(wz * 17);

    // Tower geometry parameters.
    const float wb = r.get("wb_factor", 0.5f) * P; // blade half-width
    const float tallBase = r.get("tall_base", 0.375f);
    const float tallRange = r.get("tall_range", 0.375f);
    const float shortBase = r.get("short_base", 0.1875f);
    const float shortRange = r.get("short_range", 0.1875f);
    const float shadeMin = r.get("shade_min", 0.85f);
    const float shadeMax = r.get("shade_max", 1.15f);

    auto emitModel = [&](float cx, float cz, float ty, float scaleX, float scaleZ, float shade) {
        for (const Quad& q : m.model.quads) {
            Vertex vv[4];
            for (int c = 0; c < 4; c++) {
                // Model coords are normalized [0,1]; center at 0.5.
                float lx = (q.p[c][0] - 0.5f) * scaleX;
                float ly = q.p[c][1] * ty;
                float lz = (q.p[c][2] - 0.5f) * scaleZ;
                vv[c] = {
                    (float)bx + cx + lx,
                    (float)by + ly,
                    (float)bz + cz + lz,
                    u0 + (u1 - u0) * q.uv[c][0],
                    v0 + (v1 - v0) * q.uv[c][1],
                    0.0f, 1.0f, 0.0f, shade, 1.0f, 1.0f,
                };
            }
            out.push_back(vv[0]); out.push_back(vv[1]); out.push_back(vv[2]);
            out.push_back(vv[0]); out.push_back(vv[2]); out.push_back(vv[3]);
            if (q.doubleSided) {
                out.push_back(vv[0]); out.push_back(vv[2]); out.push_back(vv[1]);
                out.push_back(vv[0]); out.push_back(vv[3]); out.push_back(vv[2]);
            }
        }
    };

    for (int pz = 0; pz < R; pz++) {
        for (int px = 0; px < R; px++) {
            bool tall = ((px + pz) & 1) == 0; // parity checkerboard
            uint32_t seedBase = tall ? 0x9C2Au : 0x9C3Bu;
            float cn = noise::noise2((float)px * cf, (float)pz * cf, cseed);
            if (cn < threshold) continue;
            float hR = noise::hash01(noise::hash2(wx * R + px, wz * R + pz, seedBase ^ 0x1Bu));
            int bladeH = tall
                ? (int)((float)R * tallBase + hR * ((float)R * tallRange))
                : (int)((float)R * shortBase + hR * ((float)R * shortRange));
            if (bladeH < 1) bladeH = 1;
            float var = shadeMin + noise::hash01(noise::hash2(wx * R + px, wz * R + pz, seedBase ^ 0x2Du)) * (shadeMax - shadeMin);
            float cx = ((float)px + 0.5f) * P;
            float cz = ((float)pz + 0.5f) * P;
            float ty = (float)bladeH * P * (heightScale > 0.0f ? heightScale : 1.0f);
            emitModel(cx, cz, ty, 2.0f * wb, 2.0f * wb, var);
        }
    }
}

} // namespace mat
