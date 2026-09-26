#pragma once
#include "player_model.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// Reusable animation clips. A clip keys named bones (channels); it does not
// own an entity. The editor binds a .model as a preview rig. Unknown records
// round-trip like .model files. See assets/entities/anims/*.animation.
namespace anim {

struct Bone {
    std::string name;
    std::string parent; // empty = root
    Vec3 pivot{};       // rest-pose world joint
};

struct Key {
    int frame = 0;
    Vec3 t{}; // local translation from rest
    Vec3 r{}; // local Euler XYZ radians
    // When set, the blend from the previous key turns by the short angle θ
    // plus spin full revolutions, instead of lerping the Euler numbers.
    bool spinSet = false;
    int spin = 0;
};

struct Track {
    std::string bone;
    std::vector<Key> keys; // sorted by frame
};

struct Clip {
    std::string name = "idle";
    float fps = 20.0f;
    int length = 20; // frames (0 .. length-1, last frame may loop to 0)
    bool loop = true;
    std::string rig; // suggested appearance stem (preview only)
    std::vector<Bone> bones;
    std::vector<Track> tracks;
    std::vector<std::string> extraLines;
    // How the held tool twists around its own shaft between two arm poses.
    // sense +1 follows the short roll, -1 takes the other direction.
    // laps adds that many extra full turns in the chosen direction.
    struct ToolTurn {
        int frameA = 0;
        int frameB = 0;
        int sense = 1;
        int laps = 0;
    };
    std::vector<ToolTurn> toolTurns;
};

struct BoneXform {
    Vec3 pivot{};
    float R[9]{}; // world rotation (column-major 3x3)
};

inline void ident9(float R[9]) {
    for (int i = 0; i < 9; i++) R[i] = 0.0f;
    R[0] = R[4] = R[8] = 1.0f;
}

inline Vec3 mul9(const float R[9], const Vec3& v) {
    return {
        R[0] * v.x + R[3] * v.y + R[6] * v.z,
        R[1] * v.x + R[4] * v.y + R[7] * v.z,
        R[2] * v.x + R[5] * v.y + R[8] * v.z
    };
}

inline bool isRigPart(const pm::Part& p) {
    if (p.type == 4) return false;
    const std::string& k = p.kind;
    if (k == "eye" || k == "eyelid" || k == "mouth" || k == "leaf" || k == "cutout"
        || k == "hair" || k == "haircard" || k == "decal")
        return false;
    if (!p.tex.empty() && p.half.z < 0.02f && p.half.x < 0.12f) return false;
    return !pm::partName(p).empty();
}

inline std::string inferParent(const std::string& name) {
    if (name == "head") return "neck";
    if (name == "neck") return "chest";
    if (name == "chest") return "abs";
    if (name == "abs") return "hip";
    if (name == "hip") return {};
    auto ends = [&](const char* s) {
        size_t n = std::strlen(s);
        return name.size() >= n && name.compare(name.size() - n, n, s) == 0;
    };
    auto prefix = [&](const char* s) {
        size_t n = std::strlen(s);
        return name.size() > n ? name.substr(0, name.size() - n) : std::string{};
    };
    if (ends("_palm")) return prefix("_palm") + "_hand";
    if (ends("_hand")) return prefix("_hand") + "_fore";
    if (ends("_fore")) return prefix("_fore") + "_upper";
    if (ends("_upper")) return prefix("_upper") + "_shoulder";
    if (ends("_shoulder")) return "chest";
    if (ends("_foot")) return prefix("_foot") + "_shin";
    if (ends("_shin")) return prefix("_shin") + "_thigh";
    if (ends("_thigh")) return "hip";
    return "hip";
}

inline Vec3 bonePivotOf(const pm::Part& p) {
    const std::string& n = pm::partName(p);
    if (p.type == 0 || n == "head")
        return { p.center.x, p.center.y - p.half.y, p.center.z };
    if (n.find("shoulder") != std::string::npos || n.find("upper") != std::string::npos
        || n.find("thigh") != std::string::npos || n.find("fore") != std::string::npos
        || n.find("shin") != std::string::npos || n.find("hand") != std::string::npos
        || n.find("foot") != std::string::npos)
        return { p.center.x, p.center.y + p.half.y, p.center.z };
    return p.center;
}

inline int findBone(const Clip& c, const std::string& name) {
    for (int i = 0; i < (int)c.bones.size(); i++)
        if (c.bones[i].name == name) return i;
    return -1;
}

inline int findTrack(const Clip& c, const std::string& bone) {
    for (int i = 0; i < (int)c.tracks.size(); i++)
        if (c.tracks[i].bone == bone) return i;
    return -1;
}

inline int parentIndex(const Clip& c, int bi) {
    if (bi < 0 || bi >= (int)c.bones.size()) return -1;
    if (c.bones[bi].parent.empty()) return -1;
    return findBone(c, c.bones[bi].parent);
}

inline std::vector<int> topoOrder(const Clip& c) {
    std::vector<int> order;
    std::vector<char> seen(c.bones.size(), 0);
    auto visit = [&](auto& self, int i) -> void {
        if (i < 0 || i >= (int)c.bones.size() || seen[i]) return;
        seen[i] = 1;
        self(self, parentIndex(c, i));
        order.push_back(i);
    };
    for (int i = 0; i < (int)c.bones.size(); i++) visit(visit, i);
    return order;
}

inline void ensurePalmBones(Clip& c, const std::vector<pm::Part>& parts) {
    for (const pm::Part& p : parts) {
        if (!isRigPart(p)) continue;
        std::string n = pm::partName(p);
        const char* suf = "_hand";
        size_t ns = n.size(), ss = std::strlen(suf);
        if (ns <= ss || n.compare(ns - ss, ss, suf) != 0) continue;
        std::string palm = n.substr(0, ns - ss) + "_palm";
        int hi = findBone(c, n);
        int pi = findBone(c, palm);
        if (pi < 0) {
            Bone b;
            b.name = palm;
            b.parent = (hi >= 0) ? n : std::string{};
            b.pivot = p.center;
            c.bones.push_back(std::move(b));
        } else {
            if (hi >= 0) c.bones[pi].parent = n;
            c.bones[pi].pivot = p.center;
        }
    }
}

inline Clip rigFromParts(const std::vector<pm::Part>& parts, const std::string& rigStem) {
    Clip c;
    c.name = "idle";
    c.rig = rigStem;
    c.fps = 20.0f;
    c.length = 20;
    c.loop = true;
    for (const pm::Part& p : parts) {
        if (!isRigPart(p)) continue;
        Bone b;
        b.name = pm::partName(p);
        if (findBone(c, b.name) >= 0) continue;
        b.parent = inferParent(b.name);
        b.pivot = bonePivotOf(p);
        c.bones.push_back(std::move(b));
    }
    for (Bone& b : c.bones)
        if (!b.parent.empty() && findBone(c, b.parent) < 0)
            b.parent.clear();
    if (c.bones.empty()) {
        Bone root;
        root.name = "root";
        root.pivot = { 0, 0.9f, 0 };
        c.bones.push_back(root);
    }
    ensurePalmBones(c, parts);
    return c;
}

inline void rebindPivots(Clip& c, const std::vector<pm::Part>& parts) {
    for (Bone& b : c.bones) {
        for (const pm::Part& p : parts) {
            if (!isRigPart(p)) continue;
            if (pm::partName(p) == b.name) {
                b.pivot = bonePivotOf(p);
                break;
            }
        }
    }
    ensurePalmBones(c, parts);
}

inline Key lerpKey(const Key& a, const Key& b, float t) {
    Key k;
    k.frame = a.frame;
    k.t = a.t + (b.t - a.t) * t;
    k.r = a.r + (b.r - a.r) * t;
    return k;
}

inline Key evalTrack(const Track& tr, float frame, int length, bool loop) {
    if (tr.keys.empty()) return {};
    if (tr.keys.size() == 1) return tr.keys[0];
    float f = frame;
    if (length > 1 && loop) {
        float span = (float)length;
        f = std::fmod(f, span);
        if (f < 0.0f) f += span;
    } else {
        if (f < 0.0f) f = 0.0f;
        if (f > (float)(length > 0 ? length - 1 : 0)) f = (float)(length > 0 ? length - 1 : 0);
    }
    const Key* lo = &tr.keys.front();
    const Key* hi = &tr.keys.back();
    if (f <= (float)lo->frame) return *lo;
    if (f >= (float)hi->frame) {
        if (loop && length > 1 && lo->frame != hi->frame) {
            float a = (float)hi->frame;
            float b = (float)(lo->frame + length);
            if (f >= a && b > a) {
                float t = (f - a) / (b - a);
                return lerpKey(*hi, *lo, t);
            }
        }
        return *hi;
    }
    for (int i = 0; i + 1 < (int)tr.keys.size(); i++) {
        const Key& a = tr.keys[i];
        const Key& b = tr.keys[i + 1];
        if (f >= (float)a.frame && f <= (float)b.frame) {
            if (b.frame == a.frame) return a;
            float t = (f - (float)a.frame) / (float)(b.frame - a.frame);
            return lerpKey(a, b, t);
        }
    }
    return *hi;
}

inline Key evalBone(const Clip& c, const std::string& bone, float frame) {
    int ti = findTrack(c, bone);
    if (ti < 0) return {};
    return evalTrack(c.tracks[ti], frame, c.length, c.loop);
}

inline std::vector<BoneXform> evalPose(const Clip& c, float frame) {
    std::vector<BoneXform> out(c.bones.size());
    auto order = topoOrder(c);
    for (int i : order) {
        const Bone& b = c.bones[i];
        Key k = evalBone(c, b.name, frame);
        float local[9];
        pm::eulerToMat(k.r, local);
        int p = parentIndex(c, i);
        if (p < 0) {
            out[i].pivot = b.pivot + k.t;
            for (int n = 0; n < 9; n++) out[i].R[n] = local[n];
        } else {
            Vec3 restOff = (b.pivot - c.bones[p].pivot) + k.t;
            out[i].pivot = out[p].pivot + mul9(out[p].R, restOff);
            pm::mat3Mul(out[p].R, local, out[i].R);
        }
    }
    return out;
}

// Hip and legs. Upper-body strikes are stamped as a whole pose, and these
// channels sit at rest — the same as idle. They must not replace walk or run.
inline bool isLowerBodyBone(const std::string& name) {
    if (name == "hip") return true;
    return name.rfind("leg_", 0) == 0;
}

inline bool keyIsRest(const Key& k) {
    const float e = 1.0e-4f;
    return std::fabs(k.t.x) < e && std::fabs(k.t.y) < e && std::fabs(k.t.z) < e
        && std::fabs(k.r.x) < e && std::fabs(k.r.y) < e && std::fabs(k.r.z) < e;
}

inline bool trackIsRest(const Track& tr) {
    for (const Key& k : tr.keys)
        if (!keyIsRest(k)) return false;
    return true;
}

inline std::vector<BoneXform> evalPoseLayered(const Clip& base, float baseFrame,
                                             const Clip* const* layers, int nLayers, float layerFrame) {
    std::vector<BoneXform> out(base.bones.size());
    auto order = topoOrder(base);
    for (int i : order) {
        const Bone& b = base.bones[i];
        Key k{};
        bool fromLayer = false;
        for (int li = nLayers - 1; li >= 0; li--) {
            if (!layers[li]) continue;
            int ti = findTrack(*layers[li], b.name);
            if (ti < 0) continue;
            if (isLowerBodyBone(b.name) && trackIsRest(layers[li]->tracks[ti]))
                continue;
            k = evalBone(*layers[li], b.name, layerFrame);
            fromLayer = true;
            break;
        }
        if (!fromLayer) k = evalBone(base, b.name, baseFrame);
        float local[9];
        pm::eulerToMat(k.r, local);
        int p = parentIndex(base, i);
        if (p < 0) {
            out[i].pivot = b.pivot + k.t;
            for (int n = 0; n < 9; n++) out[i].R[n] = local[n];
        } else {
            Vec3 restOff = (b.pivot - base.bones[p].pivot) + k.t;
            out[i].pivot = out[p].pivot + mul9(out[p].R, restOff);
            pm::mat3Mul(out[p].R, local, out[i].R);
        }
    }
    return out;
}

inline bool boneXformOf(const Clip& c, const std::vector<BoneXform>& pose, const std::string& name, BoneXform& out) {
    int i = findBone(c, name);
    if (i < 0 || i >= (int)pose.size()) return false;
    out = pose[i];
    return true;
}

inline void transpose9(const float R[9], float Rt[9]) {
    Rt[0] = R[0]; Rt[1] = R[3]; Rt[2] = R[6];
    Rt[3] = R[1]; Rt[4] = R[4]; Rt[5] = R[7];
    Rt[6] = R[2]; Rt[7] = R[5]; Rt[8] = R[8];
}

inline Vec3 toParentLocal(const Clip& c, int bi, float frame, const Vec3& world) {
    int p = parentIndex(c, bi);
    if (p < 0) return world;
    auto pose = evalPose(c, frame);
    if (p >= (int)pose.size()) return world;
    float Rt[9];
    transpose9(pose[p].R, Rt);
    return mul9(Rt, world);
}

inline std::string boneForPart(const Clip& c, const pm::Part& p) {
    std::string n = pm::partName(p);
    if (findBone(c, n) >= 0) return n;
    // Several cloth pieces share one limb: leg_l_foot, leg_l_foot_2, ...
    std::string best;
    for (const Bone& b : c.bones) {
        const std::string& bn = b.name;
        if (bn.empty() || bn.size() >= n.size()) continue;
        if (n.compare(0, bn.size(), bn) != 0) continue;
        char sep = n[bn.size()];
        if (sep != '_' && sep != '-') continue;
        if (bn.size() > best.size()) best = bn;
    }
    if (!best.empty()) return best;
    if (p.type == 0 || p.type == 4 || p.kind == "hair" || p.kind == "haircard"
        || p.kind == "eye" || p.kind == "eyelid" || p.kind == "mouth") {
        if (findBone(c, "head") >= 0) return "head";
        if (findBone(c, "neck") >= 0) return "neck";
    }
    std::string par = inferParent(n);
    if (findBone(c, par) >= 0) return par;
    if (findBone(c, "hip") >= 0) return "hip";
    if (!c.bones.empty()) return c.bones[0].name;
    return {};
}

inline std::vector<pm::Part> poseParts(const std::vector<pm::Part>& rest, const Clip& c,
                                       const std::vector<BoneXform>& pose) {
    std::vector<pm::Part> out = rest;
    for (pm::Part& p : out) {
        std::string bn = boneForPart(c, p);
        int bi = findBone(c, bn);
        if (bi < 0 || bi >= (int)pose.size()) continue;
        const Bone& b = c.bones[bi];
        Vec3 off = p.center - b.pivot;
        p.center = pose[bi].pivot + mul9(pose[bi].R, off);
        float Rp[9], Rn[9];
        pm::eulerToMat(p.rot, Rp);
        pm::mat3Mul(pose[bi].R, Rp, Rn);
        p.rot = pm::matToEuler(Rn);
    }
    return out;
}

inline std::vector<pm::Part> poseParts(const std::vector<pm::Part>& rest, const Clip& c, float frame) {
    return poseParts(rest, c, evalPose(c, frame));
}

// Joint at the far end of this bone's segment (fore -> hand is elbow -> wrist).
inline int distalBone(const Clip& c, int bi) {
    int best = -1;
    float bestD = 1e-8f;
    if (bi < 0 || bi >= (int)c.bones.size()) return -1;
    for (int i = 0; i < (int)c.bones.size(); i++) {
        if (parentIndex(c, i) != bi) continue;
        float d = (c.bones[i].pivot - c.bones[bi].pivot).lengthSq();
        if (d > bestD) { bestD = d; best = i; }
    }
    return best;
}

// Rest-pose axis of the segment this bone owns. Origin is the bone pivot.
// Forearm: elbow to wrist. No child: the incoming segment, still through this pivot.
inline bool boneSegmentAxis(const Clip& c, int bi, Vec3& origin, Vec3& dir) {
    if (bi < 0 || bi >= (int)c.bones.size()) return false;
    origin = c.bones[bi].pivot;
    Vec3 end;
    int ch = distalBone(c, bi);
    if (ch >= 0) end = c.bones[ch].pivot;
    else {
        int p = parentIndex(c, bi);
        if (p < 0) return false;
        end = origin + (origin - c.bones[p].pivot);
    }
    dir = end - origin;
    if (dir.lengthSq() < 1e-10f) return false;
    dir = dir.normalized();
    return true;
}

// Spin parts skinned to this bone around its skeleton segment. Pivots stay put.
inline void rollPartsAroundBone(std::vector<pm::Part>& parts, const Clip& c, int bi, float ang) {
    if (std::fabs(ang) < 1e-8f) return;
    Vec3 origin, dir;
    if (!boneSegmentAxis(c, bi, origin, dir)) return;
    if (bi < 0 || bi >= (int)c.bones.size()) return;
    const std::string& name = c.bones[bi].name;
    for (pm::Part& p : parts) {
        if (boneForPart(c, p) != name) continue;
        p.center = origin + pm::rotateAxis(p.center - origin, dir, ang);
        pm::composePartRot(p.rot, dir, ang);
    }
}

inline void sortKeys(Track& tr) {
    std::sort(tr.keys.begin(), tr.keys.end(), [](const Key& a, const Key& b) { return a.frame < b.frame; });
}

inline void setKey(Clip& c, const std::string& bone, int frame, const Vec3& t, const Vec3& r) {
    int ti = findTrack(c, bone);
    if (ti < 0) {
        Track tr;
        tr.bone = bone;
        c.tracks.push_back(std::move(tr));
        ti = (int)c.tracks.size() - 1;
    }
    Track& tr = c.tracks[ti];
    for (Key& k : tr.keys) {
        if (k.frame == frame) { k.t = t; k.r = r; return; }
    }
    Key k;
    k.frame = frame;
    k.t = t;
    k.r = r;
    tr.keys.push_back(k);
    sortKeys(tr);
}

// Model-space grasp point on the held item. Not a skeleton bone. Empty means
// the clip uses the static grip from the hold file.
inline const char* kGripTrack = "hold_grip";
// Per keyframe, the off-hand palm in the main palm's local space.
// In-betweens blend these stored points. The keyframe pose itself is not overwritten.
inline const char* kTouchTrack = "hold_touch";
// Extra model-space rotation of the held item. Identity means the hold-file
// facing. Keyed per frame so one pose can turn without rewriting the others.
inline const char* kFaceTrack = "hold_face";

inline bool isPoseTrack(const std::string& bone) {
    return bone != kGripTrack && bone != kTouchTrack && bone != kFaceTrack;
}

inline void armKeyFrames(const Clip& c, const std::string& carrier, std::vector<int>& marks) {
    marks.clear();
    int bi = findBone(c, carrier);
    if (bi < 0) return;
    for (int i = bi; i >= 0; ) {
        int ti = findTrack(c, c.bones[i].name);
        if (ti >= 0)
            for (const Key& k : c.tracks[ti].keys) marks.push_back(k.frame);
        int p = parentIndex(c, i);
        if (p < 0 || c.bones[p].name.rfind("arm_", 0) != 0) break;
        i = p;
    }
    std::sort(marks.begin(), marks.end());
    marks.erase(std::unique(marks.begin(), marks.end()), marks.end());
}

inline bool toolTurnOf(const Clip& c, int frameA, int frameB, int& sense, int& laps) {
    int lo = frameA < frameB ? frameA : frameB;
    int hi = frameA < frameB ? frameB : frameA;
    for (const Clip::ToolTurn& s : c.toolTurns) {
        int a = s.frameA < s.frameB ? s.frameA : s.frameB;
        int b = s.frameA < s.frameB ? s.frameB : s.frameA;
        if (a == lo && b == hi) {
            sense = s.sense < 0 ? -1 : 1;
            laps = s.laps < 0 ? 0 : s.laps;
            return true;
        }
    }
    sense = 1;
    laps = 0;
    return false;
}

inline void upsertToolTurn(Clip& c, int frameA, int frameB, int sense, int laps) {
    int lo = frameA < frameB ? frameA : frameB;
    int hi = frameA < frameB ? frameB : frameA;
    if (laps < 0) laps = 0;
    if (laps > 8) laps = 8;
    sense = sense < 0 ? -1 : 1;
    for (Clip::ToolTurn& s : c.toolTurns) {
        int a = s.frameA < s.frameB ? s.frameA : s.frameB;
        int b = s.frameA < s.frameB ? s.frameB : s.frameA;
        if (a == lo && b == hi) {
            s.frameA = lo;
            s.frameB = hi;
            s.sense = sense;
            s.laps = laps;
            return;
        }
    }
    Clip::ToolTurn s;
    s.frameA = lo;
    s.frameB = hi;
    s.sense = sense;
    s.laps = laps;
    c.toolTurns.push_back(s);
}

// Mark each bone segment that lies inside [frameA, frameB]. The later key
// stores how many extra revolutions to add on top of the short angle θ.
inline int setSpanSpin(Clip& c, int frameA, int frameB, int turns);

inline bool spanSpinValue(const Clip& c, int frameA, int frameB, int& turns, bool& mixed) {
    int lo = frameA < frameB ? frameA : frameB;
    int hi = frameA < frameB ? frameB : frameA;
    bool any = false;
    mixed = false;
    turns = 0;
    for (const Track& tr : c.tracks) {
        if (!isPoseTrack(tr.bone)) continue;
        for (int i = 1; i < (int)tr.keys.size(); i++) {
            const Key& cur = tr.keys[i];
            int prev = tr.keys[i - 1].frame;
            if (prev < lo || cur.frame > hi || cur.frame <= prev || !cur.spinSet) continue;
            if (!any) turns = cur.spin;
            else if (cur.spin != turns) mixed = true;
            any = true;
        }
    }
    return any;
}

inline Vec3 evalGrip(const Clip& c, float frame, const Vec3& fallback) {
    int ti = findTrack(c, kGripTrack);
    if (ti < 0 || c.tracks[ti].keys.empty()) return fallback;
    return evalTrack(c.tracks[ti], frame, c.length, c.loop).t;
}

inline Vec3 evalFace(const Clip& c, float frame) {
    int ti = findTrack(c, kFaceTrack);
    if (ti < 0 || c.tracks[ti].keys.empty()) return {};
    return evalTrack(c.tracks[ti], frame, c.length, c.loop).r;
}

inline Vec3 evalFaceOff(const Clip& c, float frame) {
    int ti = findTrack(c, kFaceTrack);
    if (ti < 0 || c.tracks[ti].keys.empty()) return {};
    return evalTrack(c.tracks[ti], frame, c.length, c.loop).t;
}

inline Vec3 clampGrip(Vec3 g) {
    auto c = [](float v) {
        if (v < -2.50f) return -2.50f;
        if (v > 2.50f) return 2.50f;
        return v;
    };
    return { c(g.x), c(g.y), c(g.z) };
}

inline bool trackHasKey(const Clip& c, const std::string& bone, int frame) {
    int ti = findTrack(c, bone);
    if (ti < 0) return false;
    for (const Key& k : c.tracks[ti].keys)
        if (k.frame == frame) return true;
    return false;
}

// Other keyed times keep the facing they show now. `shift` is a bone-local
// position offset; `face` is the extra model rotation.
inline void setFaceKey(Clip& c, int frame, const Vec3& shift, const Vec3& face) {
    std::vector<int> times;
    for (const Track& tr : c.tracks)
        for (const Key& k : tr.keys)
            if (k.frame != frame) times.push_back(k.frame);
    std::sort(times.begin(), times.end());
    times.erase(std::unique(times.begin(), times.end()), times.end());
    for (int t : times) {
        if (trackHasKey(c, kFaceTrack, t)) continue;
        setKey(c, kFaceTrack, t, evalFaceOff(c, (float)t), evalFace(c, (float)t));
    }
    setKey(c, kFaceTrack, frame, shift, face);
}

// A keyframe is the whole model at that time. Bones with no key of their own
// were still showing a pose (held from another key, or blended). Write that
// pose in so the frame stands alone. Bones that already have a key are left
// as authored.
inline void fillWholePose(Clip& c, int frame) {
    if (frame < 0) return;
    if (c.length > 0 && frame >= c.length) return;
    for (const Bone& b : c.bones) {
        if (trackHasKey(c, b.name, frame)) continue;
        Key k = evalBone(c, b.name, (float)frame);
        setKey(c, b.name, frame, k.t, k.r);
    }
    int gi = findTrack(c, kGripTrack);
    if (gi >= 0 && !c.tracks[gi].keys.empty() && !trackHasKey(c, kGripTrack, frame)) {
        Key k = evalTrack(c.tracks[gi], (float)frame, c.length, c.loop);
        setKey(c, kGripTrack, frame, k.t, {});
    }
}

// Other keyed times keep the grasp they show now. Then this frame stores `grip`.
inline void setGripKey(Clip& c, int frame, const Vec3& grip, const Vec3& fallback) {
    std::vector<int> times;
    for (const Track& tr : c.tracks)
        for (const Key& k : tr.keys)
            if (k.frame != frame) times.push_back(k.frame);
    std::sort(times.begin(), times.end());
    times.erase(std::unique(times.begin(), times.end()), times.end());
    for (int t : times) {
        if (trackHasKey(c, kGripTrack, t)) continue;
        setKey(c, kGripTrack, t, evalGrip(c, (float)t, fallback), {});
    }
    setKey(c, kGripTrack, frame, clampGrip(grip), {});
}

inline void anchorOtherPoses(Clip& c, int frame) {
    std::vector<int> times;
    for (const Track& tr : c.tracks)
        for (const Key& k : tr.keys)
            if (k.frame != frame) times.push_back(k.frame);
    std::sort(times.begin(), times.end());
    times.erase(std::unique(times.begin(), times.end()), times.end());
    for (int t : times) fillWholePose(c, t);
}

// Freeze every other keyed time as its own whole-model pose, then record this
// frame the same way. Later edits of this frame do not move those poses.
inline void stampKeyframe(Clip& c, int frame) {
    anchorOtherPoses(c, frame);
    fillWholePose(c, frame);
}

// Record the pose that is actually on screen. Tool-axis and off-hand contact
// are applied after evalPose, and both turn off on a fresh key, so writing the
// raw blend makes the tool and the off-hand jump.
inline void writeShownPose(Clip& c, int frame, const std::vector<BoneXform>& shown) {
    if (frame < 0 || shown.size() != c.bones.size()) return;
    anchorOtherPoses(c, frame);
    auto raw = evalPose(c, (float)frame);
    for (int i = 0; i < (int)c.bones.size(); i++) {
        Key k = evalBone(c, c.bones[i].name, (float)frame);
        bool changed = (shown[i].pivot - raw[i].pivot).lengthSq() > 1e-8f;
        if (!changed) {
            for (int n = 0; n < 9; n++)
                if (std::fabs(shown[i].R[n] - raw[i].R[n]) > 1e-4f) changed = true;
        }
        if (!changed) {
            setKey(c, c.bones[i].name, frame, k.t, k.r);
            continue;
        }
        int p = parentIndex(c, i);
        float local[9];
        if (p < 0 || p >= (int)shown.size()) {
            for (int n = 0; n < 9; n++) local[n] = shown[i].R[n];
        } else {
            float Rt[9];
            transpose9(shown[p].R, Rt);
            pm::mat3Mul(Rt, shown[i].R, local);
        }
        setKey(c, c.bones[i].name, frame, k.t, pm::matToEuler(local));
    }
}

// Rest pose at `frame`: every bone at identity, grasp at the hold-file default.
// Other keyed times keep the pose they already show.
inline void blankKeyframe(Clip& c, int frame, const Vec3& gripFallback) {
    if (frame < 0) return;
    if (c.length > 0 && frame >= c.length) return;
    anchorOtherPoses(c, frame);
    for (const Bone& b : c.bones)
        setKey(c, b.name, frame, {}, {});
    setGripKey(c, frame, gripFallback, gripFallback);
}

// Local key that rebuilds one bone's world pivot and rotation from `shown`.
inline Key keyFromWorld(const Clip& c, int i, const std::vector<BoneXform>& shown) {
    Key k;
    if (i < 0 || i >= (int)c.bones.size() || i >= (int)shown.size()) return k;
    int p = parentIndex(c, i);
    if (p < 0 || p >= (int)shown.size()) {
        k.t = shown[i].pivot - c.bones[i].pivot;
        k.r = pm::matToEuler(shown[i].R);
        return k;
    }
    float Rt[9], local[9];
    transpose9(shown[p].R, Rt);
    pm::mat3Mul(Rt, shown[i].R, local);
    Vec3 restOff = c.bones[i].pivot - c.bones[p].pivot;
    k.t = mul9(Rt, shown[i].pivot - shown[p].pivot) - restOff;
    k.r = pm::matToEuler(local);
    return k;
}

// Overwrite `frame` with a stored whole pose. Other keyed times stay put.
inline void writePose(Clip& c, int frame, const std::vector<std::string>& bones,
                      const std::vector<Key>& keys, const Vec3& grip, const Vec3& gripFallback) {
    if (frame < 0) return;
    if (c.length > 0 && frame >= c.length) return;
    anchorOtherPoses(c, frame);
    for (size_t i = 0; i < bones.size() && i < keys.size(); i++)
        setKey(c, bones[i], frame, keys[i].t, keys[i].r);
    setGripKey(c, frame, grip, gripFallback);
}

// Twist one bone around `worldAxis` at this frame only. Other keys stay as
// authored; the spans to the previous and next keys pick up the new blend.
// withChain spins the limb below too. Otherwise direct children are keyed so
// their world pose stays, and only this bone turns.
inline void rollBoneKey(Clip& c, int bi, int frame, const Vec3& worldAxis, float ang, bool withChain) {
    if (bi < 0 || bi >= (int)c.bones.size() || std::fabs(ang) < 1e-8f) return;
    if (worldAxis.lengthSq() < 1e-10f) return;
    const std::string& name = c.bones[bi].name;
    Key k = evalBone(c, name, (float)frame);
    auto before = evalPose(c, (float)frame);
    int p = parentIndex(c, bi);
    float Rloc[9], Rworld[9], Rd[9], Rn[9], Rnew[9];
    pm::eulerToMat(k.r, Rloc);
    if (p >= 0 && p < (int)before.size()) pm::mat3Mul(before[p].R, Rloc, Rworld);
    else for (int i = 0; i < 9; i++) Rworld[i] = Rloc[i];
    pm::axisAngleMat(worldAxis, ang, Rd);
    pm::mat3Mul(Rd, Rworld, Rn);
    if (p >= 0 && p < (int)before.size()) {
        float Rt[9];
        transpose9(before[p].R, Rt);
        pm::mat3Mul(Rt, Rn, Rnew);
    } else {
        for (int i = 0; i < 9; i++) Rnew[i] = Rn[i];
    }
    setKey(c, name, frame, k.t, pm::matToEuler(Rnew));
    if (withChain) return;
    auto after = evalPose(c, (float)frame);
    if (bi >= (int)after.size()) return;
    for (int ci = 0; ci < (int)c.bones.size(); ci++) {
        if (parentIndex(c, ci) != bi || ci >= (int)before.size()) continue;
        float Rt[9], Rchild[9];
        transpose9(after[bi].R, Rt);
        pm::mat3Mul(Rt, before[ci].R, Rchild);
        Vec3 restOff = c.bones[ci].pivot - c.bones[bi].pivot;
        Vec3 held = mul9(Rt, before[ci].pivot - after[bi].pivot);
        setKey(c, c.bones[ci].name, frame, held - restOff, pm::matToEuler(Rchild));
    }
}

inline void nudgeBoneWorld(Clip& c, int bi, int frame, const Vec3& worldT, const Vec3& worldAxis, float worldAng) {
    if (bi < 0 || bi >= (int)c.bones.size()) return;
    const std::string& name = c.bones[bi].name;
    stampKeyframe(c, frame);
    Key k = evalBone(c, name, (float)frame);
    if (worldT.lengthSq() > 0.0f)
        k.t += toParentLocal(c, bi, (float)frame, worldT);
    if (std::fabs(worldAng) > 1e-8f) {
        auto pose = evalPose(c, (float)frame);
        int p = parentIndex(c, bi);
        float Rloc[9], Rworld[9], Rd[9], Rn[9], Rnew[9];
        pm::eulerToMat(k.r, Rloc);
        if (p >= 0 && p < (int)pose.size()) pm::mat3Mul(pose[p].R, Rloc, Rworld);
        else for (int i = 0; i < 9; i++) Rworld[i] = Rloc[i];
        pm::axisAngleMat(worldAxis, worldAng, Rd);
        pm::mat3Mul(Rd, Rworld, Rn);
        if (p >= 0 && p < (int)pose.size()) {
            float Rt[9];
            transpose9(pose[p].R, Rt);
            pm::mat3Mul(Rt, Rn, Rnew);
        } else {
            for (int i = 0; i < 9; i++) Rnew[i] = Rn[i];
        }
        k.r = pm::matToEuler(Rnew);
    }
    setKey(c, name, frame, k.t, k.r);
}

inline bool frameHasKeys(const Clip& c, int frame) {
    for (const Track& tr : c.tracks)
        for (const Key& k : tr.keys)
            if (k.frame == frame) return true;
    return false;
}

// Drop every channel at this time: bones, grasp, and the off-hand bind marks.
inline void deleteKeyframe(Clip& c, int frame) {
    for (int ti = (int)c.tracks.size() - 1; ti >= 0; --ti) {
        Track& tr = c.tracks[ti];
        for (int i = (int)tr.keys.size() - 1; i >= 0; --i)
            if (tr.keys[i].frame == frame) tr.keys.erase(tr.keys.begin() + i);
        if (tr.keys.empty()) c.tracks.erase(c.tracks.begin() + ti);
    }
}

// Move the whole pose from one frame to another. A pose already at the
// destination is replaced. Keys stay ordered for interpolation.
inline bool moveKeyframe(Clip& c, int from, int to) {
    if (from == to) return false;
    if (to < 0 || (c.length > 0 && to >= c.length)) return false;
    if (!frameHasKeys(c, from)) return false;
    if (frameHasKeys(c, to)) deleteKeyframe(c, to);
    for (Track& tr : c.tracks) {
        for (Key& k : tr.keys)
            if (k.frame == from) k.frame = to;
        sortKeys(tr);
    }
    return true;
}

// Drop every whole pose whose frame lies in [frameA, frameB], inclusive.
inline int deleteKeyframeSpan(Clip& c, int frameA, int frameB) {
    int lo = frameA < frameB ? frameA : frameB;
    int hi = frameA < frameB ? frameB : frameA;
    std::vector<int> frames;
    for (const Track& tr : c.tracks)
        for (const Key& k : tr.keys)
            if (k.frame >= lo && k.frame <= hi) frames.push_back(k.frame);
    std::sort(frames.begin(), frames.end());
    frames.erase(std::unique(frames.begin(), frames.end()), frames.end());
    for (int f : frames) deleteKeyframe(c, f);
    return (int)frames.size();
}

// Slide every whole pose in [frameA, frameB] by delta frames. A pose already
// sitting on a destination outside the span is replaced. outA/outB are the
// span after the slide, clamped to the clip.
inline bool moveKeyframeSpan(Clip& c, int frameA, int frameB, int delta, int& outA, int& outB) {
    outA = frameA;
    outB = frameB;
    if (delta == 0) return false;
    int lo = frameA < frameB ? frameA : frameB;
    int hi = frameA < frameB ? frameB : frameA;
    int last = c.length > 0 ? c.length - 1 : 0;
    std::vector<int> frames;
    for (const Track& tr : c.tracks)
        for (const Key& k : tr.keys)
            if (k.frame >= lo && k.frame <= hi) frames.push_back(k.frame);
    std::sort(frames.begin(), frames.end());
    frames.erase(std::unique(frames.begin(), frames.end()), frames.end());
    if (frames.empty()) return false;
    if (delta > 0) std::reverse(frames.begin(), frames.end());
    bool any = false;
    for (int f : frames) {
        int to = f + delta;
        if (to < 0) to = 0;
        if (to > last) to = last;
        if (moveKeyframe(c, f, to)) any = true;
    }
    auto clamp = [&](int f) {
        f += delta;
        if (f < 0) f = 0;
        if (f > last) f = last;
        return f;
    };
    outA = clamp(frameA);
    outB = clamp(frameB);
    return any;
}

// Map a frame onto a new clip length so its place on the 0..length-1 bar stays put.
inline int scaledFrame(int frame, int oldLength, int newLength) {
    int oldLast = oldLength > 1 ? oldLength - 1 : 0;
    int newLast = newLength > 1 ? newLength - 1 : 0;
    if (frame < 0) return frame;
    if (oldLast <= 0) return 0;
    int s = (int)std::lround((double)frame * (double)newLast / (double)oldLast);
    if (s < 0) s = 0;
    if (s > newLast) s = newLast;
    return s;
}

inline void clampClipFrames(Clip& c);

// Change the clip length and slide every key so gaps stay the same fraction of the bar.
// Distinct times stay distinct while the new bar has room; otherwise later keys win.
inline void scaleClipLength(Clip& c, int newLength) {
    if (newLength < 2) newLength = 2;
    if (newLength > 240) newLength = 240;
    if (c.length < 2 || newLength == c.length) {
        c.length = newLength;
        return;
    }
    int oldLength = c.length;
    int newLast = newLength - 1;
    std::vector<int> times;
    for (const Track& tr : c.tracks)
        for (const Key& k : tr.keys) times.push_back(k.frame);
    for (const Clip::ToolTurn& t : c.toolTurns) {
        times.push_back(t.frameA);
        times.push_back(t.frameB);
    }
    std::sort(times.begin(), times.end());
    times.erase(std::unique(times.begin(), times.end()), times.end());
    std::vector<int> mapped(times.size());
    int prev = -1;
    int n = (int)times.size();
    for (int i = 0; i < n; i++) {
        int slot = scaledFrame(times[i], oldLength, newLength);
        int minS = prev + 1;
        int maxS = newLast - (n - 1 - i);
        if (maxS < minS) {
            if (slot < 0) slot = 0;
            if (slot > newLast) slot = newLast;
        } else {
            if (slot < minS) slot = minS;
            if (slot > maxS) slot = maxS;
        }
        mapped[i] = slot;
        prev = slot;
    }
    auto remap = [&](int f) {
        auto it = std::lower_bound(times.begin(), times.end(), f);
        if (it != times.end() && *it == f) return mapped[(int)(it - times.begin())];
        return scaledFrame(f, oldLength, newLength);
    };
    for (Track& tr : c.tracks) {
        std::vector<Key> next;
        for (Key k : tr.keys) {
            k.frame = remap(k.frame);
            bool hit = false;
            for (Key& e : next)
                if (e.frame == k.frame) { e = k; hit = true; break; }
            if (!hit) next.push_back(std::move(k));
        }
        tr.keys = std::move(next);
        sortKeys(tr);
    }
    for (int i = (int)c.tracks.size() - 1; i >= 0; --i)
        if (c.tracks[i].keys.empty()) c.tracks.erase(c.tracks.begin() + i);
    for (Clip::ToolTurn& t : c.toolTurns) {
        t.frameA = remap(t.frameA);
        t.frameB = remap(t.frameB);
    }
    for (int i = (int)c.toolTurns.size() - 1; i >= 0; --i)
        if (c.toolTurns[i].frameA == c.toolTurns[i].frameB)
            c.toolTurns.erase(c.toolTurns.begin() + i);
    c.length = newLength;
    clampClipFrames(c);
}

// Pull every sample back onto 0..length-1. The old tail stays on the new tail
// instead of sitting one slot past the bar.
inline void clampClipFrames(Clip& c) {
    if (c.length < 1) c.length = 1;
    int last = c.length - 1;
    for (Track& tr : c.tracks) {
        std::vector<Key> next;
        for (Key k : tr.keys) {
            if (k.frame > last) k.frame = last;
            if (k.frame < 0) k.frame = 0;
            bool hit = false;
            for (Key& e : next)
                if (e.frame == k.frame) { e = k; hit = true; break; }
            if (!hit) next.push_back(std::move(k));
        }
        tr.keys = std::move(next);
        sortKeys(tr);
    }
    for (int i = (int)c.tracks.size() - 1; i >= 0; --i)
        if (c.tracks[i].keys.empty()) c.tracks.erase(c.tracks.begin() + i);
    for (Clip::ToolTurn& t : c.toolTurns) {
        if (t.frameA > last) t.frameA = last;
        if (t.frameB > last) t.frameB = last;
        if (t.frameA < 0) t.frameA = 0;
        if (t.frameB < 0) t.frameB = 0;
    }
    for (int i = (int)c.toolTurns.size() - 1; i >= 0; --i)
        if (c.toolTurns[i].frameA == c.toolTurns[i].frameB)
            c.toolTurns.erase(c.toolTurns.begin() + i);
}

struct Quat {
    float x = 0, y = 0, z = 0, w = 1;
};

inline Quat matToQuat(const float R[9]) {
    Quat q;
    float tr = R[0] + R[4] + R[8];
    if (tr > 0.0f) {
        float s = std::sqrt(tr + 1.0f) * 2.0f;
        q.w = 0.25f * s;
        q.x = (R[5] - R[7]) / s;
        q.y = (R[6] - R[2]) / s;
        q.z = (R[1] - R[3]) / s;
    } else if (R[0] > R[4] && R[0] > R[8]) {
        float s = std::sqrt(std::max(0.0f, 1.0f + R[0] - R[4] - R[8])) * 2.0f;
        if (s < 1e-8f) return q;
        q.w = (R[5] - R[7]) / s;
        q.x = 0.25f * s;
        q.y = (R[1] + R[3]) / s;
        q.z = (R[6] + R[2]) / s;
    } else if (R[4] > R[8]) {
        float s = std::sqrt(std::max(0.0f, 1.0f + R[4] - R[0] - R[8])) * 2.0f;
        if (s < 1e-8f) return q;
        q.w = (R[6] - R[2]) / s;
        q.x = (R[1] + R[3]) / s;
        q.y = 0.25f * s;
        q.z = (R[5] + R[7]) / s;
    } else {
        float s = std::sqrt(std::max(0.0f, 1.0f + R[8] - R[0] - R[4])) * 2.0f;
        if (s < 1e-8f) return q;
        q.w = (R[1] - R[3]) / s;
        q.x = (R[6] + R[2]) / s;
        q.y = (R[5] + R[7]) / s;
        q.z = 0.25f * s;
    }
    return q;
}

inline void quatToMat(Quat q, float R[9]) {
    float x = q.x, y = q.y, z = q.z, w = q.w;
    float xx = x * x, yy = y * y, zz = z * z;
    float xy = x * y, xz = x * z, yz = y * z;
    float wx = w * x, wy = w * y, wz = w * z;
    R[0] = 1.0f - 2.0f * (yy + zz); R[3] = 2.0f * (xy - wz); R[6] = 2.0f * (xz + wy);
    R[1] = 2.0f * (xy + wz);        R[4] = 1.0f - 2.0f * (xx + zz); R[7] = 2.0f * (yz - wx);
    R[2] = 2.0f * (xz - wy);        R[5] = 2.0f * (yz + wx); R[8] = 1.0f - 2.0f * (xx + yy);
}

inline Quat quatSlerp(Quat a, Quat b, float t) {
    float dot = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    if (dot < 0.0f) {
        b.x = -b.x; b.y = -b.y; b.z = -b.z; b.w = -b.w;
        dot = -dot;
    }
    if (dot > 0.9995f) {
        Quat q{ a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t };
        float n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
        if (n > 1e-8f) { q.x /= n; q.y /= n; q.z /= n; q.w /= n; }
        return q;
    }
    if (dot > 1.0f) dot = 1.0f;
    float th = std::acos(dot);
    float s = std::sin(th);
    float w0 = std::sin((1.0f - t) * th) / s;
    float w1 = std::sin(t * th) / s;
    return { a.x * w0 + b.x * w1, a.y * w0 + b.y * w1, a.z * w0 + b.z * w1, a.w * w0 + b.w * w1 };
}

inline Quat quatMul(Quat a, Quat b) {
    return {
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z
    };
}

inline Quat quatConj(Quat a) { return { -a.x, -a.y, -a.z, a.w }; }

inline Quat axisAngleQuat(float x, float y, float z, float ang) {
    float h = ang * 0.5f;
    float s = std::sin(h);
    return { x * s, y * s, z * s, std::cos(h) };
}

// Rotate from `from` toward `to` by (θ + turns*360°) * t. θ is the short arc.
// turns = 0 is that short arc. A positive turn adds full revolutions the same way.
inline Vec3 lerpSpinEuler(const Vec3& from, const Vec3& to, float t, int turns) {
    float Ra[9], Rb[9], Ro[9];
    pm::eulerToMat(from, Ra);
    pm::eulerToMat(to, Rb);
    Quat qa = matToQuat(Ra);
    Quat qb = matToQuat(Rb);
    float dot = qa.x * qb.x + qa.y * qb.y + qa.z * qb.z + qa.w * qb.w;
    if (dot < 0.0f) {
        qb.x = -qb.x; qb.y = -qb.y; qb.z = -qb.z; qb.w = -qb.w;
    }
    Quat rel = quatMul(quatConj(qa), qb);
    float w = rel.w;
    if (w > 1.0f) w = 1.0f;
    if (w < -1.0f) w = -1.0f;
    float theta = 2.0f * std::acos(w);
    float s = std::sqrt(rel.x * rel.x + rel.y * rel.y + rel.z * rel.z);
    float ax = 1.0f, ay = 0.0f, az = 0.0f;
    if (s > 1e-6f) { ax = rel.x / s; ay = rel.y / s; az = rel.z / s; }
    float ang = (theta + 6.2831853f * (float)turns) * t;
    quatToMat(quatMul(qa, axisAngleQuat(ax, ay, az, ang)), Ro);
    return pm::matToEuler(Ro);
}

inline int setSpanSpin(Clip& c, int frameA, int frameB, int turns) {
    int lo = frameA < frameB ? frameA : frameB;
    int hi = frameA < frameB ? frameB : frameA;
    int n = 0;
    for (Track& tr : c.tracks) {
        if (!isPoseTrack(tr.bone)) continue;
        for (int i = 1; i < (int)tr.keys.size(); i++) {
            Key& cur = tr.keys[i];
            const Key& prev = tr.keys[i - 1];
            if (prev.frame < lo || cur.frame > hi || cur.frame <= prev.frame) continue;
            int use = turns;
            if (turns != 0) {
                float Ra[9], Rb[9];
                pm::eulerToMat(prev.r, Ra);
                pm::eulerToMat(cur.r, Rb);
                Quat qa = matToQuat(Ra);
                Quat qb = matToQuat(Rb);
                float dot = qa.x * qb.x + qa.y * qb.y + qa.z * qb.z + qa.w * qb.w;
                if (dot < 0.0f) dot = -dot;
                if (dot > 1.0f) dot = 1.0f;
                float theta = 2.0f * std::acos(dot);
                if (theta < 0.05f) use = 0;
            }
            cur.spinSet = true;
            cur.spin = use;
            n++;
        }
    }
    return n;
}

inline float wrapPi(float a) {
    const float pi = 3.14159265f;
    a = std::fmod(a + pi, pi * 2.0f);
    if (a < 0.0f) a += pi * 2.0f;
    return a - pi;
}

// Same orientation as R, written so each component stays near ref.
// Euler lerp then takes the short step instead of an extra turn.
inline Vec3 closestEuler(const float R[9], const Vec3& ref) {
    Vec3 a = pm::matToEuler(R);
    Vec3 b{ a.x + 3.14159265f, 3.14159265f - a.y, a.z + 3.14159265f };
    auto snap = [&](Vec3 e) {
        e.x = ref.x + wrapPi(e.x - ref.x);
        e.y = ref.y + wrapPi(e.y - ref.y);
        e.z = ref.z + wrapPi(e.z - ref.z);
        return e;
    };
    auto d2 = [&](const Vec3& u) {
        float dx = u.x - ref.x, dy = u.y - ref.y, dz = u.z - ref.z;
        return dx * dx + dy * dy + dz * dz;
    };
    Vec3 sa = snap(a), sb = snap(b);
    return d2(sa) <= d2(sb) ? sa : sb;
}

// Between two frames, rewrite the carrier bone so the held tool's world
// orientation follows the short arc. The poses at those two frames stay.
// Frames outside the span are left alone.
inline bool clearToolSpin(Clip& c, const std::string& carrier, int frameA, int frameB) {
    int bi = findBone(c, carrier);
    if (bi < 0) return false;
    int f0 = frameA < frameB ? frameA : frameB;
    int f1 = frameA < frameB ? frameB : frameA;
    if (f0 < 0) f0 = 0;
    int last = c.length > 0 ? c.length - 1 : 0;
    if (f1 > last) f1 = last;
    if (f1 <= f0 + 1) return false;

    Clip src = c;
    auto pose0 = evalPose(src, (float)f0);
    auto pose1 = evalPose(src, (float)f1);
    if (bi >= (int)pose0.size() || bi >= (int)pose1.size()) return false;
    Quat q0 = matToQuat(pose0[bi].R);
    Quat q1 = matToQuat(pose1[bi].R);
    Vec3 prev = evalBone(src, carrier, (float)f0).r;
    int parent = parentIndex(src, bi);
    bool wrote = false;
    for (int f = f0 + 1; f < f1; ++f) {
        float t = (float)(f - f0) / (float)(f1 - f0);
        float Rw[9];
        quatToMat(quatSlerp(q0, q1, t), Rw);
        auto pose = evalPose(src, (float)f);
        float local[9];
        if (parent < 0 || parent >= (int)pose.size()) {
            for (int n = 0; n < 9; n++) local[n] = Rw[n];
        } else {
            float Rt[9];
            transpose9(pose[parent].R, Rt);
            pm::mat3Mul(Rt, Rw, local);
        }
        Vec3 e = closestEuler(local, prev);
        Vec3 tr = evalBone(src, carrier, (float)f).t;
        setKey(c, carrier, f, tr, e);
        prev = e;
        wrote = true;
    }
    return wrote;
}

inline bool deleteKey(Clip& c, const std::string& bone, int frame) {
    int ti = findTrack(c, bone);
    if (ti < 0) return false;
    Track& tr = c.tracks[ti];
    for (int i = 0; i < (int)tr.keys.size(); i++) {
        if (tr.keys[i].frame == frame) {
            tr.keys.erase(tr.keys.begin() + i);
            if (tr.keys.empty()) c.tracks.erase(c.tracks.begin() + ti);
            return true;
        }
    }
    return false;
}

inline bool hasKey(const Clip& c, const std::string& bone, int frame) {
    int ti = findTrack(c, bone);
    if (ti < 0) return false;
    for (const Key& k : c.tracks[ti].keys)
        if (k.frame == frame) return true;
    return false;
}

inline int prevKeyFrame(const Clip& c, const std::string& bone, int frame) {
    int ti = findTrack(c, bone);
    int best = -1;
    auto consider = [&](const Track& tr) {
        for (const Key& k : tr.keys)
            if (k.frame < frame && k.frame > best) best = k.frame;
    };
    if (ti >= 0) consider(c.tracks[ti]);
    else for (const Track& tr : c.tracks) consider(tr);
    if (best < 0 && c.loop && c.length > 0) {
        int last = -1;
        auto lastOf = [&](const Track& tr) {
            for (const Key& k : tr.keys)
                if (k.frame > last) last = k.frame;
        };
        if (ti >= 0) lastOf(c.tracks[ti]);
        else for (const Track& tr : c.tracks) lastOf(tr);
        best = last;
    }
    return best;
}

inline int nextKeyFrame(const Clip& c, const std::string& bone, int frame) {
    int ti = findTrack(c, bone);
    int best = 1 << 30;
    auto consider = [&](const Track& tr) {
        for (const Key& k : tr.keys)
            if (k.frame > frame && k.frame < best) best = k.frame;
    };
    if (ti >= 0) consider(c.tracks[ti]);
    else for (const Track& tr : c.tracks) consider(tr);
    if (best == (1 << 30)) {
        if (c.loop) {
            int first = 1 << 30;
            auto firstOf = [&](const Track& tr) {
                for (const Key& k : tr.keys)
                    if (k.frame < first) first = k.frame;
            };
            if (ti >= 0) firstOf(c.tracks[ti]);
            else for (const Track& tr : c.tracks) firstOf(tr);
            if (first != (1 << 30)) return first;
        }
        return frame;
    }
    return best;
}

inline Clip makeWalk(const Clip& rig) {
    Clip c = rig;
    c.name = "walk";
    c.length = 20;
    c.fps = 20.0f;
    c.loop = true;
    c.tracks.clear();
    auto swing = [&](const char* bone, float amp, int phase) {
        if (findBone(c, bone) < 0) return;
        setKey(c, bone, 0, {}, { amp * (phase ? -1.0f : 1.0f), 0, 0 });
        setKey(c, bone, 10, {}, { amp * (phase ? 1.0f : -1.0f), 0, 0 });
        setKey(c, bone, 19, {}, { amp * (phase ? -1.0f : 1.0f), 0, 0 });
    };
    swing("arm_l_upper", 0.45f, 0);
    swing("arm_r_upper", 0.45f, 1);
    swing("leg_l_thigh", 0.40f, 1);
    swing("leg_r_thigh", 0.40f, 0);
    swing("arm_l_fore", 0.20f, 0);
    swing("arm_r_fore", 0.20f, 1);
    return c;
}

// Run cycle. Wider arm pump and a higher front knee than walk. Frames 0 and 10
// are the push: the back foot stays down, the front foot is off the ground.
inline Clip makeRun(const Clip& rig) {
    Clip c = rig;
    c.name = "run";
    c.length = 20;
    c.fps = 30.0f;
    c.loop = true;
    c.tracks.clear();
    auto pose = [&](const char* bone, int frame, float rx, float ty = 0.0f) {
        if (findBone(c, bone) < 0) return;
        setKey(c, bone, frame, { 0.0f, ty, 0.0f }, { rx, 0.0f, 0.0f });
    };
    pose("abs", 0, -0.12f);
    pose("hip", 0, 0.0f, -0.06f);
    pose("hip", 2, 0.0f, -0.09f);
    pose("hip", 5, 0.0f, -0.01f);
    pose("hip", 7, 0.0f, 0.03f);
    pose("hip", 8, 0.0f, 0.02f);
    pose("hip", 10, 0.0f, -0.06f);
    pose("hip", 12, 0.0f, -0.09f);
    pose("hip", 15, 0.0f, -0.01f);
    pose("hip", 17, 0.0f, 0.03f);
    pose("hip", 18, 0.0f, 0.02f);
    pose("hip", 19, 0.0f, -0.06f);
    pose("arm_l_shoulder", 0, 0.50f); pose("arm_l_shoulder", 10, -0.40f); pose("arm_l_shoulder", 19, 0.50f);
    pose("arm_r_shoulder", 0, -0.40f); pose("arm_r_shoulder", 10, 0.50f); pose("arm_r_shoulder", 19, -0.40f);
    pose("arm_l_upper", 0, 0.70f); pose("arm_l_upper", 10, -0.90f); pose("arm_l_upper", 19, 0.70f);
    pose("arm_r_upper", 0, -0.90f); pose("arm_r_upper", 10, 0.70f); pose("arm_r_upper", 19, -0.90f);
    pose("arm_l_fore", 0, -0.55f); pose("arm_l_fore", 10, -1.05f); pose("arm_l_fore", 19, -0.55f);
    pose("arm_r_fore", 0, -1.05f); pose("arm_r_fore", 10, -0.55f); pose("arm_r_fore", 19, -1.05f);
    pose("leg_l_thigh", 0, -1.15f); pose("leg_l_thigh", 5, -0.35f); pose("leg_l_thigh", 10, 0.55f);
    pose("leg_l_thigh", 15, 0.22f); pose("leg_l_thigh", 19, -1.15f);
    pose("leg_r_thigh", 0, 0.55f); pose("leg_r_thigh", 5, 0.22f); pose("leg_r_thigh", 10, -1.15f);
    pose("leg_r_thigh", 15, -0.35f); pose("leg_r_thigh", 19, 0.55f);
    pose("leg_l_shin", 0, 1.35f); pose("leg_l_shin", 5, 0.55f); pose("leg_l_shin", 10, 0.02f);
    pose("leg_l_shin", 15, 0.95f); pose("leg_l_shin", 19, 1.35f);
    pose("leg_r_shin", 0, 0.02f); pose("leg_r_shin", 5, 0.95f); pose("leg_r_shin", 10, 1.35f);
    pose("leg_r_shin", 15, 0.55f); pose("leg_r_shin", 19, 0.02f);
    pose("leg_l_foot", 0, -0.45f); pose("leg_l_foot", 5, -0.15f); pose("leg_l_foot", 10, 0.60f);
    pose("leg_l_foot", 15, 0.70f); pose("leg_l_foot", 19, -0.45f);
    pose("leg_r_foot", 0, 0.60f); pose("leg_r_foot", 5, 0.70f); pose("leg_r_foot", 10, -0.45f);
    pose("leg_r_foot", 15, -0.15f); pose("leg_r_foot", 19, 0.60f);
    return c;
}

inline Clip makeIdle(const Clip& rig) {
    Clip c = rig;
    c.name = "idle";
    c.length = 20;
    c.fps = 20.0f;
    c.loop = true;
    c.tracks.clear();
    if (!c.bones.empty())
        setKey(c, c.bones[0].name, 0, {}, {});
    return c;
}

inline Clip makeHoldBlock(const Clip& rig) {
    Clip c = rig;
    c.name = "hold_block";
    c.length = 20;
    c.fps = 20.0f;
    c.loop = true;
    c.tracks.clear();
    auto pose = [&](const char* bone, float rx, float ry, float rz) {
        if (findBone(c, bone) < 0) return;
        setKey(c, bone, 0, {}, { rx, ry, rz });
    };
    // Rx-only on the arm chain: raise the upper a little, bend the elbow so
    // both hands sit on the sides of a cube in front of the chest. Any Ry/Rz
    // twists the boxy arm meshes and drives them through the torso.
    pose("arm_l_shoulder", 0.00f, 0.00f, 0.00f);
    pose("arm_r_shoulder", 0.00f, 0.00f, 0.00f);
    pose("arm_l_upper", -0.42f, 0.00f, 0.00f);
    pose("arm_r_upper", -0.42f, 0.00f, 0.00f);
    pose("arm_l_fore", -1.48f, 0.00f, 0.00f);
    pose("arm_r_fore", -1.48f, 0.00f, 0.00f);
    pose("arm_l_hand", 0.00f, 0.00f, 0.00f);
    pose("arm_r_hand", 0.00f, 0.00f, 0.00f);
    return c;
}

inline Clip makeTuckR(const Clip& rig) {
    Clip c = rig;
    c.name = "tuck_r";
    c.length = 20;
    c.fps = 20.0f;
    c.loop = true;
    c.tracks.clear();
    auto pose = [&](const char* bone, float rx, float ry, float rz) {
        if (findBone(c, bone) < 0) return;
        setKey(c, bone, 0, {}, { rx, ry, rz });
    };
    // Rx-only: the palm's inner face stays a YZ plane, flush with the block's
    // outer face, while the block's back face sits on the chest/abs front.
    pose("arm_r_shoulder", 0.00f, 0.00f, 0.00f);
    pose("arm_r_upper", -0.22f, 0.00f, 0.00f);
    pose("arm_r_fore", -1.72f, 0.00f, 0.00f);
    pose("arm_r_hand", 0.00f, 0.00f, 0.00f);
    return c;
}

inline Clip makeTuckL(const Clip& rig) {
    Clip c = rig;
    c.name = "tuck_l";
    c.length = 20;
    c.fps = 20.0f;
    c.loop = true;
    c.tracks.clear();
    auto pose = [&](const char* bone, float rx, float ry, float rz) {
        if (findBone(c, bone) < 0) return;
        setKey(c, bone, 0, {}, { rx, ry, rz });
    };
    pose("arm_l_shoulder", 0.00f, 0.00f, 0.00f);
    pose("arm_l_upper", -0.22f, 0.00f, 0.00f);
    pose("arm_l_fore", -1.72f, 0.00f, 0.00f);
    pose("arm_l_hand", 0.00f, 0.00f, 0.00f);
    return c;
}

// Right-arm punch. Shoulder is locked so a walk cycle cannot twist the jab.
// Frame 10 (of length 20) is the hit: fist extended forward. Windup cocks the
// fist beside the chest; recovery lowers the arm. Rx only, same as hold poses.
inline Clip makePunch(const Clip& rig) {
    Clip c = rig;
    c.name = "punch";
    c.length = 20;
    c.fps = 20.0f;
    c.loop = false;
    c.tracks.clear();
    auto pose = [&](const char* bone, int frame, float rx, float ry, float rz) {
        if (findBone(c, bone) < 0) return;
        setKey(c, bone, frame, {}, { rx, ry, rz });
    };
    pose("arm_r_shoulder", 0, 0.00f, 0.00f, 0.00f);
    pose("arm_r_hand", 0, 0.00f, 0.00f, 0.00f);
    pose("arm_r_upper", 0, 0.00f, 0.00f, 0.00f);
    pose("arm_r_upper", 3, -0.30f, 0.00f, 0.00f);
    pose("arm_r_upper", 8, -0.50f, 0.00f, 0.00f);
    pose("arm_r_upper", 10, -1.25f, 0.00f, 0.00f);
    pose("arm_r_upper", 13, -0.55f, 0.00f, 0.00f);
    pose("arm_r_upper", 19, 0.00f, 0.00f, 0.00f);
    pose("arm_r_fore", 0, 0.00f, 0.00f, 0.00f);
    pose("arm_r_fore", 3, -2.20f, 0.00f, 0.00f);
    pose("arm_r_fore", 8, -2.35f, 0.00f, 0.00f);
    pose("arm_r_fore", 10, -0.12f, 0.00f, 0.00f);
    pose("arm_r_fore", 13, -0.20f, 0.00f, 0.00f);
    pose("arm_r_fore", 19, 0.00f, 0.00f, 0.00f);
    return c;
}

// Right-arm overhead chop. One frame slot is 10ms. Frames 0–80 raise up the
// front (negative Rx). Frames 80–100 chop back down the front; keys close up
// and each step is larger. Frame 100 is the hit. Frames 100–112 settle.
inline Clip makeAxeChop(const Clip& rig) {
    Clip c = rig;
    c.name = "axe_chop";
    c.length = 113;
    c.fps = 100.0f;
    c.loop = false;
    c.tracks.clear();
    auto pose = [&](const char* bone, int frame, float rx, float ry, float rz) {
        if (findBone(c, bone) < 0) return;
        setKey(c, bone, frame, {}, { rx, ry, rz });
    };
    pose("arm_r_shoulder", 0, 0.00f, 0.00f, 0.00f);
    pose("arm_r_upper", 0, 0.00f, 0.00f, 0.00f);
    pose("arm_r_upper", 30, -1.05f, 0.00f, -0.30f);
    pose("arm_r_upper", 55, -2.10f, 0.00f, -0.60f);
    pose("arm_r_upper", 80, -2.90f, 0.00f, -0.95f);
    pose("arm_r_upper", 90, -2.85416f, 0.00f, -0.92344f);
    pose("arm_r_upper", 95, -2.55192f, 0.00f, -0.74829f);
    pose("arm_r_upper", 98, -2.03386f, 0.00f, -0.44808f);
    pose("arm_r_upper", 99, -1.76501f, 0.00f, -0.29229f);
    pose("arm_r_upper", 100, -1.43319f, 0.00f, -0.10f);
    pose("arm_r_upper", 104, -0.64494f, 0.00f, -0.04500f);
    pose("arm_r_upper", 108, -0.21498f, 0.00f, -0.01500f);
    pose("arm_r_upper", 112, 0.00f, 0.00f, 0.00f);
    pose("arm_r_fore", 0, -0.10f, 0.00f, 0.00f);
    pose("arm_r_fore", 30, -0.20f, 0.00f, 0.00f);
    pose("arm_r_fore", 55, -0.32f, 0.00f, 0.00f);
    pose("arm_r_fore", 80, -0.35f, 0.00f, 0.00f);
    pose("arm_r_fore", 90, -0.35156f, 0.00f, 0.00f);
    pose("arm_r_fore", 95, -0.36187f, 0.00f, 0.00f);
    pose("arm_r_fore", 98, -0.37952f, 0.00f, 0.00f);
    pose("arm_r_fore", 99, -0.38869f, 0.00f, 0.00f);
    pose("arm_r_fore", 100, -0.40f, 0.00f, 0.00f);
    pose("arm_r_fore", 104, -0.18000f, 0.00f, 0.00f);
    pose("arm_r_fore", 108, -0.06000f, 0.00f, 0.00f);
    pose("arm_r_fore", 112, 0.00f, 0.00f, 0.00f);
    pose("arm_r_hand", 0, 0.00f, 0.00f, 0.00f);
    pose("arm_r_hand", 30, 0.10f, 0.00f, 0.00f);
    pose("arm_r_hand", 55, 0.22f, 0.00f, 0.00f);
    pose("arm_r_hand", 80, 0.40f, 0.00f, 0.00f);
    pose("arm_r_hand", 90, 0.36719f, 0.00f, 0.00f);
    pose("arm_r_hand", 95, 0.15083f, 0.00f, 0.00f);
    pose("arm_r_hand", 98, -0.22001f, 0.00f, 0.00f);
    pose("arm_r_hand", 99, -0.41247f, 0.00f, 0.00f);
    pose("arm_r_hand", 100, -0.65f, 0.00f, 0.00f);
    pose("arm_r_hand", 104, -0.29250f, 0.00f, 0.00f);
    pose("arm_r_hand", 108, -0.09750f, 0.00f, 0.00f);
    pose("arm_r_hand", 112, 0.00f, 0.00f, 0.00f);
    return c;
}

// Generic two-hand ready. The main arm keeps the carry pose: it stays in the
// side plane. The wrist turns the head toward the main-hand side and the shaft
// toward the off-hand side, at the chest-abdomen seam. The off-hand elbow
// points outward and the forearm reaches up toward the tail. One slot is 10ms.
// Frame 40 is the pose.
inline Clip makeTwoHandReady(const Clip& rig) {
    Clip c = rig;
    c.name = "two_hand_ready";
    c.length = 41;
    c.fps = 100.0f;
    c.loop = false;
    c.tracks.clear();
    auto pose = [&](const char* bone, int frame, float rx, float ry = 0.0f, float rz = 0.0f) {
        if (findBone(c, bone) < 0) return;
        setKey(c, bone, frame, {}, { rx, ry, rz });
    };
    pose("arm_r_shoulder", 0, 0.013f);
    pose("arm_r_upper", 0, -0.900f, 0.0f, 0.729f);
    pose("arm_r_fore", 0, -1.581f, 0.258f, 0.0f);
    pose("arm_r_hand", 0, -1.162f, 0.0f, 0.077f);
    pose("arm_l_shoulder", 0, 0);
    pose("arm_l_upper", 0, 0);
    pose("arm_l_fore", 0, 0);
    pose("arm_l_hand", 0, 0);
    pose("arm_r_shoulder", 40, 0.000f);
    pose("arm_r_upper", 40, -0.420f, 0.0f, 0.000f);
    pose("arm_r_fore", 40, -1.480f, 0.000f, 0.0f);
    pose("arm_r_hand", 40, 1.34206f, 1.31233f, 1.13218f);
    pose("arm_l_shoulder", 40, -0.475f);
    pose("arm_l_upper", 40, -0.07311f, 0.06145f, -0.260f);
    pose("arm_l_fore", 40, -1.46172f, 0.89883f, -0.02199f);
    pose("arm_l_hand", 40, -0.32554f, 0.34864f, 0.71824f);
    return c;
}

// Ready pose up to the overhead raise. Frame 0 is the two_hand_ready end pose.
// Frame 40 is the overhead the dig cycle starts from. One slot is 10ms.
inline Clip makePickRaise(const Clip& rig) {
    Clip c = rig;
    c.name = "pick_raise";
    c.length = 41;
    c.fps = 100.0f;
    c.loop = false;
    c.tracks.clear();
    auto pose = [&](const char* bone, int frame, float rx, float ry = 0.0f, float rz = 0.0f) {
        if (findBone(c, bone) < 0) return;
        setKey(c, bone, frame, {}, { rx, ry, rz });
    };
    pose("arm_r_shoulder", 0, 0.000f);
    pose("arm_r_upper", 0, -0.420f);
    pose("arm_r_fore", 0, -1.480f);
    pose("arm_r_hand", 0, 1.34206f, 1.31233f, 1.13218f);
    pose("arm_l_shoulder", 0, -0.475f);
    pose("arm_l_upper", 0, -0.07311f, 0.06145f, -0.260f);
    pose("arm_l_fore", 0, -1.46172f, 0.89883f, -0.02199f);
    pose("arm_l_hand", 0, -0.32554f, 0.34864f, 0.71824f);
    setKey(c, kGripTrack, 0, { 0.500f, 0.220f, 0.500f }, {});
    pose("arm_r_shoulder", 40, -0.62800f, 0.43703f, 0.00000f);
    pose("arm_r_upper", 40, -1.29903f, -0.44290f, 0.32074f);
    pose("arm_r_fore", 40, -1.18445f, -0.31516f, 0.82270f);
    pose("arm_r_hand", 40, 2.49311f, 0.31756f, 2.08029f);
    pose("arm_l_shoulder", 40, -0.24672f, -0.49446f, 0.36800f);
    pose("arm_l_upper", 40, -1.69766f, 0.51422f, -0.71953f);
    pose("arm_l_fore", 40, -1.15593f, 0.83072f, -0.93223f);
    pose("arm_l_hand", 40, -0.58494f, -0.32860f, -0.04725f);
    setKey(c, kGripTrack, 40, { 0.500f, 0.220f, 0.500f }, {});
    setKey(c, kTouchTrack, 0, {}, {});
    setKey(c, kTouchTrack, 40, {}, {});
    return c;
}

// Chop from the overhead raise (frame 0) to the hit (frame 20). One slot is 10ms.
inline Clip makeMineDown(const Clip& rig) {
    Clip c = rig;
    c.name = "mine_down";
    c.length = 21;
    c.fps = 100.0f;
    c.loop = false;
    auto pose = [&](const char* bone, int frame, float rx, float ry = 0.0f, float rz = 0.0f) {
        if (findBone(c, bone) < 0) return;
        setKey(c, bone, frame, {}, { rx, ry, rz });
    };
    auto both = [&](int frame,
                    float rsh, float rur, float ruy, float ruz, float rfr, float rfry, float rfrz, float rha, float rhay, float rhz,
                    float lsh, float lur, float luy, float luz, float lfr, float lfry, float lfrz, float lha, float lhay, float lhz) {
        pose("arm_r_shoulder", frame, rsh);
        pose("arm_r_upper", frame, rur, ruy, ruz);
        pose("arm_r_fore", frame, rfr, rfry, rfrz);
        pose("arm_r_hand", frame, rha, rhay, rhz);
        pose("arm_l_shoulder", frame, lsh);
        pose("arm_l_upper", frame, lur, luy, luz);
        pose("arm_l_fore", frame, lfr, lfry, lfrz);
        pose("arm_l_hand", frame, lha, lhay, lhz);
    };
    both(0, -0.628f, -1.781f, 0.002f, 0.661f, -0.239f, -0.405f, 0.076f, -0.227f, -0.064f, 0.080f,
         -0.06831f, -1.54995f, 0.16668f, -0.69301f, -1.22500f, 0.47779f, -0.12384f, -0.58839f, -0.33010f, -0.04575f);
    both(10, -0.522f, -1.67650f, 0.00175f, 0.516f, -0.21562f, -0.33988f, 0.06650f, -0.22787f, -0.056f, 0.04650f,
         -0.066f, -1.30976f, 0.51357f, -0.07514f, -1.00206f, 0.21851f, 0.20985f, -0.67141f, 0.35461f, 0.50013f);
    both(15, -0.27025f, -1.42831f, 0.00116f, 0.17162f, -0.16011f, -0.18520f, 0.04394f, -0.22995f, -0.037f, -0.03306f,
         -0.75593f, -1.48576f, 0.36010f, -0.72417f, -1.19586f, 0.69044f, 0.29202f, -0.25829f, -0.19283f, -0.52630f);
    both(18, -0.00981f, -1.17156f, 0.00054f, -0.18464f, -0.10268f, -0.02519f, 0.02060f, -0.23210f, -0.01734f, -0.11537f,
         -0.36120f, -2.27202f, 0.38940f, -0.93251f, -0.05350f, -0.31338f, 0.36791f, -0.36449f, 0.23315f, -0.04836f);
    both(20, 0.220f, -0.945f, 0.0f, -0.499f, -0.052f, 0.116f, 0.0f, -0.234f, 0.0f, -0.188f,
         -0.11403f, -2.08720f, 0.34492f, -1.07349f, -0.45579f, -0.07712f, 0.42877f, -0.47280f, 0.23784f, -0.35870f);
    setKey(c, kGripTrack, 0, { 0.500f, 0.220f, 0.500f }, {});
    for (int frame : { 0, 10, 15, 18, 20 })
        setKey(c, kTouchTrack, frame, {}, {});
    return c;
}

// Lift from the hit (frame 0) back to the overhead raise (frame 40).
inline Clip makeMineUp(const Clip& rig) {
    Clip c = rig;
    c.name = "mine_up";
    c.length = 41;
    c.fps = 100.0f;
    c.loop = false;
    auto pose = [&](const char* bone, int frame, float rx, float ry = 0.0f, float rz = 0.0f) {
        if (findBone(c, bone) < 0) return;
        setKey(c, bone, frame, {}, { rx, ry, rz });
    };
    auto both = [&](int frame,
                    float rsh, float rur, float ruy, float ruz, float rfr, float rfry, float rfrz, float rha, float rhay, float rhz,
                    float lsh, float lur, float luy, float luz, float lfr, float lfry, float lfrz, float lha, float lhay, float lhz) {
        pose("arm_r_shoulder", frame, rsh);
        pose("arm_r_upper", frame, rur, ruy, ruz);
        pose("arm_r_fore", frame, rfr, rfry, rfrz);
        pose("arm_r_hand", frame, rha, rhay, rhz);
        pose("arm_l_shoulder", frame, lsh);
        pose("arm_l_upper", frame, lur, luy, luz);
        pose("arm_l_fore", frame, lfr, lfry, lfrz);
        pose("arm_l_hand", frame, lha, lhay, lhz);
    };
    both(0, 0.220f, -0.945f, 0.0f, -0.499f, -0.052f, 0.116f, 0.0f, -0.234f, 0.0f, -0.188f,
         -0.11403f, -2.08720f, 0.34492f, -1.07349f, -0.45579f, -0.07712f, 0.42877f, -0.47280f, 0.23784f, -0.35870f);
    both(20, -0.204f, -1.363f, 0.001f, 0.081f, -0.14550f, -0.14450f, 0.038f, -0.23050f, -0.032f, -0.054f,
         -0.77801f, -1.59512f, 0.40064f, -0.86288f, -0.93246f, 0.33636f, 0.41334f, -0.65900f, 0.37642f, 0.38455f);
    both(40, -0.628f, -1.781f, 0.002f, 0.661f, -0.239f, -0.405f, 0.076f, -0.227f, -0.064f, 0.080f,
         -0.06831f, -1.54995f, 0.16668f, -0.69301f, -1.22500f, 0.47779f, -0.12384f, -0.58839f, -0.33010f, -0.04575f);
    setKey(c, kGripTrack, 0, { 0.500f, 0.220f, 0.500f }, {});
    for (int frame : { 0, 20, 40 })
        setKey(c, kTouchTrack, frame, {}, {});
    return c;
}

inline Clip load(const char* path) {
    Clip c;
    FILE* f = std::fopen(path, "rb");
    if (!f) return c;
    char line[1024];
    while (std::fgets(line, sizeof(line), f)) {
        std::vector<std::string> t = pm::splitWs(line);
        if (t.empty() || t[0].empty() || t[0][0] == '#') continue;
        auto keep = [&]() {
            std::string raw = line;
            while (!raw.empty() && (raw.back() == '\n' || raw.back() == '\r')) raw.pop_back();
            if (!raw.empty()) c.extraLines.push_back(raw);
        };
        if (t[0] == "name" && t.size() >= 2) { c.name = t[1]; continue; }
        if (t[0] == "fps" && t.size() >= 2) { c.fps = (float)std::atof(t[1].c_str()); continue; }
        if (t[0] == "length" && t.size() >= 2) { c.length = std::atoi(t[1].c_str()); continue; }
        if (t[0] == "loop" && t.size() >= 2) { c.loop = std::atoi(t[1].c_str()) != 0; continue; }
        if (t[0] == "rig" && t.size() >= 2) { c.rig = t[1]; continue; }
        if (t[0] == "toolspin" && t.size() >= 5) {
            Clip::ToolTurn s;
            s.frameA = std::atoi(t[1].c_str());
            s.frameB = std::atoi(t[2].c_str());
            s.sense = std::atoi(t[3].c_str()) < 0 ? -1 : 1;
            s.laps = std::atoi(t[4].c_str());
            if (s.laps < 0) s.laps = 0;
            c.toolTurns.push_back(s);
            continue;
        }
        if (t[0] == "bone" && t.size() >= 2) {
            Bone b;
            b.name = t[1];
            size_t i = 2;
            if (i < t.size() && t[i] != "parent" && t[i] != "pivot") {
                if (t[i] != "-") b.parent = t[i];
                i++;
            }
            while (i < t.size()) {
                if (t[i] == "parent" && i + 1 < t.size()) {
                    if (t[i + 1] != "-") b.parent = t[i + 1];
                    i += 2;
                } else if (t[i] == "pivot" && i + 3 < t.size()) {
                    b.pivot.x = (float)std::atof(t[i + 1].c_str());
                    b.pivot.y = (float)std::atof(t[i + 2].c_str());
                    b.pivot.z = (float)std::atof(t[i + 3].c_str());
                    i += 4;
                } else i++;
            }
            if (findBone(c, b.name) < 0) c.bones.push_back(std::move(b));
            continue;
        }
        if (t[0] == "key" && t.size() >= 9) {
            Key k;
            k.frame = std::atoi(t[1].c_str());
            k.t.x = (float)std::atof(t[3].c_str());
            k.t.y = (float)std::atof(t[4].c_str());
            k.t.z = (float)std::atof(t[5].c_str());
            k.r.x = (float)std::atof(t[6].c_str());
            k.r.y = (float)std::atof(t[7].c_str());
            k.r.z = (float)std::atof(t[8].c_str());
            setKey(c, t[2], k.frame, k.t, k.r);
            if (t.size() >= 11 && t[9] == "spin") {
                int ti = findTrack(c, t[2]);
                if (ti >= 0) {
                    for (Key& key : c.tracks[ti].keys) {
                        if (key.frame == k.frame) {
                            key.spinSet = true;
                            key.spin = std::atoi(t[10].c_str());
                            break;
                        }
                    }
                }
            }
            continue;
        }
        keep();
    }
    std::fclose(f);
    if (c.length < 1) c.length = 1;
    if (c.fps < 1.0f) c.fps = 1.0f;
    return c;
}

inline bool save(const char* path, const Clip& c) {
    FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    std::fprintf(f, "# animation clip: a keyed frame is one pose of the whole model.\n");
    std::fprintf(f, "# values are that pose split into each bone's local TRS. Unknown records round-trip.\n");
    std::fprintf(f, "name %s\n", c.name.c_str());
    std::fprintf(f, "fps %.3f\n", c.fps);
    std::fprintf(f, "length %d\n", c.length);
    std::fprintf(f, "loop %d\n", c.loop ? 1 : 0);
    if (!c.rig.empty()) std::fprintf(f, "rig %s\n", c.rig.c_str());
    for (const Clip::ToolTurn& s : c.toolTurns)
        std::fprintf(f, "toolspin %d %d %d %d\n", s.frameA, s.frameB, s.sense < 0 ? -1 : 1, s.laps < 0 ? 0 : s.laps);
    for (const std::string& s : c.extraLines) std::fprintf(f, "%s\n", s.c_str());
    for (const Bone& b : c.bones) {
        std::fprintf(f, "bone %s parent %s pivot %.4f %.4f %.4f\n",
                     b.name.c_str(), b.parent.empty() ? "-" : b.parent.c_str(),
                     b.pivot.x, b.pivot.y, b.pivot.z);
    }
    for (const Track& tr : c.tracks) {
        for (const Key& k : tr.keys) {
            std::fprintf(f, "key %d %s %.5f %.5f %.5f %.5f %.5f %.5f",
                         k.frame, tr.bone.c_str(),
                         k.t.x, k.t.y, k.t.z, k.r.x, k.r.y, k.r.z);
            if (k.spinSet) std::fprintf(f, " spin %d", k.spin);
            std::fprintf(f, "\n");
        }
    }
    std::fclose(f);
    return true;
}

struct Playback {
    std::string name = "idle";
    float clock = 0.0f;
};

inline void advance(Playback& pb, const Clip& c, float dt, float scale = 1.0f) {
    if (c.name != pb.name) {
        pb.name = c.name.empty() ? "clip" : c.name;
        pb.clock = 0.0f;
    }
    float fps = (c.fps > 0.1f) ? c.fps : 20.0f;
    int len = (c.length > 0) ? c.length : 20;
    float spd = (scale > 0.01f) ? scale : 0.01f;
    pb.clock += dt * fps * spd;
    float L = (float)len;
    if (c.loop) {
        if (L < 1.0f) L = 1.0f;
        while (pb.clock >= L) pb.clock -= L;
        while (pb.clock < 0.0f) pb.clock += L;
    } else if (pb.clock > L - 1.0f) {
        pb.clock = L - 1.0f;
    }
}

// Strike overlay keeps the arms and the grip. Body and leg tracks stay on the
// walk cycle; a whole-pose ready clip would otherwise lock them to identity.
inline Clip armStrikeClip(const Clip& src) {
    Clip c = src;
    std::vector<Track> keep;
    keep.reserve(c.tracks.size());
    for (const Track& tr : c.tracks) {
        if (tr.bone == kGripTrack || tr.bone == kTouchTrack || tr.bone == kFaceTrack
            || tr.bone.rfind("arm_", 0) == 0)
            keep.push_back(tr);
    }
    c.tracks.swap(keep);
    return c;
}

struct PlayerClips {
    Clip idle, walk, run;
    Clip holdBlock, tuckR, tuckL;
    Clip punch, axeChop, mineDown, mineUp, pickRaise, twoHandReady;
    Clip twoHandStrike; // two_hand_ready with only the arm and grip tracks
    std::vector<pm::Part> rest;
};

inline PlayerClips& playerClips() {
    static PlayerClips lib;
    static bool once = false;
    if (!once) {
        once = true;
        lib.rest = pm::buildPlayerModel();
        Clip rig = rigFromParts(lib.rest, "player");
        auto loadOr = [&](Clip& dst, const char* stem, Clip (*make)(const Clip&)) {
            dst = load(pack::animationFile(stem).c_str());
            if (dst.bones.empty() || dst.tracks.empty()) dst = make(rig);
            if (dst.bones.empty()) dst.bones = rig.bones;
            if (dst.name.empty()) dst.name = stem;
            rebindPivots(dst, lib.rest);
        };
        loadOr(lib.walk, "walk", makeWalk);
        loadOr(lib.run, "run", makeRun);
        loadOr(lib.idle, "idle", makeIdle);
        loadOr(lib.holdBlock, "hold_block", makeHoldBlock);
        loadOr(lib.tuckR, "tuck_r", makeTuckR);
        loadOr(lib.tuckL, "tuck_l", makeTuckL);
        loadOr(lib.punch, "punch", makePunch);
        loadOr(lib.axeChop, "axe_chop", makeAxeChop);
        loadOr(lib.mineDown, "mine_down", makeMineDown);
        loadOr(lib.mineUp, "mine_up", makeMineUp);
        loadOr(lib.pickRaise, "pick_raise", makePickRaise);
        loadOr(lib.twoHandReady, "two_hand_ready", makeTwoHandReady);
        // Whole-pose stamps in the raise file also key the body at identity.
        // The strike overlay must keep only the arms, or the walk freezes.
        lib.pickRaise = armStrikeClip(lib.pickRaise);
        lib.twoHandStrike = armStrikeClip(lib.twoHandReady);
        if (lib.walk.name.empty()) lib.walk.name = "walk";
        if (lib.run.name.empty()) lib.run.name = "run";
        if (lib.idle.name.empty()) lib.idle.name = "idle";
        if (lib.holdBlock.name.empty()) lib.holdBlock.name = "hold_block";
        if (lib.tuckR.name.empty()) lib.tuckR.name = "tuck_r";
        if (lib.tuckL.name.empty()) lib.tuckL.name = "tuck_l";
        if (lib.punch.name.empty()) lib.punch.name = "punch";
        if (lib.axeChop.name.empty()) lib.axeChop.name = "axe_chop";
        if (lib.mineDown.name.empty()) lib.mineDown.name = "mine_down";
        if (lib.mineUp.name.empty()) lib.mineUp.name = "mine_up";
        if (lib.pickRaise.name.empty()) lib.pickRaise.name = "pick_raise";
        if (lib.twoHandReady.name.empty()) lib.twoHandReady.name = "two_hand_ready";
        if (lib.twoHandStrike.name.empty()) lib.twoHandStrike.name = "two_hand_ready";
    }
    return lib;
}

inline void reloadPlayerModel() {
    PlayerClips& lib = playerClips();
    lib.rest = pm::buildPlayerModel();
    Clip* clips[] = {
        &lib.idle, &lib.walk, &lib.run, &lib.holdBlock, &lib.tuckR, &lib.tuckL,
        &lib.punch, &lib.axeChop, &lib.mineDown, &lib.mineUp, &lib.pickRaise,
        &lib.twoHandReady, &lib.twoHandStrike
    };
    for (Clip* c : clips) rebindPivots(*c, lib.rest);
}

inline const Clip& clipByName(const std::string& name) {
    PlayerClips& L = playerClips();
    if (name == "walk") return L.walk;
    if (name == "run") return L.run;
    if (name == "hold_block") return L.holdBlock;
    if (name == "tuck_r") return L.tuckR;
    if (name == "tuck_l") return L.tuckL;
    return L.idle;
}

inline const Clip* holdOverlayClip(const std::string& name) {
    PlayerClips& L = playerClips();
    if (name == "hold_block") return &L.holdBlock;
    if (name == "tuck_r") return &L.tuckR;
    if (name == "tuck_l") return &L.tuckL;
    return nullptr;
}

// Right-hand strike overlays. Empty / unknown returns null (no overlay).
inline const Clip* actionClip(const std::string& name) {
    if (name.empty()) return nullptr;
    PlayerClips& L = playerClips();
    if (name == "punch" || name == L.punch.name) return &L.punch;
    if (name == "axe_chop" || name == L.axeChop.name) return &L.axeChop;
    if (name == "mine_down" || name == L.mineDown.name) return &L.mineDown;
    if (name == "mine_up" || name == L.mineUp.name) return &L.mineUp;
    if (name == "pick_mine") return &L.mineDown;
    if (name == "pick_raise" || name == L.pickRaise.name) return &L.pickRaise;
    if (name == "two_hand_ready" || name == L.twoHandReady.name) return &L.twoHandReady;
    return nullptr;
}

// Axe keys sit on 10ms frame slots. The gap between two keys is that many
// slots, so spacing sets the duration and the pose change per slot sets the
// speed. Playback does not stretch the clip across the charge or the cooldown.
inline float axeChopSample(const Clip& c, float chargeDur, float coolDur,
                           float charge, float cool) {
    const float slot = 0.010f;
    float end = (c.length > 1) ? (float)(c.length - 1) : 1.0f;
    if (chargeDur < 1e-3f) chargeDur = 1.0f;
    if (coolDur < 1e-3f) coolDur = 1.0f;
    const bool inCharge = charge > 1e-4f || cool <= 1e-4f;
    float elapsed = charge;
    if (!inCharge) {
        float spent = coolDur - cool;
        if (spent < 0.0f) spent = 0.0f;
        elapsed = chargeDur + spent;
    }
    float frame = elapsed / slot;
    if (frame < 0.0f) frame = 0.0f;
    if (frame > end) frame = end;
    return frame;
}

// Clip length is in 10ms slots. The span is the time from the first frame to the last.
inline float clipSpanSec(const Clip& c) {
    int n = (c.length > 1) ? (c.length - 1) : 1;
    return (float)n * 0.010f;
}

inline float slotFrame(const Clip& c, float elapsed) {
    float end = (c.length > 1) ? (float)(c.length - 1) : 0.0f;
    float frame = elapsed / 0.010f;
    if (frame < 0.0f) frame = 0.0f;
    if (frame > end) frame = end;
    return frame;
}

// First pick swing pays for the ready and the raise, then the chop.
// A follow-up only chops, and the lift back is the recovery after each hit.
inline float axeChopSec() { return clipSpanSec(playerClips().axeChop); }
inline float pickReadySec() { return clipSpanSec(playerClips().twoHandStrike); }
inline float pickRaiseSec() { return clipSpanSec(playerClips().pickRaise); }
inline float pickDownSec() { return clipSpanSec(playerClips().mineDown); }
inline float pickUpSec() { return clipSpanSec(playerClips().mineUp); }
inline float pickFirstSec() { return pickReadySec() + pickRaiseSec() + pickDownSec(); }

struct StrikeView {
    const Clip* clip = nullptr;
    float frame = 0.0f;
};

// First swing: two_hand_ready, pick_raise, then mine_down. The hit is the last
// frame of mine_down. After the hit, mine_up plays for its own length. A held
// button then plays mine_down again. Each clip starts at frame 0.
inline StrikeView pickStrikeView(float /*chargeDur*/, float /*coolDur*/,
                                 float charge, float cool, bool primed) {
    PlayerClips& L = playerClips();
    StrikeView v;
    const bool charging = charge > 1e-4f;
    if (!primed && charging) {
        const float ready = pickReadySec();
        const float raise = pickRaiseSec();
        if (charge < ready) {
            v.clip = &L.twoHandStrike;
            v.frame = slotFrame(L.twoHandStrike, charge);
        } else if (charge < ready + raise) {
            v.clip = &L.pickRaise;
            v.frame = slotFrame(L.pickRaise, charge - ready);
        } else {
            v.clip = &L.mineDown;
            v.frame = slotFrame(L.mineDown, charge - ready - raise);
        }
        return v;
    }
    if (primed && charging) {
        v.clip = &L.mineDown;
        v.frame = slotFrame(L.mineDown, charge);
        return v;
    }
    float spent = pickUpSec() - cool;
    if (spent < 0.0f) spent = 0.0f;
    v.clip = &L.mineUp;
    v.frame = slotFrame(L.mineUp, spent);
    return v;
}

// Map the mine clock onto a strike clip. Punch hits on frame length/2: charge
// fills 0→hit, cooldown fills hit→end. The axe and the pick play one 10ms
// slot per frame. primed is the pick's follow-up swing (already raised).
inline float strikeSample(const Clip& c, float chargeDur, float coolDur,
                          float charge, float cool, bool primed = false) {
    if (c.name == "axe_chop")
        return axeChopSample(c, chargeDur, coolDur, charge, cool);
    float end = (c.length > 1) ? (float)(c.length - 1) : 1.0f;
    float impact = (c.length > 1) ? (float)(c.length / 2) : 1.0f;
    if (impact > end) impact = end;
    auto sat = [](float u) {
        if (u < 0.0f) return 0.0f;
        if (u > 1.0f) return 1.0f;
        return u;
    };
    if (charge > 1e-4f || cool <= 1e-4f) {
        float dur = (chargeDur > 1e-3f) ? chargeDur : 0.5f;
        return impact * sat(charge / dur);
    }
    float dur = (coolDur > 1e-3f) ? coolDur : 0.5f;
    return impact + (end - impact) * sat(1.0f - cool / dur);
}

// Pick swings switch clips: ready, then raise, then the dig cycle. Other
// strikes stay on the clip named by the swing.
inline StrikeView resolveStrike(const std::string& name, float chargeDur, float coolDur,
                                float charge, float cool, bool primed) {
    if (name == "pick_mine")
        return pickStrikeView(chargeDur, coolDur, charge, cool, primed);
    StrikeView v;
    v.clip = actionClip(name);
    if (v.clip)
        v.frame = strikeSample(*v.clip, chargeDur, coolDur, charge, cool, primed);
    return v;
}

} // namespace anim
