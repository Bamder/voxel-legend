#include "structure.hpp"
#include "ritual.hpp"
#include "matchmap.hpp"
#include "world.hpp"
#include "loot.hpp"
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace structure {
namespace {

struct Blueprint {
    std::string name;
    int sx = 0, sy = 0, sz = 0;
    std::vector<uint8_t> blocks;
    uint8_t at(int x, int y, int z) const {
        if (x < 0 || y < 0 || z < 0 || x >= sx || y >= sy || z >= sz) return AIR;
        return blocks[(size_t)((y * sz + z) * sx + x)];
    }
};

struct Site {
    int cx = 0, cz = 0;
    int kind = 0; // 0 item platform, 1 ritual circle
    int id = 0;
};

std::vector<Site> g_sites;
int g_activeTeams = 0; // 实际队伍数量
bool g_ritualsAssigned = false; // 是否已分配祭祠台到队伍

// 祭祠台结构文件名映射
const char* ritualStructureFile(int ritualId) {
    static const char* kRitualStructures[ritual::kRitualCount] = {
        "ritual_element.vlstruct",    // 元素之源
        "ritual_worldtree.vlstruct", // 世界树
        "ritual_god.vlstruct",       // 世界主神
        "ritual_time.vlstruct",      // 溯时之蛇
        "ritual_old_god.vlstruct",   // 旧神残魂
        "ritual_outer.vlstruct"      // 外神
    };
    if (ritualId < 0 || ritualId >= ritual::kRitualCount) return nullptr;
    return kRitualStructures[ritualId];
}

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
    if (ver < 1 || ver > 2) return false; // 支持版本 1 和 2
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
    b.name = std::filesystem::path(path).stem().string();
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
    g_sites.clear();
    printf("[DEBUG] roll() called with seed=%u\n", seed);

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

    // 出生点位置（玩家进入游戏的位置）
    const int SPAWN_X = 8;
    const int SPAWN_Z = 8;
    // 每个队伍的中心点作为祭祠台位置
    int teamSpawns[ritual::kRitualCount][2];
    int ritualCount = ritual::kRitualCount; // 6

    // 计算4个队伍的出生点（4x4区块的中心）
    for (int team = 0; team < 4 && team < ritualCount; team++) {
        matchmap::Zone zone = matchmap::combatZone(team);
        int spawnX = zone.cx0 * (int)cfg::CHUNK_X + ((int)matchmap::kZoneChunks * (int)cfg::CHUNK_X) / 2;
        int spawnZ = zone.cz0 * (int)cfg::CHUNK_Z + ((int)matchmap::kZoneChunks * (int)cfg::CHUNK_Z) / 2;
        teamSpawns[team][0] = spawnX;
        teamSpawns[team][1] = spawnZ;
    }

    // 如果有超过4个祭祠台，用随机位置填充
    for (int i = 4; i < ritualCount; i++) {
        pts.push_back({ SPAWN_X + 100 + i * 50, SPAWN_Z + 100 + i * 30 });
    }

    // 生成其他随机位置（用于遗物）
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

    // 祭祠台放置策略：每个队伍出生点放置一个祭祠台
    int rituals = ritual::kRitualCount;
    if (items + rituals > n) rituals = n - items;

    // 前4个祭祠台放在队伍出生点
    for (int i = 0; i < rituals && i < 4; i++) {
        Site s;
        s.cx = teamSpawns[i][0];
        s.cz = teamSpawns[i][1];
        s.kind = 1;
        s.id = i;
        g_sites.push_back(s);
    }

    // 如果有超过4个祭祠台，其他的使用随机位置
    for (int i = 4; i < rituals; i++) {
        int idx = items + i;
        if (idx < n) {
            Site s;
            s.cx = pts[(size_t)idx].first;
            s.cz = pts[(size_t)idx].second;
            s.kind = 1;
            s.id = i;
            g_sites.push_back(s);
        }
    }
}

void setActiveTeams(int count) {
    g_activeTeams = count;
    g_ritualsAssigned = false;
}

void assignRitualsToTeams() {
    if (g_ritualsAssigned) return;
    if (g_activeTeams <= 0) return;

    // 找到所有祭祠台站点
    std::vector<Site*> ritualSites;
    for (Site& s : g_sites) {
        if (s.kind == 1) {
            ritualSites.push_back(&s);
        }
    }

    if (ritualSites.empty()) return;

    // 随机打乱祭祠台顺序
    uint32_t rng = (uint32_t)time(nullptr);
    auto shuffle = [&](int n) {
        for (int i = n - 1; i > 0; i--) {
            rng = rng * 1664525u + 1013904223u;
            int j = (int)((rng >> 16) % (uint32_t)(i + 1));
            std::swap(ritualSites[i], ritualSites[j]);
        }
    };
    shuffle((int)ritualSites.size());

    // 计算每个队伍的出生点
    struct TeamSpawn {
        int cx, cz;
    };
    std::vector<TeamSpawn> teamSpawns;
    for (int i = 0; i < g_activeTeams && i < matchmap::kCombatTeams; i++) {
        matchmap::Zone zone = matchmap::combatZone(i);
        int spawnX = zone.cx0 * (int)cfg::CHUNK_X + ((int)matchmap::kZoneChunks * (int)cfg::CHUNK_X) / 2;
        int spawnZ = zone.cz0 * (int)cfg::CHUNK_Z + ((int)matchmap::kZoneChunks * (int)cfg::CHUNK_Z) / 2;
        teamSpawns.push_back({ spawnX, spawnZ });
    }

    // 将祭祠台分配到队伍出生点
    for (int i = 0; i < g_activeTeams && i < (int)ritualSites.size(); i++) {
        ritualSites[i]->cx = teamSpawns[i].cx;
        ritualSites[i]->cz = teamSpawns[i].cz;
    }

    g_ritualsAssigned = true;
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

    // 祭祠台结构缓存
    static Blueprint g_ritualBlueprints[ritual::kRitualCount] = {};
    static bool g_ritualLoaded[ritual::kRitualCount] = {};

    for (const Site& s : g_sites) {
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
            if (ritual::relicSpawned(s.id))
                cell(s.cx, s.cz, ground, 1, (uint8_t)ritual::blockId(s.id));
        } else {
            // 尝试加载优化后的祭祠台结构
            bool useOptimized = false;
            Blueprint* bp = nullptr;

            if (!g_ritualLoaded[s.id]) {
                const char* fname = ritualStructureFile(s.id);
                if (fname) {
                    std::string path = "assets/structures/";
                    path += fname;
                    printf("[DEBUG] Loading ritual %d from %s\n", s.id, path.c_str());
                    if (readBlueprint(path, g_ritualBlueprints[s.id])) {
                        printf("[DEBUG] readBlueprint succeeded, liveBlockCount=%d\n", liveBlockCount());
                        g_ritualLoaded[s.id] = true;
                        useOptimized = true;
                        bp = &g_ritualBlueprints[s.id];
                    } else {
                        printf("[DEBUG] readBlueprint FAILED for %s\n", path.c_str());
                    }
                }
            } else if (g_ritualLoaded[s.id] && g_ritualBlueprints[s.id].sx > 0) {
                useOptimized = true;
                bp = &g_ritualBlueprints[s.id];
            }

            if (useOptimized && bp) {
                // 使用优化后的祭祠台结构
                int ox = s.cx - bp->sx / 2;
                int oz = s.cz - bp->sz / 2;
                for (int ly = 0; ly < bp->sy; ly++) {
                    for (int lz = 0; lz < bp->sz; lz++) {
                        for (int lx = 0; lx < bp->sx; lx++) {
                            uint8_t blk = bp->at(lx, ly, lz);
                            if (blk != AIR) {
                                put(ox + lx, ground + ly, oz + lz, blk);
                            }
                        }
                    }
                }
                // 三角祭祠区域仍然使用代码生成
                for (int dz = -10; dz <= 10; dz++) {
                    for (int dx = -10; dx <= 10; dx++) {
                        int d2 = dx * dx + dz * dz;
                        if (d2 > 100) continue;
                        if (insideTri(dx, dz)) {
                            put(s.cx + dx, ground, s.cz + dz, (uint8_t)BRICK);
                        }
                    }
                }
            } else {
                // 回退到简单的几何形状生成
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
                world.setBlock(x, y, z, b.at(lx, ly, lz), false, true);
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
                b.blocks[(size_t)idxOf(b, x - minX, y - minY, z - minZ)] = world.getBlock(x, y, z);
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

} // namespace structure
