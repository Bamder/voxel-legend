#pragma once
#include <cstdio>
#include <string>

// Active resource pack. All model/skin/material files resolve through here so
// a custom pack is a root change (or extra search dirs later), not new C++
// identity tables. Unknown files are simply missing; unknown fields stay in
// the .model / .rand parsers.
namespace pack {

inline std::string& root() {
    static std::string r = "assets";
    return r;
}

inline std::string join(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    char c = a.back();
    if (c == '/' || c == '\\') return a + b;
    return a + "/" + b;
}

inline bool hasPngExt(const std::string& s) {
    if (s.size() < 4) return false;
    char a = s[s.size() - 4], b = s[s.size() - 3];
    char c = s[s.size() - 2], d = s[s.size() - 1];
    return a == '.' && (b == 'p' || b == 'P') && (c == 'n' || c == 'N') && (d == 'g' || d == 'G');
}

inline std::string withPng(const std::string& stem) {
    return hasPngExt(stem) ? stem : stem + ".png";
}

inline std::string entitiesDir() { return join(root(), "entities"); }
inline std::string animsDir() { return join(entitiesDir(), "anims"); }
inline std::string animationFile(const std::string& stem) { return join(animsDir(), stem + ".animation"); }
inline std::string holdFile(const std::string& stem) { return join(entitiesDir(), stem + ".hold"); }
inline std::string tilesDir() { return join(root(), "materials/tiles"); }
inline std::string extrasDir() { return join(root(), "materials/extras"); }
inline std::string blocksDir() { return join(root(), "materials/blocks"); }

inline std::string entityPng(const std::string& stem) { return join(entitiesDir(), withPng(stem)); }
inline std::string entityModel(const std::string& stem) { return join(entitiesDir(), stem + ".model"); }
inline std::string tilePng(const std::string& stem) { return join(tilesDir(), withPng(stem)); }
inline std::string extraPng(const std::string& stem) { return join(extrasDir(), withPng(stem)); }
inline std::string blockModel(const std::string& stem) { return join(blocksDir(), stem + ".model"); }
inline std::string blockRand(const std::string& stem) { return join(blocksDir(), stem + ".rand"); }

inline std::string dataDir() { return join(root(), "data"); }
inline std::string dataPackDir(const std::string& packName) { return join(dataDir(), packName); }

inline bool exists(const std::string& path) {
    if (path.empty()) return false;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (f) { std::fclose(f); return true; }
    return false;
}

// First existing PNG: entities, extras, tiles.
// If none exist, return entities/<stem>.png as the default write target.
inline std::string resolvePng(const std::string& stem) {
    if (stem.empty()) return {};
    if (stem.find('/') != std::string::npos || stem.find('\\') != std::string::npos) {
        return stem;
    }
    const std::string cands[] = {
        entityPng(stem), extraPng(stem), tilePng(stem)
    };
    for (const std::string& p : cands) if (exists(p)) return p;
    return entityPng(stem);
}

} // namespace pack
