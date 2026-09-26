#pragma once
#include "../world/player_model.hpp"
#include "../world/player_skin.hpp"
#include "../world/hair_voxels.hpp"
#include "../material/registry.hpp"
#include "textures.hpp"
#include <string>
#include <unordered_map>
#include <vector>

// Shared player/entity appearance mesh. Matches Renderer::drawPlayerModel:
// skin UV cuboids, atlas tiles, overlay/extra named textures, hair remainders,
// hair cards, and double-sided cutout/decal quads. Callers supply a point
// transform (game: look/pitch/swing; editor: identity on already-posed parts)
// and a named-texture predicate (overlay/extra, not atlas).
namespace pdraw {

struct Mesh {
    std::vector<float> solid; // pos3 + color4
    std::vector<float> skin;  // pos3 + color4 + uv2
    std::vector<float> sheet; // model-own unwrap, same layout as skin
    std::vector<float> atlas;
    std::unordered_map<std::string, std::vector<float>> named;
};

inline void pushSolidQuad(std::vector<float>& dst, const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& d,
                          const Vec3& col) {
    const float v[6][7] = {
        { a.x, a.y, a.z, col.x, col.y, col.z, 1 }, { b.x, b.y, b.z, col.x, col.y, col.z, 1 }, { c.x, c.y, c.z, col.x, col.y, col.z, 1 },
        { a.x, a.y, a.z, col.x, col.y, col.z, 1 }, { c.x, c.y, c.z, col.x, col.y, col.z, 1 }, { d.x, d.y, d.z, col.x, col.y, col.z, 1 },
    };
    for (auto& e : v) for (int i = 0; i < 7; i++) dst.push_back(e[i]);
}

inline void pushTexQuad(std::vector<float>& dst,
                        const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& d,
                        float ua, float va, float ub, float vb, float uc, float vc, float ud, float vd,
                        const Vec3& col) {
    const float v[6][9] = {
        { a.x, a.y, a.z, col.x, col.y, col.z, 1, ua, va }, { b.x, b.y, b.z, col.x, col.y, col.z, 1, ub, vb }, { c.x, c.y, c.z, col.x, col.y, col.z, 1, uc, vc },
        { a.x, a.y, a.z, col.x, col.y, col.z, 1, ua, va }, { c.x, c.y, c.z, col.x, col.y, col.z, 1, uc, vc }, { d.x, d.y, d.z, col.x, col.y, col.z, 1, ud, vd },
    };
    for (auto& e : v) for (int i = 0; i < 9; i++) dst.push_back(e[i]);
}

inline bool skipHiddenHead(const pm::Part& p) {
    if (p.type == 0 || p.type == 4) return true;
    if (pm::isHairPart(p) || pm::isHairCardPart(p)) return true;
    if (pm::isEyePart(p) || pm::isEyelidPart(p) || pm::isMouthPart(p)) return true;
    return false;
}

inline Vec3 identXform(const pm::Part&, float x, float y, float z) {
    return { x, y, z };
}

template<typename Xform>
void pushCuboidUV(std::vector<float>& dst, const std::vector<pm::Part>& parts, const pm::Part& p, Xform&& xform,
                  const Vec3& col, bool skinUV, float u0, float v0, float u1, float v1) {
    for (int f = 0; f < 6; f++) {
        std::vector<pm::HairRect> rs;
        if (pm::isHairPart(p)) pm::hairFaceRemainders(p, f, parts, rs);
        else {
            pm::HairRect full;
            Vec3 pmn = p.center - p.half, pmx = p.center + p.half;
            int axis = f / 2, ua = (axis + 1) % 3, va = (axis + 2) % 3;
            full.u0 = pm::hairComp(pmn, ua); full.u1 = pm::hairComp(pmx, ua);
            full.v0 = pm::hairComp(pmn, va); full.v1 = pm::hairComp(pmx, va);
            rs.push_back(full);
        }
        float fu0 = u0, fv0 = v0, fu1 = u1, fv1 = v1;
        if (skinUV) pm::skinFaceUV(p, f, fu0, fv0, fu1, fv1);
        for (const pm::HairRect& hr : rs) {
            Vec3 q[4];
            pm::hairFaceCorners(p, f, hr, q);
            Vec3 a = xform(p, q[0].x, q[0].y, q[0].z);
            Vec3 b = xform(p, q[1].x, q[1].y, q[1].z);
            Vec3 c = xform(p, q[2].x, q[2].y, q[2].z);
            Vec3 d = xform(p, q[3].x, q[3].y, q[3].z);
            pushTexQuad(dst, a, b, c, d, fu0, fv1, fu1, fv1, fu1, fv0, fu0, fv0, col);
        }
    }
}

template<typename Xform>
void pushSolidCuboid(std::vector<float>& dst, const std::vector<pm::Part>& parts, const pm::Part& p, Xform&& xform,
                     const Vec3& col) {
    if (pm::isGridHairPart(p)) {
        for (int f = 0; f < 6; f++) {
            std::vector<pm::HairRect> rs;
            pm::hairFaceRemainders(p, f, parts, rs);
            for (const pm::HairRect& hr : rs) {
                Vec3 q[4];
                pm::hairFaceCorners(p, f, hr, q);
                Vec3 a = xform(p, q[0].x, q[0].y, q[0].z);
                Vec3 b = xform(p, q[1].x, q[1].y, q[1].z);
                Vec3 c = xform(p, q[2].x, q[2].y, q[2].z);
                Vec3 d = xform(p, q[3].x, q[3].y, q[3].z);
                pushSolidQuad(dst, a, b, c, d, col);
            }
        }
        return;
    }
    Vec3 c[8];
    for (int i = 0; i < 8; i++) {
        Vec3 o{
            (i & 1) ? p.half.x : -p.half.x,
            ((i >> 1) & 1) ? p.half.y : -p.half.y,
            ((i >> 2) & 1) ? p.half.z : -p.half.z
        };
        Vec3 w = pm::partWorldOffset(p, o);
        c[i] = xform(p, w.x, w.y, w.z);
    }
    pushSolidQuad(dst, c[1], c[3], c[7], c[5], col);
    pushSolidQuad(dst, c[0], c[4], c[6], c[2], col);
    pushSolidQuad(dst, c[2], c[3], c[7], c[6], col);
    pushSolidQuad(dst, c[0], c[1], c[5], c[4], col);
    pushSolidQuad(dst, c[4], c[5], c[7], c[6], col);
    pushSolidQuad(dst, c[0], c[1], c[3], c[2], col);
}

template<typename Xform, typename HasNamed>
void build(const std::vector<pm::Part>& parts, Xform&& xform, HasNamed&& hasNamed, bool hasSkin, bool hideHead, Mesh& out,
           bool colorizeSkin = false, int sheetW = 0, int sheetH = 0) {
    out.solid.clear();
    out.skin.clear();
    out.sheet.clear();
    out.atlas.clear();
    out.named.clear();
    const Vec3 white{ 1.0f, 1.0f, 1.0f };
    for (const pm::Part& p : parts) {
        if (hideHead && skipHiddenHead(p)) continue;
        if (pm::partHasBox(p) && sheetW > 0 && sheetH > 0) {
            for (int f = 0; f < 6; f++) {
                float u0, v0, u1, v1;
                pm::modelBoxUV(p, f, sheetW, sheetH, u0, v0, u1, v1);
                std::vector<pm::HairRect> rs;
                pm::hairFaceRemainders(p, f, parts, rs);
                for (const pm::HairRect& hr : rs) {
                    Vec3 q[4];
                    pm::hairFaceCorners(p, f, hr, q);
                    Vec3 a = xform(p, q[0].x, q[0].y, q[0].z);
                    Vec3 b = xform(p, q[1].x, q[1].y, q[1].z);
                    Vec3 c = xform(p, q[2].x, q[2].y, q[2].z);
                    Vec3 d = xform(p, q[3].x, q[3].y, q[3].z);
                    pushTexQuad(out.sheet, a, b, c, d, u0, v1, u1, v1, u1, v0, u0, v0, white);
                }
            }
            continue;
        }
        if (p.kind == "cloth") {
            pushSolidCuboid(out.solid, parts, p, xform, p.color);
            continue;
        }
        if (pm::isHairPart(p)) {
            if (!p.tex.empty() && !pm::isCutoutOverlay(p.tex) && pm::effectiveKind(p) != "cutout") {
                int ti = mat::tileIndex(p.tex.c_str());
                if (ti >= 0) {
                    float u0, v0, u1, v1;
                    tex::tileUV(ti, u0, v0, u1, v1);
                    pm::applyPartUvFlip(p, u0, v0, u1, v1);
                    pushCuboidUV(out.atlas, parts, p, xform, p.color, false, u0, v0, u1, v1);
                } else if (hasNamed(p.tex)) {
                    float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
                    pm::applyPartUvFlip(p, u0, v0, u1, v1);
                    pushCuboidUV(out.named[p.tex], parts, p, xform, p.color, false, u0, v0, u1, v1);
                } else
                    pushSolidCuboid(out.solid, parts, p, xform, p.color);
            } else {
                pushSolidCuboid(out.solid, parts, p, xform, p.color);
            }
            continue;
        }
        if (pm::isHairCardPart(p) && p.tex.empty()) {
            Vec3 q[4];
            pm::texQuadLocal(p, q);
            Vec3 bl = xform(p, q[0].x, q[0].y, q[0].z);
            Vec3 br = xform(p, q[1].x, q[1].y, q[1].z);
            Vec3 tr = xform(p, q[2].x, q[2].y, q[2].z);
            Vec3 tl = xform(p, q[3].x, q[3].y, q[3].z);
            pushSolidQuad(out.solid, bl, br, tr, tl, p.color);
            pushSolidQuad(out.solid, br, bl, tl, tr, p.color);
            continue;
        }
        bool ov = !p.tex.empty() && hasNamed(p.tex);
        int ti = p.tex.empty() ? -1 : mat::tileIndex(p.tex.c_str());
        if (pm::isCutoutPart(p) || pm::isDecalPart(p) || ov) {
            float u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f;
            if (ti >= 0 && !ov) tex::tileUV(ti, u0, v0, u1, v1);
            if (p.side < 0) { float t = u0; u0 = u1; u1 = t; }
            pm::applyPartUvFlip(p, u0, v0, u1, v1);
            Vec3 q[4];
            pm::texQuadLocal(p, q);
            Vec3 bl = xform(p, q[0].x, q[0].y, q[0].z);
            Vec3 br = xform(p, q[1].x, q[1].y, q[1].z);
            Vec3 tr = xform(p, q[2].x, q[2].y, q[2].z);
            Vec3 tl = xform(p, q[3].x, q[3].y, q[3].z);
            auto& dst = ov ? out.named[p.tex] : out.atlas;
            pushTexQuad(dst, bl, br, tr, tl, u0, v1, u1, v1, u1, v0, u0, v0, p.color);
            pushTexQuad(dst, br, bl, tl, tr, u1, v1, u0, v1, u0, v0, u1, v0, p.color);
            continue;
        }
        if (hasSkin) pushCuboidUV(out.skin, parts, p, xform, colorizeSkin ? p.color : white, true, 0, 0, 1, 1);
        else pushSolidCuboid(out.solid, parts, p, xform, p.color);
    }
}

} // namespace pdraw
