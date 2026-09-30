#include "structure.hpp"
#include "guardian_fight.hpp"
#include "ritual.hpp"
#include "matchmap.hpp"
#include "world.hpp"
#include "loot.hpp"
#include "player_model.hpp"
#include "asset_pack.hpp"
#include "arcane.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace structure {
namespace {

// Named per-cell tags live after the block payload. Each set flag bit is one
// tag from kCellFlagTag, so every marker round-trips without its own save code.
struct CellTag {
    int x = 0, y = 0, z = 0;
    std::string name;
};

struct Blueprint {
    std::string name;
    int sx = 0, sy = 0, sz = 0;
    std::vector<uint8_t> blocks;
    std::vector<CellTag> tags;
    uint8_t at(int x, int y, int z) const {
        if (x < 0 || y < 0 || z < 0 || x >= sx || y >= sy || z >= sz) return AIR;
        return blocks[(size_t)((y * sz + z) * sx + x)];
    }
    uint8_t flagsAt(int x, int y, int z) const {
        uint8_t flags = 0;
        for (const CellTag& t : tags) {
            if (t.x != x || t.y != y || t.z != z) continue;
            for (int bit = 0; bit < 8; ++bit) {
                if (t.name == kCellFlagTag[bit])
                    flags = (uint8_t)(flags | (1u << bit));
            }
        }
        return flags;
    }
    // Face whose normal is the stored placement axis. -1 means upright.
    int axisFaceAt(int x, int y, int z) const {
        int face = -1;
        for (const CellTag& t : tags) {
            if (t.x != x || t.y != y || t.z != z) continue;
            if (t.name == "axisx") face = 2;
            else if (t.name == "axisz") face = 4;
        }
        return face;
    }
};

struct Site {
    int cx = 0, cz = 0;
    int kind = 0; // 0 guardian platform, 1 ritual circle
    int id = 0;
    int ground = -1; // plank height; token sits at ground + 1
    bool looted = false;
};

// Each boss is above 250, and no two share a pool.
const int kGuardianHp[ritual::RelicCount] = {
    320, // 元素核心
    280, // 原始之火
    360, // 静滞之水
    260, // 生命嫩枝
    300, // 根须织毯
    340, // 裁决天平
    420, // 黄金冠冕
    380, // 审判之书
    310, // 鳞片沙漏
    450, // 循环刻印
    480, // 深渊棱镜
    400, // 上古图腾
    330, // 残响符石
    520, // 旧神骸骨
    560, // 无目雕像
};
int g_hp[ritual::RelicCount]{};

const char* kGuardianStem[ritual::RelicCount] = {
    "elem_core", "prim_fire", "still_water", "life_sprout", "root_weave",
    "judge_scale", "gold_crown", "judge_tome", "scale_glass", "cycle_mark",
    "abyss_prism", "ancient_totem", "echo_rune", "old_bones", "eyeless",
};

Vec3 g_boxMin[ritual::RelicCount]{};
Vec3 g_boxMax[ritual::RelicCount]{};
bool g_boxReady[ritual::RelicCount]{};
struct PoseSlot { bool on = false; float x = 0, y = 0, z = 0, yaw = 0; uint8_t swing = 0; };
PoseSlot g_pose[ritual::RelicCount]{};

std::vector<Site> g_sites;
bool g_trial = false;

// Equilateral-ish triangle on the altar, pointing -Z. Vertices sit inside the stone disk.
bool insideTri(int dx, int dz) {
    const int ax = 0, az = -6;
    const int bx = 5, bz = 3;
    const int cx = -5, cz = 3;
    auto cross = [](int px, int pz, int qx, int qz) { return px * qz - pz * qx; };
    int c0 = cross(bx - ax, bz - az, dx - ax, dz - az);
    int c1 = cross(cx - bx, cz - bz, dx - bx, dz - bz);
    int c2 = cross(ax - cx, az - cz, dx - cx, dz - cz);
    return c0 >= 0 && c1 >= 0 && c2 >= 0;
}

const Site* ritualSite(int ritual) {
    for (const Site& s : g_sites) {
        if (s.kind == 1 && s.id == ritual) return &s;
    }
    return nullptr;
}

int altarGround(World& world, const Site& s) {
    int ground = world.surfaceHeight(s.cx, s.cz);
    if (ground < 1) ground = 1;
    if (ground > cfg::WORLD_H - 6) ground = cfg::WORLD_H - 6;
    return ground;
}

int idxOf(const Blueprint& b, int x, int y, int z) {
    return (y * b.sz + z) * b.sx + x;
}

Blueprint makeRoom() {
    Blueprint b;
    b.name = "platform_5x5";
    b.sx = 5;
    b.sy = 1;
    b.sz = 5;
    b.blocks.assign((size_t)(b.sx * b.sy * b.sz), (uint8_t)PLANKS);
    return b;
}

bool writeBlueprint(const std::string& path, const Blueprint& b) {
    std::filesystem::path p(path);
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path());
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    char magic[8] = { 'V','L','S','T','R','U','C','T' };
    f.write(magic, 8);
    uint16_t ver = 1;
    f.write((const char*)&ver, 2);
    int32_t sx = b.sx, sy = b.sy, sz = b.sz;
    f.write((const char*)&sx, 4);
    f.write((const char*)&sy, 4);
    f.write((const char*)&sz, 4);
    if (!b.blocks.empty())
        f.write((const char*)b.blocks.data(), (std::streamsize)b.blocks.size());
    if (!b.tags.empty()) {
        uint32_t n = (uint32_t)b.tags.size();
        f.write((const char*)&n, 4);
        for (const CellTag& t : b.tags) {
            uint16_t x = (uint16_t)t.x, y = (uint16_t)t.y, z = (uint16_t)t.z;
            uint8_t len = (uint8_t)std::min(t.name.size(), (size_t)32);
            f.write((const char*)&x, 2);
            f.write((const char*)&y, 2);
            f.write((const char*)&z, 2);
            f.write((const char*)&len, 1);
            if (len) f.write(t.name.data(), len);
        }
    }
    return (bool)f;
}

bool readBlueprint(const std::string& path, Blueprint& b) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    char magic[8]{};
    f.read(magic, 8);
    if (std::memcmp(magic, "VLSTRUCT", 8) != 0) return false;
    uint16_t ver = 0;
    f.read((char*)&ver, 2);
    // Version 2 stores the same sx/sy/sz block payload as version 1.
    if (ver != 1 && ver != 2) return false;
    int32_t sx = 0, sy = 0, sz = 0;
    f.read((char*)&sx, 4);
    f.read((char*)&sy, 4);
    f.read((char*)&sz, 4);
    if (sx < 1 || sy < 1 || sz < 1 || sx > 64 || sy > 64 || sz > 64) return false;
    b.sx = sx;
    b.sy = sy;
    b.sz = sz;
    b.blocks.assign((size_t)sx * (size_t)sy * (size_t)sz, (uint8_t)AIR);
    f.read((char*)b.blocks.data(), (std::streamsize)b.blocks.size());
    if (!f) return false;
    for (uint8_t& cell : b.blocks) {
        if (cell >= liveBlockCount()) cell = AIR;
    }
    b.tags.clear();
    b.name = std::filesystem::path(path).stem().string();
    // Reading the last block byte sets eof. Clear it before measuring a trailer.
    // Older files end after the block array. A tag trailer is optional.
    f.clear();
    std::streampos mark = f.tellg();
    if (mark == std::streampos(-1)) return true;
    f.seekg(0, std::ios::end);
    std::streampos end = f.tellg();
    f.clear();
    f.seekg(mark);
    if (end == std::streampos(-1) || end - mark < 4) return true;
    uint32_t n = 0;
    f.read((char*)&n, 4);
    size_t volume = (size_t)sx * (size_t)sy * (size_t)sz;
    if (!f || n > volume * 4) return true;
    for (uint32_t i = 0; i < n; i++) {
        uint16_t x = 0, y = 0, z = 0;
        uint8_t len = 0;
        f.read((char*)&x, 2);
        f.read((char*)&y, 2);
        f.read((char*)&z, 2);
        f.read((char*)&len, 1);
        if (!f || len > 32) return true;
        std::string name(len, '\0');
        if (len) f.read(name.data(), len);
        if (!f) return true;
        if (x < sx && y < sy && z < sz && !name.empty())
            b.tags.push_back(CellTag{ (int)x, (int)y, (int)z, name });
    }
    return true;
}

} // namespace

std::string defaultPath() {
    return "assets/structures/platform_5x5.vlstruct";
}

void ensureLibrary() {
    std::filesystem::create_directories("assets/structures");
    bool any = false;
    std::filesystem::path dir("assets/structures");
    if (std::filesystem::exists(dir)) {
        for (const auto& ent : std::filesystem::directory_iterator(dir)) {
            if (ent.is_regular_file() && ent.path().extension() == ".vlstruct") { any = true; break; }
        }
    }
    if (!any) writeBlueprint(defaultPath(), makeRoom());
}

void roll(uint32_t seed) {
    ritual::roll(seed);
    guardian_fight::reset();
    for (PoseSlot& pose : g_pose) pose = {};
    g_sites.clear();
    for (int i = 0; i < ritual::RelicCount; i++) g_hp[i] = kGuardianHp[i];
    int c0 = matchmap::playMin() + 8;
    int c1 = matchmap::playMax() - 8;
    int span = c1 - c0 + 1;
    if (span < 8) {
        c0 = matchmap::playMin();
        c1 = matchmap::playMax();
        span = c1 - c0 + 1;
    }
    uint32_t rng = seed ? seed : 1u;
    auto next = [&]() {
        rng = rng * 1664525u + 1013904223u;
        return rng;
    };
    std::vector<std::pair<int, int>> pts;
    auto far = [&](int x, int z) {
        for (const auto& p : pts) {
            int dx = p.first - x;
            int dz = p.second - z;
            if (dx * dx + dz * dz < 96 * 96) return false;
        }
        return true;
    };
    int need = ritual::RelicCount + ritual::kRitualCount;
    int guard = 0;
    while ((int)pts.size() < need && guard < 20000) {
        guard++;
        int colx = c0 + (int)(next() % (uint32_t)span);
        int colz = c0 + (int)(next() % (uint32_t)span);
        int x = colx * cfg::CHUNK_X + (int)(next() % (uint32_t)cfg::CHUNK_X);
        int z = colz * cfg::CHUNK_Z + (int)(next() % (uint32_t)cfg::CHUNK_Z);
        if (!far(x, z)) continue;
        pts.push_back({ x, z });
    }
    int n = (int)pts.size();
    int items = ritual::RelicCount;
    if (items > n) items = n;
    for (int i = 0; i < items; i++) {
        Site s;
        s.cx = pts[(size_t)i].first;
        s.cz = pts[(size_t)i].second;
        s.kind = 0;
        s.id = i;
        g_sites.push_back(s);
    }
    int rituals = ritual::kRitualCount;
    if (items + rituals > n) rituals = n - items;
    for (int i = 0; i < rituals; i++) {
        Site s;
        s.cx = pts[(size_t)(items + i)].first;
        s.cz = pts[(size_t)(items + i)].second;
        s.kind = 1;
        s.id = i;
        g_sites.push_back(s);
    }
}

void stampColumn(int cx, int cz,
                 const std::function<int(int, int)>& height,
                 const std::function<void(int, int, int, uint8_t)>& put) {
    if (!height || !put) return;
    const int x0 = cx * cfg::CHUNK_X;
    const int z0 = cz * cfg::CHUNK_Z;
    const int x1 = x0 + cfg::CHUNK_X - 1;
    const int z1 = z0 + cfg::CHUNK_Z - 1;
    auto cell = [&](int wx, int wz, int ground, int rise, uint8_t block) {
        if (wx < x0 || wx > x1 || wz < z0 || wz > z1) return;
        int wy = ground + rise;
        if (wy < 0 || wy >= cfg::WORLD_H) return;
        put(wx, wy, wz, block);
    };
    for (Site& s : g_sites) {
        int rad = (s.kind == 1) ? 10 : 2;
        if (s.cx + rad < x0 || s.cx - rad > x1 || s.cz + rad < z0 || s.cz - rad > z1) continue;
        int ground = height(s.cx, s.cz);
        if (ground < 1) ground = 1;
        if (ground > cfg::WORLD_H - 6) ground = cfg::WORLD_H - 6;
        if (s.kind == 0) {
            for (int dz = -2; dz <= 2; dz++) {
                for (int dx = -2; dx <= 2; dx++) {
                    cell(s.cx + dx, s.cz + dz, ground, 0, PLANKS);
                    for (int rise = 1; rise <= 4; rise++)
                        cell(s.cx + dx, s.cz + dz, ground, rise, AIR);
                }
            }
            if (s.cx >= x0 && s.cx <= x1 && s.cz >= z0 && s.cz <= z1) {
                s.ground = ground;
                if (!s.looted && ritual::relicSpawned(s.id))
                    cell(s.cx, s.cz, ground, 0, (uint8_t)GUARDIAN_CORE);
            }
        } else {
            for (int dz = -10; dz <= 10; dz++) {
                for (int dx = -10; dx <= 10; dx++) {
                    int d2 = dx * dx + dz * dz;
                    if (d2 > 100) continue;
                    uint8_t floor = insideTri(dx, dz) ? (uint8_t)BRICK
                        : (d2 >= 64) ? (uint8_t)COBBLE : (uint8_t)STONE;
                    cell(s.cx + dx, s.cz + dz, ground, 0, floor);
                    for (int rise = 1; rise <= 4; rise++)
                        cell(s.cx + dx, s.cz + dz, ground, rise, AIR);
                }
            }
        }
    }
}

bool isOfferingCell(World& world, int ritual, int x, int y, int z) {
    const Site* s = ritualSite(ritual);
    if (!s) return false;
    if (y != altarGround(world, *s) + 1) return false;
    return insideTri(x - s->cx, z - s->cz);
}

bool offeringPlaced(World& world, int ritual, int relic) {
    const Site* s = ritualSite(ritual);
    if (!s || relic < 0) return false;
    uint8_t id = (uint8_t)ritual::blockId(relic);
    int ground = altarGround(world, *s);
    for (int dz = -6; dz <= 3; dz++) {
        for (int dx = -5; dx <= 5; dx++) {
            if (!insideTri(dx, dz)) continue;
            if (world.getBlock(s->cx + dx, ground + 1, s->cz + dz) == id) return true;
        }
    }
    return false;
}

bool offeringReady(World& world, int ritual) {
    int relics[3];
    ritual::recipeRelics(ritual, relics);
    return offeringPlaced(world, ritual, relics[0])
        && offeringPlaced(world, ritual, relics[1])
        && offeringPlaced(world, ritual, relics[2]);
}

bool nearRitual(int ritual, float x, float z) {
    const float S = cfg::BLOCK_SCALE;
    const float rad = 10.5f * S;
    for (const Site& s : g_sites) {
        if (s.kind != 1 || s.id != ritual) continue;
        float dx = x - (s.cx + 0.5f) * S;
        float dz = z - (s.cz + 0.5f) * S;
        return dx * dx + dz * dz <= rad * rad;
    }
    return false;
}

bool inVolume(int x, int y, int z) {
    return x >= kEditX && x < kEditX + kEditW
        && y >= kEditY && y < kEditY + kEditH
        && z >= kEditZ && z < kEditZ + kEditD;
}

bool paintFile(World& world, const std::string& path) {
    Blueprint b;
    if (!readBlueprint(path, b)) b = makeRoom();
    for (int ly = 0; ly < b.sy; ly++) {
        for (int lz = 0; lz < b.sz; lz++) {
            for (int lx = 0; lx < b.sx; lx++) {
                int x = kEditX + lx;
                int y = kEditY + ly;
                int z = kEditZ + lz;
                if (!inVolume(x, y, z)) continue;
                uint8_t block = b.at(lx, ly, lz);
                world.setBlock(x, y, z, block, false, true, b.axisFaceAt(lx, ly, lz), b.flagsAt(lx, ly, lz));
            }
        }
    }
    return true;
}

bool saveFile(const World& world, const std::string& path) {
    int minX = kEditX + kEditW, minY = kEditY + kEditH, minZ = kEditZ + kEditD;
    int maxX = -1, maxY = -1, maxZ = -1;
    for (int y = kEditY; y < kEditY + kEditH; y++) {
        for (int z = kEditZ; z < kEditZ + kEditD; z++) {
            for (int x = kEditX; x < kEditX + kEditW; x++) {
                if (world.getBlock(x, y, z) == AIR) continue;
                if (x < minX) minX = x;
                if (y < minY) minY = y;
                if (z < minZ) minZ = z;
                if (x > maxX) maxX = x;
                if (y > maxY) maxY = y;
                if (z > maxZ) maxZ = z;
            }
        }
    }
    if (maxX < minX) {
        Blueprint empty;
        empty.sx = empty.sy = empty.sz = 1;
        empty.blocks.assign(1, (uint8_t)AIR);
        return writeBlueprint(path, empty);
    }
    Blueprint b;
    b.sx = maxX - minX + 1;
    b.sy = maxY - minY + 1;
    b.sz = maxZ - minZ + 1;
    b.blocks.assign((size_t)b.sx * (size_t)b.sy * (size_t)b.sz, (uint8_t)AIR);
    for (int y = minY; y <= maxY; y++) {
        for (int z = minZ; z <= maxZ; z++) {
            for (int x = minX; x <= maxX; x++) {
                int lx = x - minX, ly = y - minY, lz = z - minZ;
                uint8_t block = world.getBlock(x, y, z);
                b.blocks[(size_t)idxOf(b, lx, ly, lz)] = block;
                if (block == AIR) continue;
                uint8_t flags = world.getFlags(x, y, z);
                for (int bit = 0; bit < 8; ++bit) {
                    if (flags & (uint8_t)(1u << bit))
                        b.tags.push_back(CellTag{ lx, ly, lz, kCellFlagTag[bit] });
                }
                int axis = world.logAxisAt(x, y, z);
                if (axis == 0) b.tags.push_back(CellTag{ lx, ly, lz, "axisx" });
                else if (axis == 2) b.tags.push_back(CellTag{ lx, ly, lz, "axisz" });
            }
        }
    }
    return writeBlueprint(path, b);
}

void clearVolume(World& world) {
    for (int y = kEditY; y < kEditY + kEditH; y++) {
        for (int z = kEditZ; z < kEditZ + kEditD; z++) {
            for (int x = kEditX; x < kEditX + kEditW; x++) {
                if (world.getBlock(x, y, z) != AIR)
                    world.setBlock(x, y, z, AIR, false, true);
            }
        }
    }
}

bool validName(const std::string& name) {
    if (name.empty() || name.size() > 48) return false;
    if (name == "." || name == "..") return false;
    unsigned char tail = (unsigned char)name.back();
    if (tail <= 32 || name.back() == '.') return false;
    for (unsigned char c : name) {
        if (c < 32) return false;
        switch (c) {
        case '\\': case '/': case ':': case '*': case '?':
        case '"': case '<': case '>': case '|':
            return false;
        default: break;
        }
    }
    std::string upper = name;
    for (char& c : upper) if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    size_t dot = upper.find('.');
    std::string stem = (dot == std::string::npos) ? upper : upper.substr(0, dot);
    if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL") return false;
    if (stem.size() == 4 && (stem.compare(0, 3, "COM") == 0 || stem.compare(0, 3, "LPT") == 0)
        && stem[3] >= '1' && stem[3] <= '9')
        return false;
    return true;
}

std::string pathFor(const std::string& name) {
    return "assets/structures/" + name + ".vlstruct";
}

void listFiles(std::vector<std::string>& names) {
    names.clear();
    std::filesystem::path dir("assets/structures");
    if (!std::filesystem::exists(dir)) return;
    for (const auto& ent : std::filesystem::directory_iterator(dir)) {
        if (!ent.is_regular_file()) continue;
        if (ent.path().extension() != ".vlstruct") continue;
        names.push_back(ent.path().stem().string());
    }
    std::sort(names.begin(), names.end());
}

bool createFile(const std::string& name) {
    if (!validName(name)) return false;
    std::string path = pathFor(name);
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) return false;
    Blueprint empty;
    empty.name = name;
    empty.sx = empty.sy = empty.sz = 1;
    empty.blocks.assign(1, (uint8_t)AIR);
    return writeBlueprint(path, empty);
}

bool deleteFile(const std::string& name) {
    if (!validName(name)) return false;
    std::error_code ec;
    return std::filesystem::remove(pathFor(name), ec) && !ec;
}

void collectBuildBlocks(std::vector<uint8_t>& out) {
    out.clear();
    for (int i = 1; i < liveBlockCount(); i++) {
        if (loot::itemDef((uint8_t)i).kind == loot::Kind::Block)
            out.push_back((uint8_t)i);
    }
}

const char* guardianAppearance(int relic) {
    if (relic < 0 || relic >= ritual::RelicCount) return "";
    return kGuardianStem[relic];
}

static void ensureBox(int relic) {
    if (relic < 0 || relic >= ritual::RelicCount || g_boxReady[relic]) return;
    g_boxReady[relic] = true;
    g_boxMin[relic] = { -0.28f, 0.0f, -0.18f };
    g_boxMax[relic] = { 0.28f, 1.70f, 0.18f };
    std::string path = pack::entityModel(std::string("guardians/") + kGuardianStem[relic]);
    pm::EntityFile ef = pm::loadEntity(path.c_str());
    if (ef.parts.empty()) return;
    Vec3 mn{ 1e9f, 1e9f, 1e9f };
    Vec3 mx{ -1e9f, -1e9f, -1e9f };
    for (const pm::Part& p : ef.parts) {
        Vec3 c[8];
        pm::partWorldCorners(p, c);
        for (int i = 0; i < 8; i++) {
            mn.x = std::min(mn.x, c[i].x); mn.y = std::min(mn.y, c[i].y); mn.z = std::min(mn.z, c[i].z);
            mx.x = std::max(mx.x, c[i].x); mx.y = std::max(mx.y, c[i].y); mx.z = std::max(mx.z, c[i].z);
        }
    }
    g_boxMin[relic] = mn;
    g_boxMax[relic] = mx;
}

bool poseOf(int relic, float& x, float& y, float& z, float& yaw, uint8_t& swing) {
    if (relic < 0 || relic >= ritual::RelicCount || !g_pose[relic].on) return false;
    const PoseSlot& pose = g_pose[relic];
    x = pose.x;
    y = pose.y;
    z = pose.z;
    yaw = pose.yaw;
    swing = pose.swing;
    return true;
}

static bool spanOf(const World& world, const Site& s, GuardianSpan& g) {
    if (s.kind != 0 || s.looted || s.ground < 0) return false;
    if (s.id < 0 || s.id >= ritual::RelicCount) return false;
    int x = s.cx, y = s.ground, z = s.cz;
    if (world.getBlock(x, y, z) != (uint8_t)GUARDIAN_CORE) return false;
    ensureBox(s.id);
    const float S = cfg::BLOCK_SCALE;
    const float pad = 0.04f;
    float yaw = 0.0f;
    uint8_t swing = 0;
    float fx = (s.cx + 0.5f) * S;
    float fy = (y + 1) * S;
    float fz = (s.cz + 0.5f) * S;
    poseOf(s.id, fx, fy, fz, yaw, swing);
    g.relic = s.id;
    g.feetX = fx;
    g.feetY = fy;
    g.feetZ = fz;
    float minX = 1.0e9f, maxX = -1.0e9f, minZ = 1.0e9f, maxZ = -1.0e9f;
    const float lx[2] = { g_boxMin[s.id].x, g_boxMax[s.id].x };
    const float lz[2] = { g_boxMin[s.id].z, g_boxMax[s.id].z };
    for (float xCorner : lx) {
        for (float zCorner : lz) {
            float ox = 0.0f, oz = 0.0f;
            pm::lookYawXZ(xCorner, zCorner, yaw, ox, oz);
            minX = std::min(minX, ox);
            maxX = std::max(maxX, ox);
            minZ = std::min(minZ, oz);
            maxZ = std::max(maxZ, oz);
        }
    }
    g.minX = fx + minX - pad;
    g.maxX = fx + maxX + pad;
    g.minY = fy + std::max(0.0f, g_boxMin[s.id].y) - pad;
    g.maxY = fy + g_boxMax[s.id].y + pad;
    g.minZ = fz + minZ - pad;
    g.maxZ = fz + maxZ + pad;
    g.hp = g_hp[s.id];
    g.maxHp = kGuardianHp[s.id];
    return true;
}

bool isGuardianToken(int x, int y, int z, uint8_t block) {
    if (block < ITEM_ELEM_CORE || block > ITEM_EYELESS) return false;
    for (const Site& s : g_sites) {
        if (s.kind != 0 || s.looted || s.ground < 0) continue;
        if (s.id < 0 || s.id >= ritual::RelicCount) continue;
        if (s.cx != x || s.cz != z || s.ground + 1 != y) continue;
        return block == (uint8_t)ritual::blockId(s.id);
    }
    return false;
}

void collectGuardians(const World& world, std::vector<GuardianSpan>& out) {
    out.clear();
    for (const Site& s : g_sites) {
        GuardianSpan g;
        if (spanOf(world, s, g)) out.push_back(g);
    }
}

bool raycastGuardian(const World& world, const Vec3& origin, const Vec3& dir, float maxDist,
                     float& tHit, GuardianSpan& hit, float radius) {
    Vec3 nd = dir;
    float len = nd.length();
    if (len < 1e-8f || !std::isfinite(maxDist) || maxDist <= 0.0f ||
        !std::isfinite(radius) || radius < 0.0f || radius > 1.0f) return false;
    nd = nd / len;
    tHit = maxDist + 1.0f;
    bool any = false;
    for (const Site& s : g_sites) {
        GuardianSpan g;
        if (!spanOf(world, s, g)) continue;
        float t = 0.0f;
        Vec3 mn{ g.minX - radius, g.minY - radius, g.minZ - radius };
        Vec3 mx{ g.maxX + radius, g.maxY + radius, g.maxZ + radius };
        if (!loot::rayAabb(origin, nd, mn, mx, t)) continue;
        if (t < 0.0f || t > maxDist || t >= tHit) continue;
        tHit = t;
        hit = g;
        any = true;
    }
    return any;
}

bool damageGuardian(const World& world, int relic, int amount, int& x, int& y, int& z) {
    if (relic < 0 || relic >= ritual::RelicCount) return false;
    if (amount < 0) amount = 0;
    for (Site& s : g_sites) {
        if (s.kind != 0 || s.id != relic) continue;
        if (s.looted || s.ground < 0) return false;
        x = s.cx;
        y = s.ground;
        z = s.cz;
        if (world.getBlock(x, y, z) != (uint8_t)GUARDIAN_CORE) {
            s.looted = true;
            return false;
        }
        if (g_hp[relic] > amount) {
            g_hp[relic] -= amount;
            return false;
        }
        g_hp[relic] = 0;
        s.looted = true;
        return true;
    }
    return false;
}

int guardianStrikeHurt(uint8_t held, int relic) {
    if (relic < 0 || relic >= ritual::RelicCount) return 0;
    // Trial guardians only. Armor, resists and vulnerability stay at zero until a
    // relic profile exists. Player limb damage does not feed this pool.
    // 受伤 = max(每击物理 - 装甲 - 物理抵抗, 0) * (基础攻击倍率 + 力量增幅 + 物理易伤)
    //      + max(每击神秘学 - 神秘学抵抗, 0) * (基础攻击倍率 + 物理易伤)
    float physHit = 0.0f;
    float occultHit = 0.0f;
    const float physResist = 0.0f;
    const float occultResist = 0.0f;
    const float baseAtk = 1.0f;
    const float strAmp = 0.0f;
    const float armor = 0.0f;
    const float physVuln = 0.0f;
    if (held != AIR && hasItemTags(held, TAG_AXE)) {
        physHit = 15.0f;
    } else if (held != AIR && loot::isTool(held)) {
        physHit = 5.0f;
    } else {
        physHit = 2.0f;
        occultHit = 1.0f;
    }
    float phys = physHit - armor - physResist;
    if (phys < 0.0f) phys = 0.0f;
    float occult = occultHit - occultResist;
    if (occult < 0.0f) occult = 0.0f;
    float hurt = phys * (baseAtk + strAmp + physVuln) + occult * (baseAtk + physVuln);
    if (hurt <= 0.0f) return 0;
    return (int)std::lround(hurt);
}

int guardianArcaneHurt(uint8_t item, int relic, float distance) {
    if (relic < 0 || relic >= ritual::RelicCount || !std::isfinite(distance) || distance < 0.0f)
        return 0;
    float fraction = 0.0f;
    if (item == ITEM_ARCANE_FIREBALL) fraction = arcane::explosionDamage(distance);
    else if (item == ITEM_ARCANE_FREEZE) fraction = arcane::kFreezeDamage;
    if (!(fraction > 0.0f)) return 0;
    return std::max(1, (int)std::lround(fraction * (float)kGuardianHp[relic]));
}

void collectRoomGuardians(std::vector<GuardianSync>& out) {
    out.clear();
    for (const Site& s : g_sites) {
        if (s.kind != 0 || s.ground < 0) continue;
        if (s.id < 0 || s.id >= ritual::RelicCount) continue;
        GuardianSync g;
        g.relic = s.id;
        g.x = s.cx;
        g.y = s.ground + 1;
        g.z = s.cz;
        g.maxHp = kGuardianHp[s.id];
        g.hp = s.looted ? 0 : g_hp[s.id];
        out.push_back(g);
    }
}

void setGuardianPose(int relic, float x, float y, float z, float yaw, uint8_t swing) {
    if (relic < 0 || relic >= ritual::RelicCount) return;
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || !std::isfinite(yaw)) return;
    g_pose[relic] = { true, x, y, z, yaw, swing };
}

void clearGuardianPose(int relic) {
    if (relic < 0 || relic >= ritual::RelicCount) return;
    g_pose[relic] = {};
}

bool guardianPose(int relic, float& x, float& y, float& z, float& yaw, uint8_t& swing) {
    return poseOf(relic, x, y, z, yaw, swing);
}

void applyRoomGuardian(int relic, int hp, float x, float y, float z, float yaw, uint8_t swing) {
    if (relic < 0 || relic >= ritual::RelicCount) return;
    if (hp < 0) hp = 0;
    g_hp[relic] = hp;
    for (Site& s : g_sites) {
        if (s.kind != 0 || s.id != relic) continue;
        s.looted = hp <= 0;
    }
    if (hp <= 0) clearGuardianPose(relic);
    else setGuardianPose(relic, x, y, z, yaw, swing);
}

bool roomGuardianHit(const World& world, const Vec3& eye, int relic, float reach,
                     int& x, int& y, int& z) {
    x = y = z = 0;
    if (relic < 0 || relic >= ritual::RelicCount) return false;
    if (!std::isfinite(eye.x) || !std::isfinite(eye.y) || !std::isfinite(eye.z)) return false;
    if (!(reach > 0.0f) || !std::isfinite(reach)) return false;
    for (const Site& s : g_sites) {
        if (s.kind != 0 || s.id != relic) continue;
        GuardianSpan g;
        if (!spanOf(world, s, g)) return false;
        float cx = eye.x < g.minX ? g.minX : (eye.x > g.maxX ? g.maxX : eye.x);
        float cy = eye.y < g.minY ? g.minY : (eye.y > g.maxY ? g.maxY : eye.y);
        float cz = eye.z < g.minZ ? g.minZ : (eye.z > g.maxZ ? g.maxZ : eye.z);
        float dx = cx - eye.x, dy = cy - eye.y, dz = cz - eye.z;
        float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (dist > reach) return false;
        x = s.cx;
        y = s.ground + 1;
        z = s.cz;
        if (dist < 0.05f) return true;
        Vec3 dir{ dx / dist, dy / dist, dz / dist };
        IVec3 hit, prev;
        Vec3 nrm;
        float blockT = dist + 1.0f;
        if (world.raycast(eye, dir, dist, hit, prev, nrm, nullptr, &blockT)
            && blockT + 0.35f < dist
            && !(hit.x == x && hit.y == y && hit.z == z))
            return false;
        return true;
    }
    return false;
}

void openTrial(int relic) {
    if (relic < 0 || relic >= ritual::RelicCount) return;
    g_sites.clear();
    Site s;
    s.cx = kTrialCX;
    s.cz = kTrialCZ;
    s.kind = 0;
    s.id = relic;
    s.ground = kTrialFloor;
    s.looted = false;
    g_sites.push_back(s);
    g_hp[relic] = kGuardianHp[relic];
    g_trial = true;
    guardian_fight::reset();
    for (PoseSlot& pose : g_pose) pose = {};
}

void closeTrial() {
    if (!g_trial) return;
    g_sites.clear();
    g_trial = false;
    guardian_fight::reset();
    for (PoseSlot& pose : g_pose) pose = {};
}

bool trialActive() { return g_trial; }

void ensureTrialCore(World& world) {
    if (!g_trial || g_sites.empty()) return;
    Site& s = g_sites[0];
    if (s.kind != 0 || s.looted || s.ground < 0) return;
    if (s.id < 0 || s.id >= ritual::RelicCount) return;
    int x = s.cx;
    int y = s.ground;
    int z = s.cz;
    if (world.getBlock(x, y, z) == (uint8_t)GUARDIAN_CORE) return;
    world.setBlock(x, y, z, (uint8_t)GUARDIAN_CORE, true);
    if (world.getBlock(x, y + 1, z) != AIR)
        world.setBlock(x, y + 1, z, AIR, true);
}

bool nearestGuardian(const World& world, const Vec3& pos, float maxDist, GuardianSpan& out) {
    float best = maxDist;
    bool any = false;
    for (const Site& s : g_sites) {
        GuardianSpan g;
        if (!spanOf(world, s, g)) continue;
        float cx = (g.minX + g.maxX) * 0.5f;
        float cy = (g.minY + g.maxY) * 0.5f;
        float cz = (g.minZ + g.maxZ) * 0.5f;
        float dx = pos.x - cx, dy = pos.y - cy, dz = pos.z - cz;
        float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (dist > best) continue;
        best = dist;
        out = g;
        any = true;
    }
    return any;
}

namespace {

void ensureSpan(World& world, int x0, int z0, int x1, int z1) {
    if (x1 < x0 || z1 < z0) return;
    int cx0 = floorDiv(x0, cfg::CHUNK_X);
    int cx1 = floorDiv(x1, cfg::CHUNK_X);
    int cz0 = floorDiv(z0, cfg::CHUNK_Z);
    int cz1 = floorDiv(z1, cfg::CHUNK_Z);
    for (int cz = cz0; cz <= cz1; ++cz) {
        for (int cx = cx0; cx <= cx1; ++cx)
            world.ensureColumn(cx, cz);
    }
}

int footprintFloor(World& world, int x0, int z0, int x1, int z1, int height) {
    int floorY = 1;
    for (int z = z0; z <= z1; ++z) {
        for (int x = x0; x <= x1; ++x) {
            int h = world.surfaceHeight(x, z);
            if (h > floorY) floorY = h;
        }
    }
    if (height < 1) height = 1;
    if (floorY > cfg::WORLD_H - height) floorY = cfg::WORLD_H - height;
    if (floorY < 1) floorY = 1;
    return floorY;
}

void putSolid(World& world, int x, int y, int z, uint8_t block, int placeFace = -1, int cellFlags = -1) {
    if (y < 0 || y >= cfg::WORLD_H) return;
    world.setBlock(x, y, z, block, true, false, placeFace, cellFlags);
}

// Bottom sits on the highest terrain under the box. Columns that have a solid
// on the blueprint's bottom layer are filled up from the local surface.
int stampBlueprint(World& world, int worldX, int worldZ, const Blueprint& b, int& placed) {
    int x0 = worldX - b.sx / 2;
    int z0 = worldZ - b.sz / 2;
    int x1 = x0 + b.sx - 1;
    int z1 = z0 + b.sz - 1;
    ensureSpan(world, x0, z0, x1, z1);
    int floorY = footprintFloor(world, x0, z0, x1, z1, b.sy);
    for (int lz = 0; lz < b.sz; ++lz) {
        for (int lx = 0; lx < b.sx; ++lx) {
            uint8_t base = b.at(lx, 0, lz);
            if (base == AIR) continue;
            int x = x0 + lx;
            int z = z0 + lz;
            int ground = world.surfaceHeight(x, z);
            for (int y = ground; y < floorY; ++y)
                putSolid(world, x, y, z, base);
        }
    }
    placed = 0;
    for (int ly = 0; ly < b.sy; ++ly) {
        for (int lz = 0; lz < b.sz; ++lz) {
            for (int lx = 0; lx < b.sx; ++lx) {
                uint8_t block = b.at(lx, ly, lz);
                if (block == AIR) continue;
                putSolid(world, x0 + lx, floorY + ly, z0 + lz, block,
                         b.axisFaceAt(lx, ly, lz), b.flagsAt(lx, ly, lz));
                placed++;
            }
        }
    }
    return floorY;
}

Blueprint fallbackAltar() {
    Blueprint b;
    b.sx = 5;
    b.sy = 5;
    b.sz = 5;
    b.blocks.assign(125, (uint8_t)AIR);
    auto set = [&](int x, int y, int z, uint8_t block) {
        b.blocks[(size_t)((y * b.sz + z) * b.sx + x)] = block;
    };
    for (int z = 0; z < 5; ++z) {
        for (int x = 0; x < 5; ++x)
            set(x, 0, z, (x == 2 && z == 2) ? (uint8_t)COBBLE : (uint8_t)STONE);
    }
    for (int y = 1; y <= 3; ++y) set(2, y, 2, (uint8_t)COBBLE);
    set(2, 4, 2, (uint8_t)PLANKS);
    return b;
}

Blueprint fallbackRoom() {
    Blueprint b;
    b.sx = 7;
    b.sy = 4;
    b.sz = 7;
    b.blocks.assign((size_t)b.sx * b.sy * b.sz, (uint8_t)AIR);
    for (int y = 0; y < b.sy; ++y) {
        for (int z = 0; z < b.sz; ++z) {
            for (int x = 0; x < b.sx; ++x) {
                bool shell = y == 0 || x == 0 || x == b.sx - 1 || z == 0 || z == b.sz - 1;
                uint8_t block = AIR;
                if (shell) block = (uint8_t)PLANKS;
                else if (y == b.sy - 1) block = (uint8_t)WOOD;
                b.blocks[(size_t)((y * b.sz + z) * b.sx + x)] = block;
            }
        }
    }
    return b;
}

} // namespace

// Ritual altar file names
const char* ritualAltarName(int altarIndex) {
    switch (altarIndex) {
        case 0: return "ritual_element";
        case 1: return "ritual_god";
        case 2: return "ritual_old_god";
        case 3: return "ritual_outer";
        case 4: return "ritual_time";
        case 5: return "ritual_worldtree";
        default: return "ritual_element";
    }
}

bool paintRitualAltar(World& world, int worldX, int& worldY, int worldZ, int altarIndex) {
    if (altarIndex < 0 || altarIndex >= kRitualAltarCount) altarIndex = 0;
    std::string path = "assets/structures/";
    path += ritualAltarName(altarIndex);
    path += ".vlstruct";
    Blueprint b;
    bool loaded = std::filesystem::exists(path) && readBlueprint(path, b);
    if (!loaded) b = fallbackAltar();
    int placed = 0;
    worldY = stampBlueprint(world, worldX, worldZ, b, placed);
    fprintf(stderr, "[DEBUG] Altar %s: placed %d blocks at (%d,%d,%d) size=(%d,%d,%d) loaded=%d\n",
        ritualAltarName(altarIndex), placed, worldX, worldY, worldZ, b.sx, b.sy, b.sz, loaded ? 1 : 0);
    return true;
}

// Stonehenge structure file names
const char* stonehengeName(int stonehengeIndex) {
    switch (stonehengeIndex) {
        case 0: return "stonehenge";
        default: return "stonehenge";
    }
}

bool paintStonehenge(World& world, int worldX, int& worldY, int worldZ, int stonehengeIndex) {
    if (stonehengeIndex < 0 || stonehengeIndex >= kStonehengeCount) stonehengeIndex = 0;
    std::string path = "assets/structures/";
    path += stonehengeName(stonehengeIndex);
    path += ".vlstruct";
    Blueprint b;
    bool loaded = std::filesystem::exists(path) && readBlueprint(path, b);
    if (!loaded) {
        // Fallback: create a simple stonehenge-like structure
        b.sx = 41; b.sy = 11; b.sz = 41;
        b.blocks = std::vector<uint8_t>((size_t)b.sx * b.sy * b.sz, 0);
    }
    int placed = 0;
    worldY = stampBlueprint(world, worldX, worldZ, b, placed);
    fprintf(stderr, "[DEBUG] Stonehenge %s: placed %d blocks at (%d,%d,%d) size=(%d,%d,%d) loaded=%d\n",
        stonehengeName(stonehengeIndex), placed, worldX, worldY, worldZ, b.sx, b.sy, b.sz, loaded ? 1 : 0);
    return true;
}

// Room building file names (props, weapon, clue). The files are medieval
// houses written by tools/stl2vlstruct/medieval_rooms.py.
const char* roomBuildingName(int roomIndex) {
    switch (roomIndex) {
        case 0: return "room_basic";
        case 1: return "room_isometric";
        case 2: return "room_japanese";
        default: return "room_basic";
    }
}

bool paintRoomBuilding(World& world, int worldX, int& worldY, int worldZ, int roomIndex) {
    if (roomIndex < 0 || roomIndex >= kRoomBuildingCount) roomIndex = 0;
    std::string path = "assets/structures/";
    path += roomBuildingName(roomIndex);
    path += ".vlstruct";
    Blueprint b;
    bool loaded = std::filesystem::exists(path) && readBlueprint(path, b);
    if (!loaded) b = fallbackRoom();
    int placed = 0;
    worldY = stampBlueprint(world, worldX, worldZ, b, placed);
    fprintf(stderr, "[DEBUG] Room %s: placed %d blocks at (%d,%d,%d) size=(%d,%d,%d) loaded=%d\n",
        roomBuildingName(roomIndex), placed, worldX, worldY, worldZ, b.sx, b.sy, b.sz, loaded ? 1 : 0);
    return true;
}

} // namespace structure
