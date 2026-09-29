#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "room_net.hpp"
#include "room_body.hpp"
#include "room_inventory.hpp"
#include "../core/config.hpp"
#include "../world/animation.hpp"
#include "../world/arcane.hpp"
#include "../world/clue.hpp"
#include "../world/combat.hpp"
#include "../world/guardian_fight.hpp"
#include "../world/guardian_fight_world.hpp"
#include "../world/ritual.hpp"
#include "../world/structure.hpp"
#include "../world/world.hpp"
#include "../world/matchmap.hpp"
#include <filesystem>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <deque>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

constexpr int kHashEvery = 40;
constexpr int kHashBatch = 8;

struct RevDelta {
    uint32_t rev = 0;
    std::vector<World::AuthCell> cells;
    bool sod = false;
    std::vector<World::AuthSod> sods;
    bool bark = false;
    std::vector<World::AuthBark> barks;
};

struct SeenTree {
    uint32_t rev = 0;
};

struct MineId {
    uint32_t tree = 0;
    int x = 0, y = 0, z = 0;
    bool operator==(const MineId& o) const {
        return tree == o.tree && x == o.x && y == o.y && z == o.z;
    }
};

struct MineIdHash {
    size_t operator()(const MineId& k) const {
        size_t h = k.tree;
        h = h * 1315423911u + (unsigned)k.x;
        h = h * 1315423911u + (unsigned)k.y;
        h = h * 1315423911u + (unsigned)k.z;
        return h;
    }
};

struct SeenMine {
    uint32_t serial = 0;
};

struct SrvChunk {
    uint32_t rev = 0;
    bool diverged = false;
    std::deque<RevDelta> log;
};

struct SeenSlice {
    uint32_t rev = 0;
    bool base = false;
};

struct SClient {
    room_body::State body;
    room_inventory::State inventory;
    combat::MeleeState melee;
    combat::MeleeState guardianMelee;
    arcane::CastState fireball;
    arcane::CastState freezeCast;
    arcane::CastState healCast;
    std::vector<arcane::Burning> burning;
    arcane::Frozen frozen;
    NetConn conn;
    uint32_t id = 0;
    std::string name;
    int team = -1;
    bool spectator = false;
    bool landed = true;
    bool deathDeploy = false;
    bool pin = false;
    int dbx = 0, dbz = 0;
    float pinLeft = 0.0f;
    float fade = 0.0f;
    float landX = 0, landY = 0, landZ = 0;
    bool known = false;
    bool sentWelcome = false;
    bool hasInput = false;
    float x = 0, y = 0, z = 0, yaw = 0, pitch = 0, bodyYaw = 0;
    float vx = 0, vz = 0;
    bool flying = false;
    bool sprinting = false;
    uint8_t heldL = 0, heldR = 0, carried = 0;
    uint8_t wearU = 0, wearL = 0, wearS = 0;
    uint8_t strikeKind = 0;
    float strikeCharge = 0, strikeCool = 0, mineCharge = 0, mineCooldown = 0;
    bool pickRaised = false;
    uint8_t selectedLeft = 0, selectedRight = 0;
    uint32_t attackStartedAt = 0;
    uint32_t combatAck = 0;
    uint32_t arcaneAck = 0;
    uint8_t guardianRelic = 255;
    uint32_t hitFlashUntil = 0;
    uint32_t healFlashUntil = 0;
    std::string animName = "idle";
    float animClock = 0;
    uint8_t animClip = anim::kNetIdle;
    uint16_t animFrameQ = 0;
    uint8_t strikeClip = 0;
    uint16_t strikeFrameQ = 0;
    std::vector<uint32_t> announced;
    std::unordered_map<int64_t, SeenSlice> seen;
    std::unordered_set<int64_t> cols;
    std::deque<int64_t> baseQ;
    std::unordered_set<int64_t> baseQueued;
    int hashCursor = 0;
    std::unordered_map<uint32_t, SeenTree> trees;
    bool treeResync = false;
    std::unordered_map<MineId, SeenMine, MineIdHash> mines;
};

struct Seat {
    RoomPlayerNet player;
    bool used = false;
};

void slog(FILE* f, const char* msg) {
    if (!f) return;
    fprintf(f, "%s\n", msg);
    fflush(f);
}

bool inventoryHas(const room_inventory::State& inventory, uint8_t item) {
    for (const ItemSlot slot : inventory.slots)
        if (!slot.empty() && slot.block == item) return true;
    return false;
}

void setNonBlock(SOCKET s) {
    u_long mode = 1;
    ioctlsocket(s, FIONBIO, &mode);
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));
}

long long nowMs() {
    return (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void applyEdit(World& world, const BlockEditNet& e) {
    if (e.y < 0 || e.y >= cfg::WORLD_H) return;
    int cx = floorDiv(e.x, cfg::CHUNK_X);
    int cz = floorDiv(e.z, cfg::CHUNK_Z);
    world.ensureColumn(cx, cz);
    if (!world.columnLoaded(cx, cz)) return;
    if (world.getBlock(e.x, e.y, e.z) == e.block) return;
    world.setBlock(e.x, e.y, e.z, e.block, false, false);
}

void applyBark(World& world, const PlayInputNet::BarkEdit& e) {
    if (e.y < 0 || e.y >= cfg::WORLD_H || e.face > 5) return;
    int cx = floorDiv(e.x, cfg::CHUNK_X);
    int cz = floorDiv(e.z, cfg::CHUNK_Z);
    world.ensureColumn(cx, cz);
    if (e.place) world.addBarkFace(e.x, e.y, e.z, e.face);
    else world.takeBarkFace(e.x, e.y, e.z, e.face);
}

void applyMine(World& world, const PlayInputNet::MineEdit& e) {
    int phys = -1;
    if (e.tree != 0) {
        const std::vector<PhysicsIsland>& islands = world.physicsIslands();
        phys = -2;
        for (int i = 0; i < (int)islands.size(); i++) {
            if (islands[(size_t)i].netId == e.tree) { phys = i; break; }
        }
        if (phys < 0) return;
    } else if (e.y < 0 || e.y >= cfg::WORLD_H) {
        return;
    }
    uint8_t mined = phys < 0 ? world.getBlock(e.x, e.y, e.z)
                             : world.getPhysBlock(phys, e.x, e.y, e.z);
    bool broke = world.applyMineHit(phys, e.x, e.y, e.z, e.tool, e.face);
    if (!broke) return;
    if (phys < 0) {
        int cx = floorDiv(e.x, cfg::CHUNK_X);
        int cz = floorDiv(e.z, cfg::CHUNK_Z);
        world.ensureColumn(cx, cz);
        world.setBlock(e.x, e.y, e.z, AIR, false, false);
        if (loot::isHarvestBreak(mined, e.tool)) {
            loot::DropSpec drops[4];
            int count = loot::harvestDrops(mined, e.tool, drops, 4);
            Vec3 position{(e.x + .5f) * cfg::BLOCK_SCALE,
                          (e.y + .5f) * cfg::BLOCK_SCALE,
                          (e.z + .5f) * cfg::BLOCK_SCALE};
            for (int i = 0; i < count; ++i)
                world.spawnDrop(position, drops[i].item, drops[i].count, true);
        }
    } else {
        uint8_t drop = AIR;
        world.breakPhysBlock(phys, e.x, e.y, e.z, drop);
    }
}

void queueBase(SClient& c, int64_t col, bool front) {
    if (!c.baseQueued.insert(col).second) return;
    if (front) c.baseQ.push_front(col);
    else c.baseQ.push_back(col);
}

void noteAuth(std::unordered_map<int64_t, SrvChunk>& srv, World& world) {
    for (World::AuthSlice& s : world.flushAuth()) {
        int64_t key = chunkKey(s.cx, s.cy, s.cz);
        SrvChunk& sc = srv[key];
        sc.rev++;
        sc.diverged = true;
        RevDelta d;
        d.rev = sc.rev;
        d.cells = std::move(s.cells);
        d.sod = s.sod;
        d.sods = std::move(s.sods);
        d.bark = s.bark;
        d.barks = std::move(s.barks);
        sc.log.push_back(std::move(d));
        while (sc.log.size() > 32) sc.log.pop_front();
    }
}

void fillChunkSync(World& world, std::unordered_map<int64_t, SrvChunk>& srv, SClient& c,
                   PlayDeltaNet& d, uint32_t serverTick) {
    int pcx = floorDiv((int)std::floor(c.x / cfg::BLOCK_SCALE), cfg::CHUNK_X);
    int pcz = floorDiv((int)std::floor(c.z / cfg::BLOCK_SCALE), cfg::CHUNK_Z);
    std::unordered_set<int64_t> now;
    for (int dz = -cfg::LOAD_RADIUS; dz <= cfg::LOAD_RADIUS; dz++) {
        for (int dx = -cfg::LOAD_RADIUS; dx <= cfg::LOAD_RADIUS; dx++)
            now.insert(columnKey(pcx + dx, pcz + dz));
    }
    std::vector<int64_t> left;
    for (int64_t col : c.cols)
        if (!now.count(col)) left.push_back(col);
    for (int64_t col : left) {
        c.cols.erase(col);
        int cx = columnCX(col), cz = columnCZ(col);
        for (int cy = 0; cy < cfg::CHUNK_LAYERS; cy++) c.seen.erase(chunkKey(cx, cy, cz));
        std::deque<int64_t> keep;
        for (int64_t q : c.baseQ)
            if (q != col) keep.push_back(q);
        c.baseQ.swap(keep);
        c.baseQueued.erase(col);
    }
    for (int64_t col : now) {
        if (c.cols.insert(col).second) queueBase(c, col, false);
        int cx = columnCX(col), cz = columnCZ(col);
        for (int cy = 0; cy < cfg::CHUNK_LAYERS; cy++) {
            if (!world.chunkExists(cx, cy, cz)) continue;
            int64_t key = chunkKey(cx, cy, cz);
            if (!c.seen[key].base) queueBase(c, col, false);
        }
    }

    int full = 0;
    int headers = 0;
    while (!c.baseQ.empty() && headers < 16) {
        int64_t col = c.baseQ.front();
        c.baseQ.pop_front();
        c.baseQueued.erase(col);
        if (!c.cols.count(col)) continue;
        int cx = columnCX(col), cz = columnCZ(col);
        bool requeue = false;
        for (int cy = 0; cy < cfg::CHUNK_LAYERS; cy++) {
            if (!world.chunkExists(cx, cy, cz)) continue;
            int64_t key = chunkKey(cx, cy, cz);
            SeenSlice& seen = c.seen[key];
            if (seen.base) continue;
            auto sit = srv.find(key);
            bool diverged = sit != srv.end() && sit->second.diverged;
            uint32_t rev = sit == srv.end() ? 0u : sit->second.rev;
            if (diverged) {
                if (full >= 1) { requeue = true; break; }
                World::Chunk* ch = world.getChunk(cx, cy, cz);
                if (!ch) continue;
                ChunkBaseNet base;
                base.cx = cx;
                base.cy = cy;
                base.cz = cz;
                base.rev = rev;
                base.pristine = false;
                base.blocks = ch->blocks;
                base.water = ch->waterLevel;
                base.flags = ch->flags;
                base.sod.reserve(ch->sodFaces.size());
                for (const World::Chunk::SodFace& sf : ch->sodFaces) {
                    if (sf.face >= 6) continue;
                    base.sod.push_back(AuthSodNet{ sf.x, sf.z, sf.y, sf.face, sf.stage, World::quantSodRem(sf.rem) });
                }
                base.bark.reserve(ch->barkFaces.size());
                for (const World::Chunk::BarkFace& bf : ch->barkFaces)
                    base.bark.push_back(AuthBarkNet{ bf.x, bf.z, bf.y, bf.face });
                d.bases.push_back(std::move(base));
                full++;
            } else {
                ChunkBaseNet base;
                base.cx = cx;
                base.cy = cy;
                base.cz = cz;
                base.rev = 0;
                base.pristine = true;
                d.bases.push_back(base);
            }
            seen.base = true;
            seen.rev = rev;
            headers++;
        }
        if (requeue) queueBase(c, col, true);
        break;
    }

    int sentChunks = 0;
    for (auto& kv : c.seen) {
        if (!kv.second.base || sentChunks >= 24) continue;
        auto sit = srv.find(kv.first);
        if (sit == srv.end() || sit->second.rev <= kv.second.rev) continue;
        bool progressed = false;
        for (const RevDelta& rd : sit->second.log) {
            if (rd.rev <= kv.second.rev) continue;
            if (rd.rev != kv.second.rev + 1) break;
            ChunkDeltaNet delta;
            delta.cx = chunkCX(kv.first);
            delta.cy = chunkCY(kv.first);
            delta.cz = chunkCZ(kv.first);
            delta.rev = rd.rev;
            delta.cells.reserve(rd.cells.size());
            for (const World::AuthCell& cell : rd.cells)
                delta.cells.push_back(AuthCellNet{ cell.x, cell.y, cell.z, cell.block, cell.water, cell.flags });
            delta.sod = rd.sod;
            if (rd.sod) {
                delta.sods.reserve(rd.sods.size());
                for (const World::AuthSod& s : rd.sods)
                    delta.sods.push_back(AuthSodNet{ s.x, s.z, s.y, s.face, s.stage, s.rem });
            }
            delta.bark = rd.bark;
            if (rd.bark) {
                delta.barks.reserve(rd.barks.size());
                for (const World::AuthBark& bk : rd.barks)
                    delta.barks.push_back(AuthBarkNet{ bk.x, bk.z, bk.y, bk.face });
            }
            d.deltas.push_back(std::move(delta));
            kv.second.rev = rd.rev;
            progressed = true;
        }
        if (kv.second.rev < sit->second.rev) {
            bool hasNext = false;
            for (const RevDelta& rd : sit->second.log)
                if (rd.rev == kv.second.rev + 1) hasNext = true;
            if (!hasNext) {
                kv.second.base = false;
                queueBase(c, columnKey(chunkCX(kv.first), chunkCZ(kv.first)), true);
            }
        }
        if (progressed) sentChunks++;
    }

    if (serverTick % (uint32_t)kHashEvery == 0) {
        std::vector<int64_t> keys;
        for (const auto& kv : c.seen) {
            if (!kv.second.base) continue;
            auto sit = srv.find(kv.first);
            uint32_t rev = sit == srv.end() ? 0u : sit->second.rev;
            if (kv.second.rev != rev) continue;
            int cx = chunkCX(kv.first), cy = chunkCY(kv.first), cz = chunkCZ(kv.first);
            if (!world.chunkExists(cx, cy, cz)) continue;
            keys.push_back(kv.first);
        }
        std::sort(keys.begin(), keys.end());
        if (!keys.empty()) {
            if (c.hashCursor < 0 || c.hashCursor >= (int)keys.size()) c.hashCursor = 0;
            int n = 0;
            for (int i = 0; i < (int)keys.size() && n < kHashBatch; i++) {
                int64_t key = keys[(c.hashCursor + i) % (int)keys.size()];
                World::Chunk* ch = world.getChunk(chunkCX(key), chunkCY(key), chunkCZ(key));
                if (!ch) continue;
                ChunkHashNet chk;
                chk.cx = chunkCX(key);
                chk.cy = chunkCY(key);
                chk.cz = chunkCZ(key);
                chk.rev = c.seen[key].rev;
                chk.hash = world.chunkAuthHash(*ch);
                d.checks.push_back(chk);
                n++;
            }
            if (n > 0) c.hashCursor = (c.hashCursor + n) % (int)keys.size();
        }
    }
}

int chunkCol(float p, int size) {
    return floorDiv((int)std::floor(p / cfg::BLOCK_SCALE), size);
}

TreeNet toWire(const World::NetTree& n) {
    TreeNet t;
    t.id = n.id;
    t.rev = n.rev;
    t.cells = n.cells;
    t.ox = n.ox;
    t.oy = n.oy;
    t.oz = n.oz;
    t.cx = n.com.x;
    t.cy = n.com.y;
    t.cz = n.com.z;
    t.vx = n.vel.x;
    t.vy = n.vel.y;
    t.vz = n.vel.z;
    t.wx = n.omega.x;
    t.wy = n.omega.y;
    t.wz = n.omega.z;
    t.ax = n.ax.x;
    t.ay = n.ax.y;
    t.az = n.ax.z;
    t.bx = n.ay.x;
    t.by = n.ay.y;
    t.bz = n.ay.z;
    t.dx = n.az.x;
    t.dy = n.az.y;
    t.dz = n.az.z;
    t.px = n.pivot.x;
    t.py = n.pivot.y;
    t.pz = n.pivot.z;
    t.hold = n.hold;
    t.still = n.still;
    t.body.reserve(n.body.size());
    for (const World::NetTreeCell& c : n.body)
        t.body.push_back(TreeCellNet{ c.x, c.y, c.z, c.block, c.flags });
    return t;
}

bool treeVisible(const SClient& c, const PhysicsIsland& t) {
    int pcx = chunkCol(c.x, cfg::CHUNK_X);
    int pcy = chunkCol(c.y, cfg::CHUNK_Y);
    int pcz = chunkCol(c.z, cfg::CHUNK_Z);
    int cx = chunkCol(t.com.x, cfg::CHUNK_X);
    int cy = chunkCol(t.com.y, cfg::CHUNK_Y);
    int cz = chunkCol(t.com.z, cfg::CHUNK_Z);
    return std::max(std::abs(cx - pcx), std::max(std::abs(cy - pcy), std::abs(cz - pcz))) <= cfg::LOAD_RADIUS;
}

void fillTrees(World& world, SClient& c, PlayDeltaNet& d, uint32_t serverTick) {
    std::unordered_map<uint32_t, const PhysicsIsland*> live;
    for (const PhysicsIsland& t : world.physicsIslands()) {
        if (t.netId == 0 || !treeVisible(c, t)) continue;
        live[t.netId] = &t;
    }
    int gone = 0;
    std::vector<uint32_t> drop;
    for (const auto& kv : c.trees)
        if (!live.count(kv.first)) drop.push_back(kv.first);
    for (uint32_t id : drop) {
        if (gone >= 32) break;
        d.treesGone.push_back(id);
        c.trees.erase(id);
        gone++;
    }
    int fulls = 0;
    bool waiting = false;
    std::vector<const PhysicsIsland*> order;
    order.reserve(live.size());
    for (const auto& kv : live) order.push_back(kv.second);
    auto needsBody = [&](const PhysicsIsland* t) {
        auto it = c.trees.find(t->netId);
        return c.treeResync || it == c.trees.end() || it->second.rev != t->contentRev;
    };
    std::sort(order.begin(), order.end(), [&](const PhysicsIsland* a, const PhysicsIsland* b) {
        bool au = needsBody(a);
        bool bu = needsBody(b);
        if (au != bu) return au;
        return a->netId < b->netId;
    });
    for (const PhysicsIsland* tp : order) {
        if (d.trees.size() >= 32) { waiting = true; break; }
        const PhysicsIsland& t = *tp;
        auto seen = c.trees.find(t.netId);
        bool unseen = seen == c.trees.end();
        bool stale = !unseen && seen->second.rev != t.contentRev;
        bool needFull = c.treeResync || unseen || stale;
        if (needFull && fulls >= 1) {
            waiting = true;
            if (unseen) continue;
        }
        bool sendFull = needFull && fulls < 1;
        World::NetTree nt;
        world.exportNetTree(t, nt, sendFull);
        d.trees.push_back(toWire(nt));
        if (sendFull) {
            c.trees[t.netId].rev = t.contentRev;
            fulls++;
        }
    }
    if (c.treeResync && !waiting) c.treeResync = false;
    if (serverTick % (uint32_t)kHashEvery != 0) return;
    std::vector<uint32_t> ids;
    ids.reserve(live.size());
    for (const auto& kv : live) {
        auto seen = c.trees.find(kv.first);
        if (seen == c.trees.end() || seen->second.rev != kv.second->contentRev) return;
        ids.push_back(kv.first);
    }
    d.treeCheck = true;
    d.treeHash = world.treeAuthHashOf(ids);
}

bool mineVisible(const SClient& c, const World& world, const World::MineView& v) {
    if (v.tree != 0) {
        for (const PhysicsIsland& t : world.physicsIslands())
            if (t.netId == v.tree) return treeVisible(c, t);
        return false;
    }
    int pcx = chunkCol(c.x, cfg::CHUNK_X);
    int pcy = chunkCol(c.y, cfg::CHUNK_Y);
    int pcz = chunkCol(c.z, cfg::CHUNK_Z);
    int cx = floorDiv(v.x, cfg::CHUNK_X);
    int cy = floorDiv(v.y, cfg::CHUNK_Y);
    int cz = floorDiv(v.z, cfg::CHUNK_Z);
    int dx = std::abs(cx - pcx);
    int dy = std::abs(cy - pcy);
    int dz = std::abs(cz - pcz);
    return std::max(dx, std::max(dy, dz)) <= cfg::LOAD_RADIUS;
}

void fillMines(World& world, SClient& c, PlayDeltaNet& d) {
    std::vector<World::MineView> all;
    world.collectMineViews(all);
    std::unordered_set<MineId, MineIdHash> now;
    for (const World::MineView& v : all) {
        if (!mineVisible(c, world, v)) continue;
        MineId id{ v.tree, v.x, v.y, v.z };
        now.insert(id);
        auto seen = c.mines.find(id);
        if (seen != c.mines.end() && seen->second.serial == v.serial) continue;
        if (d.mines.size() >= 32) continue;
        MineNet m;
        m.tree = v.tree;
        m.x = v.x;
        m.y = v.y;
        m.z = v.z;
        m.rem = v.rem;
        m.serial = v.serial;
        for (int i = 0; i < v.nHits && i < World::kMaxMineHits; i++)
            m.hits.push_back(MineHitNet{ v.hits[i].step, v.hits[i].face });
        d.mines.push_back(std::move(m));
        c.mines[id].serial = v.serial;
    }
    std::vector<MineId> drop;
    for (const auto& kv : c.mines)
        if (!now.count(kv.first)) drop.push_back(kv.first);
    for (const MineId& id : drop) {
        if (d.mineGone.size() >= 32) break;
        d.mineGone.push_back(MineGoneNet{ id.tree, id.x, id.y, id.z });
        c.mines.erase(id);
    }
}

bool chunksVisible(const SClient& a, const SClient& b) {
    int dx = std::abs(chunkCol(a.x, cfg::CHUNK_X) - chunkCol(b.x, cfg::CHUNK_X));
    int dz = std::abs(chunkCol(a.z, cfg::CHUNK_Z) - chunkCol(b.z, cfg::CHUNK_Z));
    return std::max(dx, dz) <= cfg::LOAD_RADIUS;
}

void stepAvatar(SClient& c) {
    if (c.spectator || !c.hasInput) {
        c.animName = "idle";
        c.animClock = 0;
        c.animClip = anim::kNetIdle;
        c.animFrameQ = 0;
        c.strikeClip = anim::kNetNone;
        c.strikeFrameQ = 0;
        return;
    }
    const anim::PlayerClips& lib = anim::playerClips();
    float hs = std::hypot(c.vx, c.vz);
    bool moving = !c.flying && hs > 0.35f;
    bool running = moving && c.sprinting;
    const anim::Clip& clip = !moving ? lib.idle : (running ? lib.run : lib.walk);
    float scale = 1.0f;
    if (moving) {
        float ref = running ? cfg::SPRINT_SPEED : cfg::WALK_SPEED;
        if (ref > 0.1f) scale = clampf(hs / ref, 0.45f, 1.35f);
    }
    anim::Playback pb{ c.animName, c.animClock };
    anim::advance(pb, clip, 1.0f / (float)cfg::TICKS_PER_SECOND, scale);
    c.animName = pb.name;
    c.animClock = pb.clock;
    c.animClip = anim::netId(clip);
    c.animFrameQ = anim::quantizeFrame(pb.clock);

    c.strikeClip = anim::kNetNone;
    c.strikeFrameQ = 0;
    const char* sname = nullptr;
    if (c.strikeKind == kStrikePunch) sname = "punch";
    else if (c.strikeKind == kStrikeAxe) sname = "axe_chop";
    else if (c.strikeKind == kStrikePick) sname = "pick_mine";
    if (sname) {
        anim::StrikeView sv = anim::resolveStrike(sname, c.strikeCharge, c.strikeCool,
                                                   c.mineCharge, c.mineCooldown, c.pickRaised);
        if (sv.clip) {
            c.strikeClip = anim::netId(*sv.clip);
            c.strikeFrameQ = anim::quantizeFrame(sv.frame);
        }
    }
}

PlayerPoseNet poseOf(const SClient& o, uint32_t serverTick) {
    PlayerPoseNet pose;
    pose.id = o.id;
    pose.name = o.name;
    pose.x = o.x;
    pose.y = o.y;
    pose.z = o.z;
    pose.yaw = o.yaw;
    pose.pitch = o.pitch;
    pose.bodyYaw = o.bodyYaw;
    pose.spectator = o.spectator;
    pose.clip = o.animClip;
    pose.frameQ = o.animFrameQ;
    pose.strike = o.strikeClip;
    pose.strikeQ = o.strikeFrameQ;
    pose.heldL = o.heldL;
    pose.heldR = o.heldR;
    pose.carried = o.carried;
    pose.wearU = o.wearU;
    pose.wearL = o.wearL;
    pose.wearS = o.wearS;
    for (int i = 0; i < vitals::Count; ++i) pose.health[i] = o.body.player.vitals.limb[i].health;
    for (const auto& status : o.burning)
        if (arcane::burning(status, serverTick)) { pose.status |= kStatusBurning; break; }
    if (arcane::frozen(o.frozen, serverTick)) pose.status |= kStatusFrozen;
    if (serverTick < o.healFlashUntil) pose.status |= kStatusHealing;
    pose.hitFlash = serverTick < o.hitFlashUntil ? 1 : 0;
    pose.dead = o.body.player.dead;
    return pose;
}

} // namespace

int runRoomServer(uint16_t port, const std::string& handoffUtf8) {
    WSADATA w{};
    if (WSAStartup(MAKEWORD(2, 2), &w) != 0) return 1;

    FILE* log = fopen("room_server.log", "w");
    slog(log, "room server starting");

    std::vector<RoomTeamNet> teams;
    std::vector<RoomPlayerNet> roster;
    readRoomHandoff(handoffUtf8, teams, roster);
    std::vector<Seat> seats;
    for (const RoomPlayerNet& p : roster) seats.push_back(Seat{ p, false });

    uint32_t seed = (uint32_t)std::chrono::high_resolution_clock::now().time_since_epoch().count();
    try {
        std::random_device rd;
        seed ^= rd();
    } catch (...) {
    }
    if (seed == 0) seed = 1;

    World world(seed);
    world.setSaveEnabled(false);
    world.reset(seed);
    world.setMatchBounds(true);
    world.setAuthCapture(true);
    world.setKeepEdited(true);
    world.setTagTrees(true);

    const float S = cfg::BLOCK_SCALE;
    int sh = world.surfaceHeight(8, 8);
    Vec3 base{ 8.5f * S, (float)(sh + 3) * S, 8.5f * S };
    char line[128];
    snprintf(line, sizeof(line), "seed %u port %u", seed, (unsigned)port);
    slog(log, line);

    SOCKET listenSock = INVALID_SOCKET;
    long long bindDeadline = nowMs() + 5000;
    for (;;) {
        listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listenSock == INVALID_SOCKET) {
            slog(log, "socket failed");
            if (log) fclose(log);
            WSACleanup();
            return 1;
        }
        int reuse = 1;
        setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(port);
        if (bind(listenSock, (sockaddr*)&addr, sizeof(addr)) == 0 && listen(listenSock, 8) == 0) break;
        closesocket(listenSock);
        listenSock = INVALID_SOCKET;
        if (nowMs() > bindDeadline) {
            slog(log, "bind failed");
            if (log) fclose(log);
            WSACleanup();
            return 1;
        }
        Sleep(50);
    }
    {
        u_long mode = 1;
        ioctlsocket(listenSock, FIONBIO, &mode);
    }
    slog(log, "listening");

    std::vector<SClient> clients;
    std::unordered_map<int64_t, SrvChunk> srvChunks;
    uint32_t nextId = 1;
    int spawnSlot = 0;
    bool worldReady = false;
    bool hadClient = false;
    long long emptySince = nowMs();
    long long started = emptySince;
    long long tickClock = started;
    float tickAcc = 0.0f;
    uint32_t serverTick = 0;
    std::deque<std::pair<uint32_t, CombatEventNet>> combatEvents;
    uint32_t nextCombatSerial = 1;
    std::vector<arcane::Projectile> projectiles;
    uint32_t nextProjectileId = 1;
    std::deque<std::pair<uint32_t, ArcaneEventNet>> arcaneEvents;
    uint32_t nextArcaneSerial = 1;
    // Building generation will bind its placed clue drops to this match-owned
    // director. Until it does, no unbound ITEM_CLUE can advance progression.
    clue::Director clueDirector;

    // Ritual altar placement: tracks which altars have been placed and team spawn info
    bool ritualAltarsPlaced = false;
    int ritualAltarTeamSpawns[matchmap::kCombatTeams][3] = {}; // [team][x, y, z]
    bool ritualAltarUsedForTeam[matchmap::kCombatTeams] = {};   // whether team has an altar

    auto broadcastDeploy = [&]() {
        for (SClient& c : clients) {
            if (!c.sentWelcome || c.team < 1 || c.team > matchmap::kCombatTeams) continue;
            std::vector<DeployPinNet> pins;
            for (const SClient& o : clients) {
                if (!o.sentWelcome || o.team != c.team) continue;
                DeployPinNet pin;
                pin.id = o.id;
                pin.name = o.name;
                pin.bx = o.dbx;
                pin.bz = o.dbz;
                if (o.pin && !o.landed) {
                    pin.phase = 1;
                    pin.t = o.pinLeft;
                    pins.push_back(pin);
                } else if (o.landed && o.fade > 0.01f) {
                    pin.phase = 2;
                    pin.t = o.fade;
                    pins.push_back(pin);
                }
            }
            c.conn.send((uint16_t)RoomMsg::DeploySync, encodeDeploySync(pins));
            c.conn.pump();
        }
    };

    while (true) {
        for (;;) {
            SOCKET c = accept(listenSock, nullptr, nullptr);
            if (c == INVALID_SOCKET) break;
            setNonBlock(c);
            SClient cl;
            cl.conn = NetConn((uintptr_t)c);
            clients.push_back(std::move(cl));
        }

        int live = 0;
        for (size_t i = 0; i < clients.size();) {
            SClient& c = clients[i];
            c.conn.pump();
            if (c.conn.dead()) {
                c.conn.close();
                clients.erase(clients.begin() + (int)i);
                continue;
            }
            uint16_t type = 0;
            std::vector<uint8_t> payload;
            while (c.conn.pop(type, payload)) {
                const uint8_t* p = payload.data();
                const uint8_t* end = p + payload.size();
                if (!c.known && type == (uint16_t)RoomMsg::PlayHello) {
                    std::string name;
                    if (!decodeHello(p, end, name)) {
                        c.conn.close();
                        break;
                    }
                    if (name.empty()) name = "玩家";
                    c.name = name;
                    c.team = -1;
                    for (Seat& seat : seats) {
                        if (seat.used || seat.player.name != name) continue;
                        seat.used = true;
                        c.team = seat.player.team;
                        break;
                    }
                    c.spectator = (c.team == 0);
                    if (!c.spectator && c.team >= 1 && c.team <= matchmap::kCombatTeams) {
                        room_inventory::add(c.inventory, ITEM_GUIDE_BOOK, 1);
                        if (clueDirector.targetFor(c.team).active)
                            room_inventory::add(c.inventory, ITEM_CLUE, 1);
                    }
                    c.id = nextId++;
                    c.known = true;
                    hadClient = true;
                    c.landed = c.spectator || c.team < 1 || c.team > matchmap::kCombatTeams;
                    c.pin = false;
                    c.fade = 0.0f;
                    int bx = (int)std::floor(base.x / S);
                    int bz = (int)std::floor(base.z / S);
                    if (!c.landed) {
                        matchmap::Zone zone = matchmap::combatZone(c.team - 1);
                        bx = zone.cx0 * cfg::CHUNK_X + (matchmap::kZoneChunks * cfg::CHUNK_X) / 2;
                        bz = zone.cz0 * cfg::CHUNK_Z + (matchmap::kZoneChunks * cfg::CHUNK_Z) / 2;
                    }
                    int by = world.surfaceHeight(bx, bz);
                    float ox = 0.0f, oz = 0.0f;
                    if (c.landed) {
                        ox = (float)(spawnSlot % 4) * 1.5f;
                        oz = (float)(spawnSlot / 4) * 1.5f;
                        spawnSlot++;
                    }
                    c.x = (bx + 0.5f) * S + ox;
                    c.y = (float)(by + 3) * S;
                    c.z = (bz + 0.5f) * S + oz;
                    c.landX = c.x;
                    c.landY = c.y;
                    c.landZ = c.z;
                    c.yaw = 0.4f;
                    c.pitch = -0.15f;
                    room_body::spawn(c.body, {c.x, c.y, c.z});
                    c.body.player.yaw = c.yaw;
                    c.body.player.pitch = c.pitch;
                    // DEBUG: log spawn point to file
                    snprintf(line, sizeof(line), "[DEBUG] Player %s spawned at team=%d, bx=%d, by=%d, bz=%d", c.name.c_str(), c.team, bx, by, bz);
                    slog(log, line);
                    // Place ritual altars near team spawn points (only once, when first player joins)
                    if (!ritualAltarsPlaced && !c.spectator && c.team >= 1 && c.team <= matchmap::kCombatTeams) {
                        snprintf(line, sizeof(line), "[DEBUG] Entering altar placement for team=%d", c.team);
                        slog(log, line);
                        // Record this team's spawn point
                        ritualAltarTeamSpawns[c.team - 1][0] = bx;
                        ritualAltarTeamSpawns[c.team - 1][1] = by;
                        ritualAltarTeamSpawns[c.team - 1][2] = bz;
                        ritualAltarUsedForTeam[c.team - 1] = true;
                        // Place altars when first player joins
                        // Count active teams
                        int activeTeams = 0;
                        for (int i = 0; i < matchmap::kCombatTeams; i++) {
                            if (ritualAltarUsedForTeam[i]) activeTeams++;
                        }
                        // Use a simple seeded RNG based on server seed
                        uint32_t rngSeed = seed ^ (uint32_t)(c.team * 7919);
                        auto rng = [&]() {
                            rngSeed = rngSeed * 1664525u + 1013904223u;
                            return rngSeed >> 16;
                        };
                        // Assign altars to all active teams (one per team)
                        for (int i = 0; i < matchmap::kCombatTeams; i++) {
                            if (!ritualAltarUsedForTeam[i]) continue;
                            int altarIndex = i % structure::kRitualAltarCount;
                            int wx = ritualAltarTeamSpawns[i][0];
                            int wy = ritualAltarTeamSpawns[i][1];
                            int wz = ritualAltarTeamSpawns[i][2];
                            // Place altar 3-5 blocks away from spawn point
                            int offsetX = (rng() % 5) - 2;
                            int offsetZ = (rng() % 5) - 2;
                            int finalX = wx + offsetX, finalZ = wz + offsetZ;
                            // Ensure the column is loaded before placing altar
                            int colX = finalX / cfg::CHUNK_X;
                            int colZ = finalZ / cfg::CHUNK_Z;
                            if (finalX < 0 && finalX % cfg::CHUNK_X != 0) colX--;
                            if (finalZ < 0 && finalZ % cfg::CHUNK_Z != 0) colZ--;
                            world.ensureColumn(colX, colZ);
                            std::string altarPath = "assets/structures/" + std::string(structure::ritualAltarName(altarIndex)) + ".vlstruct";
                            bool fileExists = std::filesystem::exists(altarPath);
                            snprintf(line, sizeof(line), "[DEBUG] Placing altar %d (%s) for team %d at wx=%d, wy=%d, wz=%d col=(%d,%d) fileExists=%d",
                                altarIndex, structure::ritualAltarName(altarIndex), i + 1, finalX, wy, finalZ, colX, colZ, fileExists ? 1 : 0);
                            slog(log, line);
                            structure::paintRitualAltar(world, finalX, wy, finalZ, altarIndex);
                        }
                        // Place remaining random altars to fill up to kRitualAltarCount (6)
                        for (int extra = activeTeams; extra < structure::kRitualAltarCount; extra++) {
                            // Pick a random team zone for the extra altar
                            int zoneIdx = rng() % matchmap::kCombatTeams;
                            matchmap::Zone zone = matchmap::combatZone(zoneIdx);
                            int wx = zone.cx0 * cfg::CHUNK_X + (matchmap::kZoneChunks * cfg::CHUNK_X) / 2;
                            int wz = zone.cz0 * cfg::CHUNK_Z + (matchmap::kZoneChunks * cfg::CHUNK_Z) / 2;
                            int wy = world.surfaceHeight(wx, wz);
                            if (wy < 1) wy = 1;
                            // Offset from zone center
                            int offsetX = (rng() % 8) - 4;
                            int offsetZ = (rng() % 8) - 4;
                            int finalX = wx + offsetX, finalZ = wz + offsetZ;
                            // Ensure the column is loaded before placing altar
                            int colX = finalX / cfg::CHUNK_X;
                            int colZ = finalZ / cfg::CHUNK_Z;
                            if (finalX < 0 && finalX % cfg::CHUNK_X != 0) colX--;
                            if (finalZ < 0 && finalZ % cfg::CHUNK_Z != 0) colZ--;
                            world.ensureColumn(colX, colZ);
                            int altarIdx = extra % structure::kRitualAltarCount;
                            std::string altarPath = "assets/structures/" + std::string(structure::ritualAltarName(altarIdx)) + ".vlstruct";
                            bool fileExists = std::filesystem::exists(altarPath);
                            snprintf(line, sizeof(line), "[DEBUG] Placing extra altar %d (%s) at wx=%d, wy=%d, wz=%d col=(%d,%d) fileExists=%d",
                                altarIdx, structure::ritualAltarName(altarIdx), finalX, wy, finalZ, colX, colZ, fileExists ? 1 : 0);
                            slog(log, line);
                            structure::paintRitualAltar(world, finalX, wy, finalZ, altarIdx);
                        }
                        slog(log, "[DEBUG] All ritual altars placed!");
                        ritualAltarsPlaced = true;
                    }
                } else if (c.sentWelcome && type == (uint16_t)RoomMsg::PlayInput) {
                    PlayInputNet in;
                    if (!decodePlayInput(p, end, in)) continue;
                    if (!room_body::accept(c.body, in, serverTick)) continue;
                    c.yaw = c.body.player.yaw;
                    c.pitch = c.body.player.pitch;
                    // Spectator coordinates drive streaming only, never combat.
                    if (c.spectator) {
                        if (!std::isfinite(in.x) || !std::isfinite(in.y) || !std::isfinite(in.z) ||
                            std::fabs(in.x) > 100000 || std::fabs(in.y) > 100000 || std::fabs(in.z) > 100000) continue;
                        c.x = in.x; c.y = in.y; c.z = in.z;
                    }
                    c.selectedLeft = in.selectedLeft;
                    c.selectedRight = in.selectedRight;
                    if (in.layoutSequence)
                        room_inventory::acceptLayout(c.inventory, in.layoutSequence,
                                                     in.layoutBaseRevision, in.layout);
                    if (in.pickupSequence && !c.spectator && c.landed && !c.body.player.dead) {
                        const loot::Drop* pendingDrop = world.dropById(in.pickupDrop);
                        bool clueDrop = pendingDrop && pendingDrop->item == ITEM_CLUE;
                        bool routeAllows = !clueDrop || clueDirector.canPickup(in.pickupDrop, c.team);
                        if (routeAllows && room_inventory::pickup(c.inventory, in.pickupSequence, world,
                                in.pickupDrop, c.body.player.eye(), c.body.player.pos) && clueDrop &&
                            clueDirector.claimPickup(in.pickupDrop, c.team)) {
                            for (SClient& teammate : clients) {
                                if (!teammate.known || teammate.team != c.team ||
                                    inventoryHas(teammate.inventory, ITEM_CLUE)) continue;
                                room_inventory::add(teammate.inventory, ITEM_CLUE, 1);
                            }
                        }
                    }
                    c.heldL = room_inventory::held(c.inventory, c.body.player.vitals, c.selectedLeft);
                    c.heldR = room_inventory::held(c.inventory, c.body.player.vitals,
                                                   cfg::HAND_SLOTS + c.selectedRight);
                    c.carried = in.carried;
                    c.wearU = in.wearU;
                    c.wearL = in.wearL;
                    c.wearS = in.wearS;
                    c.combatAck = in.combatAck;
                    c.arcaneAck = in.arcaneAck;
                    bool guardianBusy = c.guardianMelee.pending ||
                        uint32_t(c.guardianMelee.readyAt - serverTick) < 0x80000000u;
                    bool combatBusy = guardianBusy || c.melee.pending ||
                        uint32_t(c.melee.readyAt - serverTick) < 0x80000000u;
                    if (!combatBusy) {
                        c.strikeKind = in.strikeKind;
                        c.strikeCharge = in.strikeCharge;
                        c.strikeCool = in.strikeCool;
                        c.mineCharge = in.mineCharge;
                        c.mineCooldown = in.mineCooldown;
                        c.pickRaised = in.pickRaised;
                    }
                    bool attackAccepted = false;
                    if (in.attackSequence) {
                        combat::Hand hand = in.attackHand ? combat::Hand::Right : combat::Hand::Left;
                        int slot = hand == combat::Hand::Left ? c.selectedLeft : cfg::HAND_SLOTS + c.selectedRight;
                        uint8_t owned = room_inventory::held(c.inventory, c.body.player.vitals, slot);
                        combat::Actor actor{c.id, c.team,
                            c.landed && c.fade <= 0 && !c.spectator && !c.body.player.dead,
                            combat::EntityCategory::Player};
                        if (combat::beginMelee(c.melee, in.attackSequence, serverTick, actor,
                                               c.body.player.vitals, hand, owned, guardianBusy)) {
                            attackAccepted = true;
                            c.attackStartedAt = serverTick;
                            c.strikeKind = owned == HAND_AXE ? kStrikeAxe : kStrikePick;
                            c.pickRaised = false;
                        }
                    }
                    bool castAccepted = false;
                    if (in.castSequence) {
                        combat::Hand hand = in.castHand ? combat::Hand::Right : combat::Hand::Left;
                        int slot = hand == combat::Hand::Left ? c.selectedLeft : cfg::HAND_SLOTS + c.selectedRight;
                        uint8_t owned = room_inventory::held(c.inventory, c.body.player.vitals, slot);
                        combat::Actor actor{c.id, c.team,
                            c.landed && c.fade <= 0 && !c.spectator && !c.body.player.dead,
                            combat::EntityCategory::Player};
                        bool blocked = combatBusy || attackAccepted || c.melee.pending ||
                            in.mineCharge > 0.0f || in.mineCooldown > 0.0f ||
                            in.carried != AIR;
                        auto spawnProjectile = [&](bool freezeSpell) {
                            uint32_t projectileId = nextProjectileId++;
                            if (!nextProjectileId) nextProjectileId = 1;
                            Vec3 direction = c.body.player.lookDir();
                            auto projectile = freezeSpell
                                ? arcane::makeFreeze(projectileId, c.id, in.castSequence, c.team,
                                    c.body.player.eye() + direction * 0.45f, direction, serverTick)
                                : arcane::makeFireball(projectileId, c.id, in.castSequence, c.team,
                                    c.body.player.eye() + direction * 0.45f, direction, serverTick);
                            if (projectile.id) {
                                projectiles.push_back(projectile);
                                castAccepted = true;
                            }
                        };
                        if (owned == ITEM_ARCANE_FIREBALL &&
                            arcane::beginFireball(c.fireball, in.castSequence, serverTick, actor,
                                c.body.player.vitals, hand, owned, blocked || projectiles.size() >= 64) &&
                            room_inventory::consume(c.inventory, slot, ITEM_ARCANE_FIREBALL, 1)) {
                            spawnProjectile(false);
                        } else if (owned == ITEM_ARCANE_FREEZE &&
                            arcane::beginFreeze(c.freezeCast, in.castSequence, serverTick, actor,
                                c.body.player.vitals, hand, owned, blocked || projectiles.size() >= 64) &&
                            room_inventory::consume(c.inventory, slot, ITEM_ARCANE_FREEZE, 1)) {
                            spawnProjectile(true);
                        } else if (owned == ITEM_ARCANE_HEAL) {
                            SClient* healTarget = nullptr;
                            Vec3 origin = c.body.player.eye();
                            Vec3 direction = c.body.player.lookDir();
                            float obstruction = arcane::kHealRange + .01f;
                            IVec3 hitBlock{}, previous{}; Vec3 normal{};
                            world.raycast(origin, direction, arcane::kHealRange, hitBlock, previous,
                                          normal, nullptr, &obstruction);
                            std::optional<combat::LimbHit> bestHit;
                            for (SClient& target : clients) {
                                if (target.id == c.id || target.team != c.team || target.spectator ||
                                    !target.landed || target.fade > 0 || target.body.player.dead ||
                                    !combat::canHealPlayer(target.body.player.vitals)) continue;
                                auto hit = combat::rayPlayer(origin, direction, target.body.player.pos,
                                    target.body.player.bodyYaw, arcane::kHealRange, obstruction);
                                if (hit && (!bestHit || hit->distance < bestHit->distance)) {
                                    bestHit = hit;
                                    healTarget = &target;
                                }
                            }
                            if (!healTarget && combat::canHealPlayer(c.body.player.vitals)) healTarget = &c;
                            bool healBlocked = blocked || !healTarget;
                            if (arcane::beginHeal(c.healCast, in.castSequence, serverTick, actor,
                                    c.body.player.vitals, hand, owned, healBlocked) &&
                                room_inventory::consume(c.inventory, slot, ITEM_ARCANE_HEAL, 1) &&
                                combat::healPlayer(healTarget->body.player.vitals, arcane::kHealFraction)) {
                                castAccepted = true;
                                healTarget->healFlashUntil = serverTick + arcane::ticks(.8f);
                                ArcaneEventNet pulse;
                                pulse.serial = nextArcaneSerial++;
                                if (!nextArcaneSerial) nextArcaneSerial = 1;
                                pulse.kind = ArcaneEventKind::HealPulse;
                                Vec3 center = healTarget->body.player.pos + Vec3{0,.9f,0};
                                pulse.x = center.x; pulse.y = center.y; pulse.z = center.z;
                                arcaneEvents.push_back({serverTick, pulse});
                            }
                        }
                        if (castAccepted) {
                            c.strikeKind = kStrikePunch;
                            c.strikeCharge = 0.18f;
                            c.strikeCool = 0.25f;
                            c.mineCharge = 0.18f;
                            c.mineCooldown = 0.0f;
                            c.heldL = room_inventory::held(c.inventory, c.body.player.vitals, c.selectedLeft);
                            c.heldR = room_inventory::held(c.inventory, c.body.player.vitals,
                                                           cfg::HAND_SLOTS + c.selectedRight);
                        }
                    }
                    bool guardianAccepted = false;
                    if (in.guardianSequence) {
                        combat::Hand hand = in.attackHand ? combat::Hand::Right : combat::Hand::Left;
                        int slot = hand == combat::Hand::Left ? c.selectedLeft
                                                             : cfg::HAND_SLOTS + c.selectedRight;
                        uint8_t held = room_inventory::held(c.inventory, c.body.player.vitals, slot);
                        auto weapon = combat::weapon(held);
                        combat::Actor actor{c.id, c.team,
                            c.landed && c.fade <= 0 && !c.spectator && !c.body.player.dead,
                            combat::EntityCategory::Player};
                        float guardianT = weapon ? weapon->reach + .01f : 0.0f;
                        structure::GuardianSpan guardian;
                        bool aimed = weapon && structure::raycastGuardian(world, c.body.player.eye(),
                            c.body.player.lookDir(), weapon->reach, guardianT, guardian) &&
                            guardian.relic == in.guardianRelic;
                        bool blocked = combatBusy || attackAccepted || castAccepted || !aimed || in.carried != AIR;
                        if (combat::beginMelee(c.guardianMelee, in.guardianSequence, serverTick, actor,
                                              c.body.player.vitals, hand, held, blocked)) {
                            guardianAccepted = true;
                            c.guardianRelic = in.guardianRelic;
                            c.attackStartedAt = serverTick;
                            c.strikeKind = held == HAND_AXE ? kStrikeAxe : kStrikePick;
                            c.pickRaised = false;
                        }
                    }
                    c.hasInput = true;
                    if (!c.spectator && c.landed && !c.body.player.dead &&
                        !attackAccepted && !castAccepted && !guardianAccepted && !combatBusy &&
                        !c.melee.pending && !c.guardianMelee.pending) {
                        for (const PlayInputNet::MineEdit& e : in.mines)
                            if (e.tool == c.heldR) applyMine(world, e);
                        for (const BlockEditNet& e : in.edits) applyEdit(world, e);
                        for (const PlayInputNet::BarkEdit& e : in.bark) applyBark(world, e);
                    }
                    if (in.treeResync) c.treeResync = true;
                    for (const PlayInputNet::ChunkAsk& ask : in.resync) {
                        c.seen.erase(chunkKey(ask.cx, ask.cy, ask.cz));
                        queueBase(c, columnKey(ask.cx, ask.cz), true);
                    }
                } else if (c.sentWelcome && type == (uint16_t)RoomMsg::Deploy) {
                    uint8_t action = 0;
                    int bx = 0, bz = 0;
                    if (!decodeDeploy(p, end, action, bx, bz)) continue;
                    if (c.spectator || c.team < 1 || c.team > matchmap::kCombatTeams) continue;
                    if (action == 2) {
                        if (!c.body.player.dead) continue; // death is never a client declaration
                        c.landed = false;
                        c.deathDeploy = true;
                        c.pin = false;
                        c.pinLeft = 0.0f;
                        c.fade = 0.0f;
                        continue;
                    }
                    if (c.landed) continue;
                    if (action == 0) {
                        c.pin = false;
                        c.pinLeft = 0.0f;
                        continue;
                    }
                    if (action != 1 || !matchmap::blockInZone(c.team - 1, bx, bz)) continue;
                    c.pin = true;
                    c.dbx = bx;
                    c.dbz = bz;
                    c.pinLeft = c.deathDeploy ? matchmap::kDeployDeathSeconds : matchmap::kDeploySeconds;
                }
            }
            if (c.conn.dead()) {
                c.conn.close();
                clients.erase(clients.begin() + (int)i);
                continue;
            }
            if (c.sentWelcome) live++;
            i++;
        }

        if (!worldReady) {
            world.update(base, 2);
            if (world.loadedChunks() >= 32) {
                worldReady = true;
                slog(log, "world ready");
            }
        } else {
            std::vector<Vec3> anchors;
            for (const SClient& c : clients) {
                if (!c.sentWelcome) continue;
                anchors.push_back(Vec3{ c.x, c.y, c.z });
            }
            if (anchors.empty()) anchors.push_back(base);
            world.updateAnchors(anchors.data(), (int)anchors.size(), 1);
        }

        if (worldReady) {
            for (SClient& c : clients) {
                if (!c.known || c.sentWelcome) continue;
                c.conn.send((uint16_t)RoomMsg::PlayWelcome,
                            encodePlayWelcome(c.id, seed, c.x, c.y, c.z, c.spectator, c.team));
                c.sentWelcome = true;
                snprintf(line, sizeof(line), "welcome %u %s", c.id, c.name.c_str());
                slog(log, line);
            }
        }

        if (worldReady) {
            long long tnow = nowMs();
            float dt = (float)(tnow - tickClock) / 1000.0f;
            tickClock = tnow;
            if (dt < 0.0f) dt = 0.0f;
            if (dt > 0.1f) dt = 0.1f;
            tickAcc += dt;
            int steps = 0;
            while (tickAcc >= 1.0f / (float)cfg::TICKS_PER_SECOND && steps < 4) {
                tickAcc -= 1.0f / (float)cfg::TICKS_PER_SECOND;
                steps++;
                serverTick++;
                {
                    const float stepDt = 1.0f / (float)cfg::TICKS_PER_SECOND;
                    for (SClient& c : clients) {
                        if (!c.sentWelcome) continue;
                        if (c.pin && !c.landed) {
                            c.pinLeft -= stepDt;
                            if (c.pinLeft <= 0.0f) {
                                c.pinLeft = 0.0f;
                                c.pin = false;
                                c.landed = true;
                                c.deathDeploy = false;
                                c.fade = 1.0f;
                                int by = world.surfaceHeight(c.dbx, c.dbz);
                                c.landX = (c.dbx + 0.5f) * S;
                                c.landY = (float)(by + 3) * S;
                                c.landZ = (c.dbz + 0.5f) * S;
                                c.x = c.landX;
                                c.y = c.landY;
                                c.z = c.landZ;
                                c.vx = 0.0f;
                                c.vz = 0.0f;
                                room_body::spawn(c.body, {c.x, c.y, c.z});
                                c.burning.clear();
                                c.frozen = {};
                                c.healFlashUntil = 0;
                            }
                        } else if (c.landed && c.fade > 0.0f) {
                            c.fade -= stepDt / matchmap::kDeployFade;
                            if (c.fade < 0.0f) c.fade = 0.0f;
                        }
                    }
                }
                for (int sub = 0; sub < 6; sub++) world.treeFallPhysics(cfg::FIXED_DT);
                world.treeFallGameTick();
                world.updateDrops(1.0f / (float)cfg::TICKS_PER_SECOND);
                world.sodTick(2048, serverTick);
                world.waterTick(serverTick);
                noteAuth(srvChunks, world);
                for (SClient& c : clients) {
                    if (!c.sentWelcome) continue;
                    if (!c.spectator) {
                        room_body::tick(c.body, world, serverTick, c.landed, c.mineCharge > 0,
                                        arcane::frozen(c.frozen, serverTick));
                        const Player& p = c.body.player;
                        c.x = p.pos.x; c.y = p.pos.y; c.z = p.pos.z;
                        c.vx = p.vel.x; c.vz = p.vel.z;
                        c.bodyYaw = p.bodyYaw; c.flying = false; c.sprinting = p.sprinting;
                    }
                    if (c.body.player.dead) {
                        combat::cancelMelee(c.melee);
                        combat::cancelMelee(c.guardianMelee);
                    }
                    if (combat::takeMeleeHit(c.melee, serverTick)) {
                        combat::Hand hand = c.melee.hand;
                        int slot = hand == combat::Hand::Left ? c.selectedLeft : cfg::HAND_SLOTS + c.selectedRight;
                        uint8_t equipped = room_inventory::held(c.inventory, c.body.player.vitals, slot);
                        auto def = combat::weapon(c.melee.item);
                        combat::Actor attacker{c.id, c.team,
                            c.landed && c.fade <= 0 && !c.spectator && !c.body.player.dead,
                            combat::EntityCategory::Player};
                        if (def && equipped == c.melee.item && attacker.active) {
                            Vec3 origin = c.body.player.eye();
                            Vec3 direction = c.body.player.lookDir();
                            float obstruction = def->reach + .01f;
                            IVec3 block{}, previous{}; Vec3 normal{};
                            world.raycast(origin, direction, def->reach, block, previous, normal,
                                          nullptr, &obstruction);
                            SClient* bestTarget = nullptr;
                            std::optional<combat::LimbHit> bestHit;
                            for (SClient& target : clients) {
                                combat::Actor victim{target.id, target.team,
                                    target.landed && target.fade <= 0 && !target.spectator && !target.body.player.dead,
                                    combat::EntityCategory::Player};
                                if (!combat::hostile(attacker, victim)) continue;
                                auto hit = combat::rayPlayer(origin, direction, target.body.player.pos,
                                                             target.body.player.bodyYaw, def->reach, obstruction);
                                if (hit && (!bestHit || hit->distance < bestHit->distance)) {
                                    bestHit = hit; bestTarget = &target;
                                }
                            }
                            if (bestTarget && bestHit) {
                                combat::DamageSource source{c.id, c.melee.action, c.melee.item,
                                                            combat::DamageCategory::Physical};
                                auto result = combat::damagePlayer(bestTarget->body.player.vitals,
                                                                   source, def->damage, bestHit->limb);
                                if (result.applied) {
                                    bestTarget->hitFlashUntil = serverTick + 4;
                                    if (result.killed) {
                                        bestTarget->body.player.dead = true;
                                        bestTarget->body.player.vel = {};
                                    }
                                    CombatEventNet event;
                                    event.serial = nextCombatSerial++;
                                    if (!nextCombatSerial) nextCombatSerial = 1;
                                    event.kind = result.killed ? CombatEventKind::Death : CombatEventKind::Hit;
                                    event.attacker = c.id; event.target = bestTarget->id;
                                    event.limb = (int8_t)result.limb;
                                    event.amount = (uint16_t)std::min(65535, (int)std::lround(result.amount * 10000));
                                    combatEvents.push_back({serverTick, event});
                                }
                            }
                        }
                    }
                    if (combat::takeMeleeHit(c.guardianMelee, serverTick)) {
                        combat::Hand hand = c.guardianMelee.hand;
                        int slot = hand == combat::Hand::Left ? c.selectedLeft
                                                             : cfg::HAND_SLOTS + c.selectedRight;
                        uint8_t equipped = room_inventory::held(c.inventory, c.body.player.vitals, slot);
                        auto def = combat::weapon(c.guardianMelee.item);
                        combat::Actor attacker{c.id, c.team,
                            c.landed && c.fade <= 0 && !c.spectator && !c.body.player.dead,
                            combat::EntityCategory::Player};
                        if (def && equipped == c.guardianMelee.item && attacker.active &&
                            c.guardianRelic < ritual::RelicCount) {
                            float guardianT = def->reach + .01f;
                            structure::GuardianSpan guardian;
                            int gx = 0, gy = 0, gz = 0;
                            bool aimed = structure::raycastGuardian(world, c.body.player.eye(),
                                c.body.player.lookDir(), def->reach, guardianT, guardian) &&
                                guardian.relic == c.guardianRelic;
                            if (aimed && guardian_fight::vulnerable(c.guardianRelic) &&
                                structure::roomGuardianHit(world, c.body.player.eye(),
                                    c.guardianRelic, def->reach, gx, gy, gz)) {
                                int dmg = structure::guardianStrikeHurt(equipped, c.guardianRelic);
                                guardian_fight::noteDamage(c.guardianRelic, c.id, dmg);
                                if (structure::damageGuardian(world, c.guardianRelic, dmg, gx, gy, gz)) {
                                    world.setBlock(gx, gy, gz, AIR, false, false);
                                    uint8_t drop = (uint8_t)ritual::blockId(c.guardianRelic);
                                    const float Sdrop = cfg::BLOCK_SCALE;
                                    world.spawnDrop({ (gx + 0.5f) * Sdrop, (gy + 0.5f) * Sdrop,
                                                      (gz + 0.5f) * Sdrop }, drop, 1, true);
                                }
                            }
                        }
                    }
                    if (auto def = combat::weapon(c.melee.item)) {
                        bool recovering = uint32_t(c.melee.readyAt - serverTick) < 0x80000000u;
                        if (c.melee.pending) {
                            c.strikeCharge = def->windup;
                            c.strikeCool = def->recovery;
                            c.mineCharge = (float)uint32_t(serverTick - c.attackStartedAt) / cfg::TICKS_PER_SECOND;
                            c.mineCooldown = 0;
                        } else if (recovering) {
                            c.mineCharge = 0;
                            c.mineCooldown = (float)uint32_t(c.melee.readyAt - serverTick) / cfg::TICKS_PER_SECOND;
                        } else if (c.melee.action) {
                            c.strikeKind = kStrikeNone; c.mineCharge = c.mineCooldown = 0;
                            c.melee.item = AIR; c.melee.action = 0;
                        }
                    }
                    if (auto def = combat::weapon(c.guardianMelee.item)) {
                        bool recovering = uint32_t(c.guardianMelee.readyAt - serverTick) < 0x80000000u;
                        if (c.guardianMelee.pending) {
                            c.strikeCharge = def->windup;
                            c.strikeCool = def->recovery;
                            c.mineCharge = (float)uint32_t(serverTick - c.attackStartedAt) /
                                           cfg::TICKS_PER_SECOND;
                            c.mineCooldown = 0;
                        } else if (recovering) {
                            c.mineCharge = 0;
                            c.mineCooldown = (float)uint32_t(c.guardianMelee.readyAt - serverTick) /
                                             cfg::TICKS_PER_SECOND;
                        } else if (c.guardianMelee.action) {
                            c.strikeKind = kStrikeNone;
                            c.mineCharge = c.mineCooldown = 0;
                            c.guardianMelee.item = AIR;
                            c.guardianMelee.action = 0;
                            c.guardianRelic = 255;
                        }
                    }
                    stepAvatar(c);
                }
                for (size_t i = 0; i < projectiles.size();) {
                    arcane::Projectile& projectile = projectiles[i];
                    if (arcane::reached(serverTick, projectile.expireAt)) {
                        projectiles.erase(projectiles.begin() + (std::ptrdiff_t)i);
                        continue;
                    }
                    Vec3 old = projectile.pos;
                    Vec3 step = projectile.vel * (1.0f / (float)cfg::TICKS_PER_SECOND);
                    float distance = step.length();
                    Vec3 direction = distance > 1e-6f ? step / distance : Vec3{};
                    float obstruction = distance + 0.01f;
                    IVec3 block{}, previous{}; Vec3 normal{};
                    bool hitWorld = distance > 0.0f && world.raycast(old, direction, distance,
                        block, previous, normal, nullptr, &obstruction);
                    SClient* hitPlayer = nullptr;
                    float hitDistance = obstruction;
                    combat::Actor attacker{projectile.owner, projectile.team, true,
                                            combat::EntityCategory::Player};
                    float hitRadius = projectile.kind == arcane::ProjectileKind::Freeze
                        ? arcane::kFreezeHitRadius : arcane::kFireballHitRadius;
                    structure::GuardianSpan directGuardian;
                    float guardianDistance = distance + 0.01f;
                    bool hitGuardian = distance > 0.0f && structure::raycastGuardian(
                        world, old, direction, distance, guardianDistance, directGuardian, hitRadius) &&
                        guardianDistance < obstruction;
                    for (SClient& target : clients) {
                        combat::Actor victim{target.id, target.team,
                            target.landed && target.fade <= 0 && !target.spectator && !target.body.player.dead,
                            combat::EntityCategory::Player};
                        if (!combat::hostile(attacker, victim)) continue;
                        auto hit = combat::sweepPlayer(old, direction, target.body.player.pos,
                            target.body.player.bodyYaw, distance, obstruction, hitRadius);
                        if (hit && hit->distance < hitDistance) {
                            hitDistance = hit->distance;
                            hitPlayer = &target;
                        }
                    }
                    if (hitPlayer && hitGuardian) {
                        if (guardianDistance < hitDistance) hitPlayer = nullptr;
                        else hitGuardian = false;
                    }
                    if (hitWorld || hitPlayer || hitGuardian) {
                        float impactDistance = hitPlayer ? hitDistance
                            : (hitGuardian ? guardianDistance : obstruction);
                        Vec3 impact = old + direction * std::min(distance, impactDistance);
                        ArcaneEventNet burst;
                        burst.serial = nextArcaneSerial++;
                        if (!nextArcaneSerial) nextArcaneSerial = 1;
                        burst.kind = projectile.kind == arcane::ProjectileKind::Freeze
                            ? ArcaneEventKind::FreezeImpact : ArcaneEventKind::FireballExplode;
                        burst.x = impact.x; burst.y = impact.y; burst.z = impact.z;
                        arcaneEvents.push_back({serverTick, burst});

                        auto hurtGuardian = [&](const structure::GuardianSpan& guardian, int amount) {
                            if (amount <= 0 || guardian.hp <= 0) return;
                            if (!guardian_fight::vulnerable(guardian.relic)) return;
                            int gx = 0, gy = 0, gz = 0;
                            int dealt = std::min(amount, guardian.hp);
                            bool killed = structure::damageGuardian(world, guardian.relic, amount, gx, gy, gz);
                            guardian_fight::noteDamage(guardian.relic, projectile.owner, dealt);
                            if (killed) {
                                world.setBlock(gx, gy, gz, AIR, false, false);
                                uint8_t drop = (uint8_t)ritual::blockId(guardian.relic);
                                const float Sdrop = cfg::BLOCK_SCALE;
                                world.spawnDrop({(gx + 0.5f) * Sdrop, (gy + 0.5f) * Sdrop,
                                                 (gz + 0.5f) * Sdrop}, drop, 1, true);
                            }
                            CombatEventNet event;
                            event.serial = nextCombatSerial++;
                            if (!nextCombatSerial) nextCombatSerial = 1;
                            event.kind = killed ? CombatEventKind::Death : CombatEventKind::Hit;
                            event.attacker = projectile.owner;
                            event.target = 0; // guardian: no player network id
                            event.limb = -1;
                            event.amount = (uint16_t)std::min(65535, dealt);
                            combatEvents.push_back({serverTick, event});
                        };

                        if (projectile.kind == arcane::ProjectileKind::Freeze) {
                            if (hitGuardian) {
                                hurtGuardian(directGuardian, structure::guardianArcaneHurt(
                                    ITEM_ARCANE_FREEZE, directGuardian.relic));
                            } else if (hitPlayer) {
                                combat::DamageSource source{projectile.owner, projectile.action,
                                    ITEM_ARCANE_FREEZE, combat::DamageCategory::Frost};
                                auto result = combat::damagePlayer(hitPlayer->body.player.vitals, source,
                                    {arcane::kFreezeDamage, 1.0f, true}, -1);
                                if (result.applied) {
                                    hitPlayer->hitFlashUntil = serverTick + 4;
                                    if (result.killed) {
                                        hitPlayer->body.player.dead = true;
                                        hitPlayer->body.player.vel = {};
                                        hitPlayer->burning.clear();
                                        hitPlayer->frozen = {};
                                    } else {
                                        arcane::freeze(hitPlayer->frozen, projectile.owner,
                                                       projectile.action, serverTick);
                                        hitPlayer->body.player.vel.x = 0.0f;
                                        hitPlayer->body.player.vel.z = 0.0f;
                                        hitPlayer->body.player.sprinting = false;
                                    }
                                    CombatEventNet event;
                                    event.serial = nextCombatSerial++;
                                    if (!nextCombatSerial) nextCombatSerial = 1;
                                    event.kind = result.killed ? CombatEventKind::Death : CombatEventKind::Hit;
                                    event.attacker = projectile.owner; event.target = hitPlayer->id; event.limb = -1;
                                    event.amount = (uint16_t)std::min(65535,
                                        (int)std::lround(result.amount * 10000));
                                    combatEvents.push_back({serverTick, event});
                                }
                            }
                        } else {
                            std::vector<structure::GuardianSpan> guardians;
                            structure::collectGuardians(world, guardians);
                            for (const structure::GuardianSpan& guardian : guardians) {
                                float cx = std::max(guardian.minX, std::min(impact.x, guardian.maxX));
                                float cy = std::max(guardian.minY, std::min(impact.y, guardian.maxY));
                                float cz = std::max(guardian.minZ, std::min(impact.z, guardian.maxZ));
                                float blastDistance = (Vec3{cx, cy, cz} - impact).length();
                                hurtGuardian(guardian, structure::guardianArcaneHurt(
                                    ITEM_ARCANE_FIREBALL, guardian.relic, blastDistance));
                            }
                            for (SClient& target : clients) {
                                combat::Actor victim{target.id, target.team,
                                    target.landed && target.fade <= 0 && !target.spectator && !target.body.player.dead,
                                    combat::EntityCategory::Player};
                                if (!combat::hostile(attacker, victim)) continue;
                                Vec3 center = target.body.player.pos + Vec3{0, 0.9f, 0};
                                float amount = arcane::explosionDamage((center - impact).length());
                                if (amount <= 0.0f) continue;
                                combat::DamageSource source{projectile.owner, projectile.action,
                                    ITEM_ARCANE_FIREBALL, combat::DamageCategory::Fire};
                                auto result = combat::damagePlayer(target.body.player.vitals, source,
                                                                   {amount, 1.0f, true}, -1);
                                if (!result.applied) continue;
                                target.hitFlashUntil = serverTick + 4;
                                if (result.killed) {
                                    target.body.player.dead = true;
                                    target.body.player.vel = {};
                                    target.burning.clear();
                                    target.frozen = {};
                                } else {
                                    auto status = std::find_if(target.burning.begin(), target.burning.end(),
                                        [&](const arcane::Burning& value) { return value.source == projectile.owner; });
                                    if (status == target.burning.end()) {
                                        target.burning.erase(std::remove_if(target.burning.begin(), target.burning.end(),
                                            [&](const arcane::Burning& value) { return !arcane::burning(value, serverTick); }),
                                            target.burning.end());
                                        if (target.burning.size() < 64) {
                                            target.burning.push_back({});
                                            status = target.burning.end() - 1;
                                        }
                                    }
                                    if (status != target.burning.end())
                                        arcane::ignite(*status, projectile.owner, projectile.action, serverTick);
                                }
                                CombatEventNet event;
                                event.serial = nextCombatSerial++;
                                if (!nextCombatSerial) nextCombatSerial = 1;
                                event.kind = result.killed ? CombatEventKind::Death : CombatEventKind::Hit;
                                event.attacker = projectile.owner; event.target = target.id; event.limb = -1;
                                event.amount = (uint16_t)std::min(65535, (int)std::lround(result.amount * 10000));
                                combatEvents.push_back({serverTick, event});
                            }
                        }
                        projectiles.erase(projectiles.begin() + (std::ptrdiff_t)i);
                        continue;
                    }
                    projectile.pos = old + step;
                    ++i;
                }

                for (SClient& target : clients) {
                    if (target.body.player.dead || target.spectator || !target.landed) {
                        target.burning.clear();
                        target.frozen = {};
                        target.healFlashUntil = 0;
                        continue;
                    }
                    if (!arcane::frozen(target.frozen, serverTick)) target.frozen = {};
                    for (arcane::Burning& status : target.burning) {
                        if (!arcane::takeBurnDamage(status, serverTick)) continue;
                        combat::DamageSource source{status.source, status.action,
                            ITEM_ARCANE_FIREBALL, combat::DamageCategory::Fire};
                        auto result = combat::damagePlayer(target.body.player.vitals, source,
                                                           {arcane::kBurnDamage, 1.0f, true}, -1);
                        if (!result.applied) continue;
                        target.hitFlashUntil = serverTick + 4;
                        if (result.killed) {
                            target.body.player.dead = true;
                            target.body.player.vel = {};
                            target.frozen = {};
                        }
                        CombatEventNet event;
                        event.serial = nextCombatSerial++;
                        if (!nextCombatSerial) nextCombatSerial = 1;
                        event.kind = result.killed ? CombatEventKind::Death : CombatEventKind::Hit;
                        event.attacker = status.source; event.target = target.id; event.limb = -1;
                        event.amount = (uint16_t)std::min(65535, (int)std::lround(result.amount * 10000));
                        combatEvents.push_back({serverTick, event});
                        if (result.killed) break;
                    }
                    if (target.body.player.dead) {
                        target.burning.clear();
                        target.frozen = {};
                        target.healFlashUntil = 0;
                    }
                    else target.burning.erase(std::remove_if(target.burning.begin(), target.burning.end(),
                        [&](const arcane::Burning& value) { return !arcane::burning(value, serverTick); }),
                        target.burning.end());
                }
                {
                    guardian_fight::Home homes[guardian_fight::kSlots];
                    int homeCount = 0;
                    std::vector<structure::GuardianSync> live;
                    structure::collectRoomGuardians(live);
                    const float block = cfg::BLOCK_SCALE;
                    for (const structure::GuardianSync& g : live) {
                        if (homeCount >= guardian_fight::kSlots) break;
                        guardian_fight::Home& home = homes[homeCount++];
                        home.relic = g.relic;
                        home.hp = (float)g.hp;
                        home.maxHp = (float)(g.maxHp > 0 ? g.maxHp : 1);
                        home.feet = { (g.x + 0.5f) * block, g.y * block, (g.z + 0.5f) * block };
                    }
                    guardian_fight::Rival rivals[32];
                    int rivalCount = 0;
                    for (SClient& c : clients) {
                        if (!c.sentWelcome || c.spectator || rivalCount >= 32) continue;
                        guardian_fight::Rival& rival = rivals[rivalCount++];
                        rival.id = c.id;
                        rival.feet = c.body.player.pos;
                        rival.alive = !c.body.player.dead;
                        rival.active = c.landed && c.fade <= 0.0f && !c.body.player.dead;
                        if (c.melee.pending && !arcane::reached(serverTick, c.melee.hitAt))
                            rival.windup = (float)(c.melee.hitAt - serverTick) / (float)cfg::TICKS_PER_SECOND;
                    }
                    guardian_fight::Blow blows[32];
                    int blowCount = guardian_fight::tick(homes, homeCount, rivals, rivalCount,
                        1.0f / (float)cfg::TICKS_PER_SECOND, guardianGround(world), blows, 32);
                    for (int i = 0; i < blowCount; ++i) {
                        const guardian_fight::Blow& blow = blows[i];
                        SClient* target = nullptr;
                        for (SClient& c : clients)
                            if (c.id == blow.target) { target = &c; break; }
                        if (!target || target->spectator || target->body.player.dead || !target->landed) continue;
                        combat::DamageSource source{guardian_fight::attackerId(blow.relic), 1, AIR,
                                                    combat::DamageCategory::Physical};
                        combat::DamageSpec spec{blow.amount, 1.0f, blow.wholeBody};
                        auto result = combat::damagePlayer(target->body.player.vitals, source, spec, blow.limb);
                        if (!result.applied) continue;
                        target->hitFlashUntil = serverTick + 4;
                        if (result.killed) {
                            target->body.player.dead = true;
                            target->body.player.vel = {};
                        }
                        CombatEventNet event;
                        event.serial = nextCombatSerial++;
                        if (!nextCombatSerial) nextCombatSerial = 1;
                        event.kind = result.killed ? CombatEventKind::Death : CombatEventKind::Hit;
                        event.attacker = source.attacker;
                        event.target = target->id;
                        event.limb = (int8_t)result.limb;
                        event.amount = (uint16_t)std::min(65535, (int)std::lround(result.amount * 10000));
                        combatEvents.push_back({serverTick, event});
                    }
                    guardian_fight::Pose posed[guardian_fight::kSlots];
                    int poseCount = guardian_fight::poses(posed, guardian_fight::kSlots);
                    bool posedRelic[guardian_fight::kSlots]{};
                    for (int i = 0; i < poseCount; ++i) {
                        const guardian_fight::Pose& pose = posed[i];
                        if (pose.relic < 0 || pose.relic >= guardian_fight::kSlots) continue;
                        posedRelic[pose.relic] = true;
                        structure::setGuardianPose(pose.relic, pose.feet.x, pose.feet.y, pose.feet.z,
                                                   pose.yaw, pose.swing);
                    }
                    for (int relic = 0; relic < guardian_fight::kSlots; ++relic)
                        if (!posedRelic[relic]) structure::clearGuardianPose(relic);
                }
                while (!combatEvents.empty() && uint32_t(serverTick - combatEvents.front().first) > 200)
                    combatEvents.pop_front();
                while (!arcaneEvents.empty() && uint32_t(serverTick - arcaneEvents.front().first) > 200)
                    arcaneEvents.pop_front();
                for (SClient& c : clients) {
                    if (!c.sentWelcome) continue;
                    PlayDeltaNet d;
                    d.serverTick = serverTick;
                    d.body = room_body::snapshot(c.body, c.landed);
                    fillChunkSync(world, srvChunks, c, d, serverTick);
                    std::vector<structure::GuardianSync> guards;
                    structure::collectRoomGuardians(guards);
                    for (const structure::GuardianSync& g : guards) {
                        if (d.guardians.size() >= 16) break;
                        int cx = floorDiv(g.x, cfg::CHUNK_X);
                        int cy = floorDiv(g.y, cfg::CHUNK_Y);
                        int cz = floorDiv(g.z, cfg::CHUNK_Z);
                        auto seen = c.seen.find(chunkKey(cx, cy, cz));
                        if (seen == c.seen.end() || !seen->second.base) continue;
                        float px = (g.x + 0.5f) * cfg::BLOCK_SCALE;
                        float py = g.y * cfg::BLOCK_SCALE;
                        float pz = (g.z + 0.5f) * cfg::BLOCK_SCALE;
                        float yaw = 0.0f;
                        uint8_t swing = 0;
                        structure::guardianPose(g.relic, px, py, pz, yaw, swing);
                        d.guardians.push_back(GuardianNet{ (uint8_t)g.relic, (uint16_t)g.hp, (uint16_t)g.maxHp,
                                                          px, py, pz, yaw, swing });
                    }
                    fillTrees(world, c, d, serverTick);
                    fillMines(world, c, d);
                    std::vector<uint32_t> visible;
                    d.inventoryRevision = c.inventory.revision;
                    d.inventoryLayoutAck = c.inventory.lastLayoutSequence;
                    d.inventory = c.inventory.slots;
                    clue::Target clueTarget = clueDirector.targetFor(c.team);
                    if (clueTarget.active) {
                        d.clue.active = true;
                        d.clue.stage = clueTarget.stage;
                        d.clue.destination = (uint8_t)clueTarget.destination;
                        d.clue.rewardItem = clueTarget.rewardItem;
                        d.clue.bossRewardClaimed = clueTarget.bossRewardClaimed;
                        d.clue.x = clueTarget.position.x;
                        d.clue.y = clueTarget.position.y;
                        d.clue.z = clueTarget.position.z;
                    }
                    for (const auto& drop : world.drops()) {
                        if (!drop.netId || d.drops.size() >= 128) continue;
                        DropNet net;
                        net.id = drop.netId;
                        net.x = drop.pos.x; net.y = drop.pos.y; net.z = drop.pos.z;
                        net.vx = drop.vel.x; net.vy = drop.vel.y; net.vz = drop.vel.z;
                        net.axx = drop.ax.x; net.axy = drop.ax.y; net.axz = drop.ax.z;
                        net.ayx = drop.ay.x; net.ayy = drop.ay.y; net.ayz = drop.ay.z;
                        net.azx = drop.az.x; net.azy = drop.az.y; net.azz = drop.az.z;
                        net.avx = drop.angVel.x; net.avy = drop.angVel.y; net.avz = drop.angVel.z;
                        net.age = drop.age;
                        net.item = drop.item; net.count = drop.count; net.grounded = drop.grounded;
                        d.drops.push_back(net);
                    }
                    for (const auto& queued : combatEvents) {
                        if (queued.second.serial <= c.combatAck || d.combat.size() >= 32) continue;
                        d.combat.push_back(queued.second);
                    }
                    for (const auto& projectile : projectiles) {
                        if (d.projectiles.size() >= 64) break;
                        ArcaneProjectileNet net;
                        net.id = projectile.id; net.owner = projectile.owner;
                        net.kind = projectile.kind == arcane::ProjectileKind::Freeze
                            ? ArcaneProjectileKind::Freeze : ArcaneProjectileKind::Fireball;
                        net.x = projectile.pos.x; net.y = projectile.pos.y; net.z = projectile.pos.z;
                        net.vx = projectile.vel.x; net.vy = projectile.vel.y; net.vz = projectile.vel.z;
                        d.projectiles.push_back(net);
                    }
                    for (const auto& queued : arcaneEvents) {
                        if (queued.second.serial <= c.arcaneAck || d.arcane.size() >= 32) continue;
                        d.arcane.push_back(queued.second);
                    }
                    d.players.push_back(poseOf(c, serverTick));
                    for (const SClient& o : clients) {
                        if (!o.sentWelcome || o.id == c.id) continue;
                        if (!chunksVisible(c, o)) continue;
                        if (visible.size() >= 63) break;
                        visible.push_back(o.id);
                        d.players.push_back(poseOf(o, serverTick));
                    }
                    for (uint32_t old : c.announced) {
                        bool still = false;
                        for (uint32_t id : visible)
                            if (id == old) still = true;
                        if (!still) d.removed.push_back(old);
                    }
                    c.announced.swap(visible);
                    c.conn.send((uint16_t)RoomMsg::PlayDelta, encodePlayDelta(d));
                    c.conn.pump();
                }
                broadcastDeploy();
            }
        }

        int connected = 0;
        for (const SClient& c : clients)
            if (c.known) connected++;
        if (connected == 0) {
            if (hadClient && nowMs() - emptySince > 8000) break;
            if (!hadClient && nowMs() - started > 60000) break;
        } else {
            emptySince = nowMs();
        }
        (void)live;
        Sleep(16);
    }

    for (SClient& c : clients) c.conn.close();
    closesocket(listenSock);
    slog(log, "room server stop");
    if (log) fclose(log);
    WSACleanup();
    return 0;
}
