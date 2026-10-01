#pragma once
#include "animation.hpp"
#include "asset_pack.hpp"
#include "blocks.hpp"
#include "loot.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace plugin { const char* blockId(uint8_t id); }

// Binding file: model skeleton <-> action clip <-> held item.
// Path: assets/entities/<rig>.hold
// One row is a relative pose of an item on a bone (usually arm_*_palm).
// side right/left = which palm OWNS the tool (tool is parented there).
// Files are authored for side right; the editor Tool L button mirrors preview.
// item/clip may be "*" (any). More specific rows win. Unknown keys round-trip.
namespace hold {

struct Spec {
    std::string item = "*";   // block stem or *
    std::string clip = "*";   // animation name or *
    std::string side = "right"; // left | right
    std::string bone;         // default arm_*_palm
    Vec3 grip{ 0.50f, 0.22f, 0.50f }; // model-space point that sits on the bone origin
    Vec3 offset{};            // extra translation in bone space after rot
    Vec3 rot{};               // Euler XYZ radians, bone-local
    float scale = 0.48f;
};

struct File {
    std::string rig = "player";
    std::vector<Spec> rows;
    std::vector<std::string> extraLines;
};

inline bool isWild(const std::string& s) { return s.empty() || s == "*"; }

inline const char* defaultBone(const std::string& side) {
    return (side == "left") ? "arm_l_palm" : "arm_r_palm";
}

inline std::string itemNameOf(uint8_t block) {
    if (block == AIR || !validBlock(block)) return {};
    // Lights must match player.hold stems even if a display name is registered.
    if (block == TORCH) return "torch";
    if (block == LANTERN) return "lantern";
    const char* id = plugin::blockId(block);
    if (id && id[0]) return std::string(id);
    const char* n = blockOf(block).name;
    return n ? std::string(n) : std::string{};
}

inline bool isHandLight(uint8_t b) {
    return b == TORCH || b == LANTERN;
}

inline bool isCarryBlock(uint8_t b) {
    if (b == AIR || !validBlock(b)) return false;
    // Torches and lanterns stay in the hand hotbar; they are not hug-carried.
    if (isHandLight(b)) return false;
    if (loot::isTool(b)) return false;
    return loot::itemDef(b).kind == loot::Kind::Block;
}

inline bool isCarrying(uint8_t carried) { return isCarryBlock(carried); }

inline int matchScore(const Spec& s, const std::string& item, const std::string& clip, const std::string& side) {
    if (!isWild(s.side) && s.side != side) return -1;
    if (!isWild(s.item) && s.item != item) return -1;
    if (!isWild(s.clip) && s.clip != clip) return -1;
    int sc = 0;
    if (!isWild(s.item)) sc += 4;
    if (!isWild(s.clip)) sc += 2;
    if (!isWild(s.side)) sc += 1;
    return sc;
}

inline Spec fallbackSpec(const std::string& side) {
    Spec s;
    s.side = (side == "left") ? "left" : "right";
    s.bone = defaultBone(s.side);
    return s;
}

inline Spec resolve(const File& f, const std::string& item, const std::string& clip, const std::string& side) {
    // Empty item id means "unknown block" — do not treat it as "*" or every
    // item-specific row is skipped and everything collapses to the wild grip.
    if (item.empty()) return fallbackSpec(side);
    int best = -1;
    int bestI = -1;
    for (int i = 0; i < (int)f.rows.size(); i++) {
        int sc = matchScore(f.rows[i], item, clip, side);
        if (sc > best) { best = sc; bestI = i; }
    }
    Spec out = (bestI >= 0) ? f.rows[bestI] : fallbackSpec(side);
    if (out.bone.empty()) out.bone = defaultBone(side);
    if (out.side.empty()) out.side = side;
    if (out.scale < 1e-4f) out.scale = 0.48f;
    return out;
}

inline Spec resolveBlock(const File& f, uint8_t block, const std::string& clip, const std::string& side) {
    return resolve(f, itemNameOf(block), clip, side);
}

inline int findRow(const File& f, const std::string& item, const std::string& clip, const std::string& side) {
    for (int i = 0; i < (int)f.rows.size(); i++) {
        const Spec& s = f.rows[i];
        if (s.item == item && s.clip == clip && s.side == side) return i;
    }
    return -1;
}

inline Spec& upsert(File& f, const Spec& src) {
    int i = findRow(f, src.item, src.clip, src.side);
    if (i >= 0) { f.rows[i] = src; return f.rows[i]; }
    f.rows.push_back(src);
    return f.rows.back();
}

inline Vec3 pointOnBone(const Spec& s, const anim::BoneXform& xf, float x, float y, float z) {
    Vec3 local{
        (x - s.grip.x) * s.scale,
        (y - s.grip.y) * s.scale,
        (z - s.grip.z) * s.scale
    };
    local = pm::rotateEuler(local, s.rot) + s.offset;
    return xf.pivot + anim::mul9(xf.R, local);
}

inline Vec3 originOnBone(const Spec& s, const anim::BoneXform& xf) {
    return xf.pivot + anim::mul9(xf.R, s.offset);
}

inline void composeFace(Spec& s, const Vec3& face, const Vec3& extraOff = {}) {
    s.offset += extraOff;
    if (face.lengthSq() < 1e-12f) return;
    float R[9], F[9], Out[9];
    pm::eulerToMat(s.rot, R);
    pm::eulerToMat(face, F);
    pm::mat3Mul(R, F, Out);
    s.rot = pm::matToEuler(Out);
}

// Between two arm poses, turn the tool around its own shaft. The short roll is
// the default. sense -1 takes the other direction; laps adds full turns that way.
// Arm joints stay on their authored interpolation. Only the carrier palm is aimed.
template <class EvalAt>
inline void applyToolAxis(const anim::Clip& keyClip, std::vector<anim::BoneXform>& pose,
                          float frame, const Spec& spec, EvalAt evalAt) {
    if (spec.bone.empty() || pose.empty()) return;
    int bi = anim::findBone(keyClip, spec.bone);
    if (bi < 0 || bi >= (int)pose.size()) return;
    std::vector<int> marks;
    anim::armKeyFrames(keyClip, spec.bone, marks);
    if (marks.size() < 2) return;
    int f0 = -1, f1 = -1;
    for (int m : marks) {
        if (std::fabs(frame - (float)m) < 0.04f) return;
        if ((float)m < frame) f0 = m;
        if ((float)m > frame && f1 < 0) f1 = m;
    }
    if (f0 < 0 || f1 < 0 || f1 <= f0) return;

    auto pose0 = evalAt((float)f0);
    auto pose1 = evalAt((float)f1);
    if (bi >= (int)pose0.size() || bi >= (int)pose1.size()) return;

    auto toolOf = [&](const float palm[9], float at, float out[9]) {
        float Rh[9], Rf[9], Rm[9];
        pm::eulerToMat(spec.rot, Rh);
        pm::eulerToMat(anim::evalFace(keyClip, at), Rf);
        pm::mat3Mul(Rh, Rf, Rm);
        pm::mat3Mul(palm, Rm, out);
    };
    float R0[9], R1[9];
    toolOf(pose0[bi].R, (float)f0, R0);
    toolOf(pose1[bi].R, (float)f1, R1);
    Vec3 d0{ R0[3], R0[4], R0[5] };
    Vec3 d1{ R1[3], R1[4], R1[5] };
    Vec3 x0{ R0[0], R0[1], R0[2] };
    Vec3 x1{ R1[0], R1[1], R1[2] };
    if (d0.lengthSq() < 1e-8f || d1.lengthSq() < 1e-8f) return;
    d0 = d0.normalized();
    d1 = d1.normalized();

    float cosSwing = d0.dot(d1);
    if (cosSwing > 1.0f) cosSwing = 1.0f;
    if (cosSwing < -1.0f) cosSwing = -1.0f;
    float swing = std::acos(cosSwing);
    Vec3 swingAxis = d0.cross(d1);
    if (swingAxis.lengthSq() < 1e-8f) {
        swingAxis = d0.cross(x0);
        if (swingAxis.lengthSq() < 1e-8f) swingAxis = d0.cross(Vec3{ 0, 0, 1 });
        swing = (cosSwing > 0.0f) ? 0.0f : 3.14159265f;
    }
    if (swingAxis.lengthSq() < 1e-8f) return;
    swingAxis = swingAxis.normalized();

    auto spin = [&](const Vec3& v, const Vec3& axis, float ang) {
        return pm::rotateAxis(v, axis, ang);
    };
    Vec3 xEnd = spin(x0, swingAxis, swing);
    float tau = 0.0f;
    {
        Vec3 a = xEnd - d1 * xEnd.dot(d1);
        Vec3 b = x1 - d1 * x1.dot(d1);
        if (a.lengthSq() > 1e-8f && b.lengthSq() > 1e-8f) {
            a = a.normalized();
            b = b.normalized();
            tau = std::atan2(a.cross(b).dot(d1), a.dot(b));
        }
    }
    int sense = 1, laps = 0;
    anim::toolTurnOf(keyClip, f0, f1, sense, laps);
    float sign = tau >= 0.0f ? 1.0f : -1.0f;
    if (std::fabs(tau) < 1e-4f) sign = 1.0f;
    float travel = (sense >= 0)
        ? tau + sign * 6.2831853f * (float)laps
        : (tau - sign * 6.2831853f) + (-sign) * 6.2831853f * (float)laps;

    float t = (frame - (float)f0) / (float)(f1 - f0);
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    Vec3 y = spin(d0, swingAxis, swing * t);
    Vec3 x = spin(x0, swingAxis, swing * t);
    if (y.lengthSq() < 1e-8f) return;
    y = y.normalized();
    x = spin(x, y, travel * t);
    x = x - y * x.dot(y);
    if (x.lengthSq() < 1e-8f) return;
    x = x.normalized();
    Vec3 z = x.cross(y);

    float Rtool[9] = {
        x.x, x.y, x.z,
        y.x, y.y, y.z,
        z.x, z.y, z.z
    };
    float Rh[9], Rf[9], Rm[9], Rt[9], Rpalm[9];
    pm::eulerToMat(spec.rot, Rh);
    pm::eulerToMat(anim::evalFace(keyClip, frame), Rf);
    pm::mat3Mul(Rh, Rf, Rm);
    anim::transpose9(Rm, Rt);
    pm::mat3Mul(Rtool, Rt, Rpalm);
    for (int i = 0; i < 9; i++) pose[bi].R[i] = Rpalm[i];
}

// Reflect the grasp through the palm's local YZ plane: world tool = R_bone * M * hold(p).
// Mirroring an arm pose is M*R*M; the held item needs this extra local X flip or it stays right-handed.
// Off-hand palm stays on the tool between keyframes. Each keyframe stores its
// own hold_touch point. In-betweens blend those stored points.
inline void keepToolContact(const anim::Clip& poseClip, std::vector<anim::BoneXform>& pose,
                            const anim::Clip& bindClip, float frame, Spec spec, bool respectKeys = true) {
    int ti = anim::findTrack(bindClip, anim::kTouchTrack);
    if (ti < 0 || bindClip.tracks[ti].keys.empty() || pose.empty()) return;
    const anim::Track& touch = bindClip.tracks[ti];
    if (respectKeys) {
        // A keyframe stores the pose itself. Contact only fills the spans between keys.
        for (const anim::Track& tr : bindClip.tracks)
            for (const anim::Key& k : tr.keys)
                if (std::fabs(frame - (float)k.frame) < 0.04f) return;
    } else {
        for (const anim::Key& k : touch.keys)
            if (std::fabs(frame - (float)k.frame) < 0.04f) return;
    }

    int loF = touch.keys.front().frame;
    int hiF = touch.keys.back().frame;
    for (const anim::Key& k : touch.keys) {
        if ((float)k.frame <= frame) loF = k.frame;
        if ((float)k.frame >= frame) { hiF = k.frame; break; }
    }

    auto carrierName = [&](const std::string& bone) {
        if (bone.find("arm_l_") != std::string::npos) return std::string("arm_l_palm");
        if (bone.find("arm_r_") != std::string::npos) return std::string("arm_r_palm");
        return std::string();
    };
    std::string carrier = carrierName(spec.bone);
    if (carrier.empty()) carrier = "arm_r_palm";
    std::string offName = (carrier.find("arm_r_") == 0) ? "arm_l_palm" : "arm_r_palm";

    const char* chainName[5] = {};
    std::string prefix = offName.substr(0, 6);
    std::string nSh = prefix + "shoulder", nUp = prefix + "upper", nFo = prefix + "fore";
    std::string nHa = prefix + "hand", nPa = prefix + "palm";
    chainName[0] = nSh.c_str();
    chainName[1] = nUp.c_str();
    chainName[2] = nFo.c_str();
    chainName[3] = nHa.c_str();
    chainName[4] = nPa.c_str();
    int chain[5];
    for (int i = 0; i < 5; i++) {
        chain[i] = anim::findBone(poseClip, chainName[i]);
        if (chain[i] < 0 || chain[i] >= (int)pose.size()) return;
    }
    int chest = anim::parentIndex(poseClip, chain[0]);
    if (chest < 0 || chest >= (int)pose.size()) return;

    Vec3 modelLo = anim::evalTrack(touch, (float)loF, bindClip.length, bindClip.loop).t;
    Vec3 modelHi = anim::evalTrack(touch, (float)hiF, bindClip.length, bindClip.loop).t;
    float u = 0.0f;
    if (hiF != loF)
        u = (frame - (float)loF) / (float)(hiF - loF);
    if (u < 0.0f) u = 0.0f;
    if (u > 1.0f) u = 1.0f;
    Vec3 model = modelLo + (modelHi - modelLo) * u;

    anim::BoneXform car{};
    if (!anim::boneXformOf(poseClip, pose, carrier, car)) return;
    Vec3 target = car.pivot + anim::mul9(car.R, model);

    // The evaluated pose is already the smooth blend. Pull the palm onto the
    // tool along the shortest arc from that pose so the arm cannot flip.
    auto rotateFrom = [&](int joint, const Vec3& axis, float ang) {
        if (std::fabs(ang) < 1e-5f || axis.lengthSq() < 1e-8f) return;
        float Rd[9];
        pm::axisAngleMat(axis, ang, Rd);
        Vec3 origin = pose[joint].pivot;
        bool on = false;
        for (int i = 0; i < 5; i++) {
            if (chain[i] == joint) on = true;
            if (!on) continue;
            int bi = chain[i];
            if (bi != joint)
                pose[bi].pivot = origin + anim::mul9(Rd, pose[bi].pivot - origin);
            float Rn[9];
            pm::mat3Mul(Rd, pose[bi].R, Rn);
            for (int n = 0; n < 9; n++) pose[bi].R[n] = Rn[n];
        }
    };
    auto aimPalm = [&](int joint) {
        Vec3 v1 = pose[chain[4]].pivot - pose[joint].pivot;
        Vec3 v2 = target - pose[joint].pivot;
        float l1 = std::sqrt(v1.lengthSq());
        float l2 = std::sqrt(v2.lengthSq());
        if (l1 < 1e-4f || l2 < 1e-4f) return;
        v1 = v1 * (1.0f / l1);
        v2 = v2 * (1.0f / l2);
        Vec3 axis = v1.cross(v2);
        float s = std::sqrt(axis.lengthSq());
        float c = v1.dot(v2);
        if (c > 1.0f) c = 1.0f;
        if (c < -1.0f) c = -1.0f;
        if (s < 1e-6f) return;
        float ang = std::atan2(s, c);
        const float kMax = 0.45f;
        if (ang > kMax) ang = kMax;
        rotateFrom(joint, axis * (1.0f / s), ang);
    };
    // Shoulder, upper arm, and forearm carry the hand. The wrist is not used
    // to hold a fixed angle against the shaft.
    for (int iter = 0; iter < 6; iter++)
        for (int ji = 2; ji >= 0; ji--) aimPalm(chain[ji]);

    // Turn the palm onto the forearm. The angle between palm and shaft may change.
    // The turn fades out at the sample frames so those authored wrists stay put.
    float align = std::sin(u * 3.14159265f);
    Vec3 forearm = pose[chain[3]].pivot - pose[chain[2]].pivot;
    Vec3 wrist = pose[chain[4]].pivot - pose[chain[3]].pivot;
    float lf = std::sqrt(forearm.lengthSq());
    float lw = std::sqrt(wrist.lengthSq());
    if (align > 1e-3f && lf > 1e-4f && lw > 1e-4f) {
        forearm = forearm * (1.0f / lf);
        wrist = wrist * (1.0f / lw);
        Vec3 axis = wrist.cross(forearm);
        float s = std::sqrt(axis.lengthSq());
        float c = wrist.dot(forearm);
        if (c > 1.0f) c = 1.0f;
        if (c < -1.0f) c = -1.0f;
        if (s > 1e-6f) rotateFrom(chain[3], axis * (1.0f / s), std::atan2(s, c) * align);
    }
    for (int iter = 0; iter < 4; iter++)
        for (int ji = 2; ji >= 0; ji--) aimPalm(chain[ji]);
}

// 180 degrees about a model axis. 0 red X, 1 yellow shaft Y, 2 blue Z.
// The grip stays put; the model faces the other way around that axis.
inline void flipModelAxis(Spec& s, int axis) {
    float R[9], Spin[9], Out[9];
    pm::eulerToMat(s.rot, R);
    Vec3 ax{ 1, 0, 0 };
    if (axis == 1) ax = { 0, 1, 0 };
    else if (axis == 2) ax = { 0, 0, 1 };
    pm::axisAngleMat(ax, 3.14159265f, Spin);
    pm::mat3Mul(R, Spin, Out);
    s.rot = pm::matToEuler(Out);
}

// Off-hand palm in the main palm's local space. This is the bind itself, so it
// does not move when another item's grip or facing is selected.
inline bool measureTouch(const anim::Clip& c, float frame, const Spec& spec, Vec3& model) {
    if (spec.bone.empty() || c.bones.empty()) return false;
    auto carrierName = [&](const std::string& bone) {
        if (bone.find("arm_l_") != std::string::npos) return std::string("arm_l_palm");
        if (bone.find("arm_r_") != std::string::npos) return std::string("arm_r_palm");
        return std::string();
    };
    std::string carrier = carrierName(spec.bone);
    if (carrier.empty()) carrier = "arm_r_palm";
    std::string offName = (carrier.find("arm_r_") == 0) ? "arm_l_palm" : "arm_r_palm";
    auto sample = anim::evalPose(c, frame);
    anim::BoneXform car{}, off{};
    if (!anim::boneXformOf(c, sample, carrier, car)) return false;
    if (!anim::boneXformOf(c, sample, offName, off)) return false;
    float Rt[9];
    anim::transpose9(car.R, Rt);
    model = anim::mul9(Rt, off.pivot - car.pivot);
    return true;
}

// Every keyed time stores its own hold_touch. Clips without that track are left alone.
inline void recordTouches(anim::Clip& c, const Spec& spec) {
    int ti = anim::findTrack(c, anim::kTouchTrack);
    if (ti < 0 || c.tracks[ti].keys.empty()) return;
    std::vector<int> times;
    for (const anim::Track& tr : c.tracks)
        for (const anim::Key& k : tr.keys)
            if (anim::isPoseTrack(tr.bone)) times.push_back(k.frame);
    std::sort(times.begin(), times.end());
    times.erase(std::unique(times.begin(), times.end()), times.end());
    for (int frame : times) {
        Vec3 model{};
        if (!measureTouch(c, (float)frame, spec, model)) continue;
        anim::setKey(c, anim::kTouchTrack, frame, model, {});
    }
}

// Write the pose contact used to show on each key into the keys themselves.
// Touch samples were already absolute. Other keyed times were only correct
// while contact ran on top of them.
inline void bakeKeyedContact(anim::Clip& c, const Spec& spec) {
    int ti = anim::findTrack(c, anim::kTouchTrack);
    if (ti < 0 || c.tracks[ti].keys.empty()) return;
    std::vector<int> times;
    for (const anim::Track& tr : c.tracks)
        for (const anim::Key& k : tr.keys) times.push_back(k.frame);
    std::sort(times.begin(), times.end());
    times.erase(std::unique(times.begin(), times.end()), times.end());
    for (int frame : times) {
        auto raw = anim::evalPose(c, (float)frame);
        auto shown = raw;
        keepToolContact(c, shown, c, (float)frame, spec, false);
        if (shown.size() != raw.size()) continue;
        for (int i = 0; i < (int)c.bones.size() && i < (int)shown.size(); i++) {
            bool changed = (shown[i].pivot - raw[i].pivot).lengthSq() > 1e-8f;
            if (!changed) {
                for (int n = 0; n < 9; n++)
                    if (std::fabs(shown[i].R[n] - raw[i].R[n]) > 1e-4f) changed = true;
            }
            if (!changed) continue;
            anim::Key k = anim::keyFromWorld(c, i, shown);
            anim::setKey(c, c.bones[i].name, frame, k.t, k.r);
        }
    }
}

inline void mirrorPalmX(Spec& s) {
    float R[9], Out[9];
    pm::eulerToMat(s.rot, R);
    for (int i = 0; i < 9; i++) Out[i] = R[i];
    Out[0] = -Out[0];
    Out[3] = -Out[3];
    Out[6] = -Out[6];
    s.rot = pm::matToEuler(Out);
    s.offset.x = -s.offset.x;
}

inline void nudge(Spec& s, const anim::BoneXform& xf, const Vec3& worldT, const Vec3& worldAxis, float worldAng) {
    if (worldT.lengthSq() > 0.0f) {
        float Rt[9];
        anim::transpose9(xf.R, Rt);
        s.offset += anim::mul9(Rt, worldT);
    }
    if (std::fabs(worldAng) > 1e-8f) {
        float Rhold[9], Rworld[9], Rd[9], Rn[9], Rnew[9], Rt[9];
        pm::eulerToMat(s.rot, Rhold);
        pm::mat3Mul(xf.R, Rhold, Rworld);
        pm::axisAngleMat(worldAxis, worldAng, Rd);
        pm::mat3Mul(Rd, Rworld, Rn);
        anim::transpose9(xf.R, Rt);
        pm::mat3Mul(Rt, Rn, Rnew);
        s.rot = pm::matToEuler(Rnew);
    }
}

inline File defaults(const std::string& rig) {
    File f;
    f.rig = rig.empty() ? "player" : rig;
    auto add = [&](const char* item, const char* side, const char* bone) {
        Spec s;
        s.item = item;
        s.clip = "*";
        s.side = side;
        s.bone = bone;
        s.grip = { 0.50f, 0.22f, 0.50f };
        s.offset = { 0.00f, -0.02f, 0.04f };
        s.rot = { 1.57080f, 0.00f, 0.00f };
        s.scale = 0.48f;
        f.rows.push_back(s);
    };
    add("*", "right", "arm_r_palm");
    add("*", "left", "arm_l_palm");
    auto addClip = [&](const char* clip, const char* side, const char* bone,
                       Vec3 offset, Vec3 rot, float scale) {
        Spec s;
        s.item = "*";
        s.clip = clip;
        s.side = side;
        s.bone = bone;
        s.grip = { 0.50f, 0.50f, 0.50f };
        s.offset = offset;
        s.rot = rot;
        s.scale = scale;
        f.rows.push_back(s);
    };
    addClip("hold_block", "right", "chest", { 0.00f, -0.13f, 0.34f }, { 0.00f, 0.00f, 0.00f }, 0.36f);
    addClip("hold_block", "left", "chest", { 0.00f, -0.13f, 0.34f }, { 0.00f, 0.00f, 0.00f }, 0.36f);
    // One-hand clamp: back face on chest/abs front (z=0.12), outer face on the
    // palm's inner plane (x=±0.20). Center = (±(0.20-half), -0.13, 0.12+half).
    addClip("tuck_r", "right", "chest", { 0.08f, -0.13f, 0.24f }, { 0.00f, 0.00f, 0.00f }, 0.24f);
    addClip("tuck_l", "left", "chest", { -0.08f, -0.13f, 0.24f }, { 0.00f, 0.00f, 0.00f }, 0.24f);
    // Torch / lantern raise: shaft upright in the raised palm.
    auto addLight = [&](const char* item, const char* side, const char* bone, const char* clip,
                        Vec3 grip, Vec3 offset, Vec3 rot, float scale) {
        Spec s;
        s.item = item;
        s.clip = clip;
        s.side = side;
        s.bone = bone;
        s.grip = grip;
        s.offset = offset;
        s.rot = rot;
        s.scale = scale;
        f.rows.push_back(s);
    };
    addLight("torch", "right", "arm_r_palm", "raise_r",
             { 0.50f, 0.18f, 0.50f }, { 0.00f, -0.02f, 0.04f }, { 0.20f, 0.00f, 0.00f }, 0.42f);
    addLight("torch", "left", "arm_l_palm", "raise_r",
             { 0.50f, 0.18f, 0.50f }, { 0.00f, -0.02f, 0.04f }, { 0.20f, 0.00f, 0.00f }, 0.42f);
    addLight("lantern", "right", "arm_r_palm", "raise_r",
             { 0.50f, 0.9754f, 0.50f }, { 0.00f, -0.02f, 0.03f }, { 0.07795f, 0.00f, 0.00f }, 0.40f);
    addLight("lantern", "left", "arm_l_palm", "raise_r",
             { 0.50f, 0.9754f, 0.50f }, { 0.00f, -0.02f, 0.03f }, { 0.07795f, 0.00f, 0.00f }, 0.40f);
    return f;
}

inline File load(const char* path) {
    File f;
    FILE* fp = std::fopen(path, "rb");
    if (!fp) return f;
    char line[1024];
    while (std::fgets(line, sizeof(line), fp)) {
        std::vector<std::string> t = pm::splitWs(line);
        if (t.empty() || t[0].empty() || t[0][0] == '#') continue;
        auto keep = [&]() {
            std::string raw = line;
            while (!raw.empty() && (raw.back() == '\n' || raw.back() == '\r')) raw.pop_back();
            if (!raw.empty()) f.extraLines.push_back(raw);
        };
        if (t[0] == "rig" && t.size() >= 2) { f.rig = t[1]; continue; }
        if (t[0] != "hold") { keep(); continue; }
        Spec s;
        for (size_t i = 1; i < t.size(); ) {
            auto need = [&](size_t n) { return i + n < t.size(); };
            if (t[i] == "item" && need(1)) { s.item = t[i + 1]; i += 2; }
            else if (t[i] == "clip" && need(1)) { s.clip = t[i + 1]; i += 2; }
            else if (t[i] == "side" && need(1)) { s.side = t[i + 1]; i += 2; }
            else if (t[i] == "bone" && need(1)) { s.bone = t[i + 1]; i += 2; }
            else if (t[i] == "grip" && need(3)) {
                s.grip.x = (float)std::atof(t[i + 1].c_str());
                s.grip.y = (float)std::atof(t[i + 2].c_str());
                s.grip.z = (float)std::atof(t[i + 3].c_str());
                i += 4;
            } else if (t[i] == "offset" && need(3)) {
                s.offset.x = (float)std::atof(t[i + 1].c_str());
                s.offset.y = (float)std::atof(t[i + 2].c_str());
                s.offset.z = (float)std::atof(t[i + 3].c_str());
                i += 4;
            } else if (t[i] == "rot" && need(3)) {
                s.rot.x = (float)std::atof(t[i + 1].c_str());
                s.rot.y = (float)std::atof(t[i + 2].c_str());
                s.rot.z = (float)std::atof(t[i + 3].c_str());
                i += 4;
            } else if (t[i] == "scale" && need(1)) {
                s.scale = (float)std::atof(t[i + 1].c_str());
                i += 2;
            } else i++;
        }
        if (s.bone.empty()) s.bone = defaultBone(s.side);
        f.rows.push_back(std::move(s));
    }
    std::fclose(fp);
    return f;
}

inline bool save(const char* path, const File& f) {
    FILE* fp = std::fopen(path, "wb");
    if (!fp) return false;
    std::fprintf(fp, "# hold bind: model skeleton <-> action clip <-> item\n");
    std::fprintf(fp, "# item/clip may be * (any). More specific rows win.\n");
    std::fprintf(fp, "# bone is usually arm_r_palm / arm_l_palm (child of the hand).\n");
    std::fprintf(fp, "# grip = model-space point on the bone; offset/rot are bone-local.\n");
    std::fprintf(fp, "rig %s\n", f.rig.empty() ? "player" : f.rig.c_str());
    for (const std::string& s : f.extraLines) std::fprintf(fp, "%s\n", s.c_str());
    for (const Spec& s : f.rows) {
        std::fprintf(fp,
            "hold item %s clip %s side %s bone %s grip %.4f %.4f %.4f offset %.5f %.5f %.5f rot %.5f %.5f %.5f scale %.4f\n",
            s.item.empty() ? "*" : s.item.c_str(),
            s.clip.empty() ? "*" : s.clip.c_str(),
            s.side.empty() ? "right" : s.side.c_str(),
            s.bone.empty() ? defaultBone(s.side) : s.bone.c_str(),
            s.grip.x, s.grip.y, s.grip.z,
            s.offset.x, s.offset.y, s.offset.z,
            s.rot.x, s.rot.y, s.rot.z,
            s.scale);
    }
    std::fclose(fp);
    return true;
}

inline void ensureCarryRows(File& f) {
    File d = defaults(f.rig.empty() ? "player" : f.rig);
    for (const Spec& s : d.rows) {
        if (s.clip != "hold_block" && s.clip != "tuck_r" && s.clip != "tuck_l"
            && s.clip != "raise_r") continue;
        if (findRow(f, s.item, s.clip, s.side) < 0) f.rows.push_back(s);
    }
}

inline File& playerHold() {
    static File f;
    static bool once = false;
    static std::filesystem::file_time_type mtime{};
    const std::string path = pack::holdFile("player");
    bool reload = !once;
    if (once) {
        std::error_code ec;
        auto mt = std::filesystem::last_write_time(path, ec);
        if (!ec && mt != mtime) reload = true;
    }
    if (reload) {
        once = true;
        std::error_code ec;
        mtime = std::filesystem::last_write_time(path, ec);
        f = load(path.c_str());
        if (f.rows.empty()) f = defaults("player");
        if (f.rig.empty()) f.rig = "player";
        ensureCarryRows(f);
    }
    return f;
}

} // namespace hold
