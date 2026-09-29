#pragma once
#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "../core/config.hpp"
#include "blocks.hpp"
#include "loot.hpp"
#include "../core/math.hpp"

// Interleaved world-mesh vertex (matches shader attribute layout 0..5).
struct Vertex {
    float px, py, pz;
    float u, v;
    float nx, ny, nz;
    float faceShade;
    float ao;
    float alpha;
};

#include "tree_fall.hpp"

// 20 bits cx | 16 bits cy | 24 bits? 20+16+24 = 60. Use 22/12/22 so cy fits 0..7
// and horizontal coords stay signed.
inline int64_t chunkKey(int cx, int cy, int cz) {
    uint64_t x = (uint32_t)cx & 0x3FFFFFu;
    uint64_t y = (uint32_t)cy & 0xFFFu;
    uint64_t z = (uint32_t)cz & 0x3FFFFFu;
    return (int64_t)((x << 42) | (y << 22) | z);
}
inline int signExtend(int v, int bits) {
    int s = 1 << (bits - 1);
    return (v ^ s) - s;
}
inline int chunkCX(int64_t key) { return signExtend((int)(((uint64_t)key >> 42) & 0x3FFFFFu), 22); }
inline int chunkCY(int64_t key) { return signExtend((int)(((uint64_t)key >> 22) & 0xFFFu), 12); }
inline int chunkCZ(int64_t key) { return signExtend((int)((uint64_t)key & 0x3FFFFFu), 22); }

// Horizontal column id for tree origins (not a stored terrain chunk).
inline int64_t columnKey(int cx, int cz) {
    return ((int64_t)(uint32_t)cx << 32) | (uint32_t)cz;
}
inline int columnCX(int64_t key) { return (int)(key >> 32); }
inline int columnCZ(int64_t key) { return (int)(key & 0xFFFFFFFFu); }

inline int floorDiv(int a, int b) {
    int q = a / b;
    int r = a % b;
    if (r != 0 && ((r < 0) != (b < 0))) q--;
    return q;
}

class World {
public:
    struct Chunk {
        std::vector<uint8_t> blocks;   // CHUNK_VOLUME bytes, index (y*Z + z)*X + x
        std::vector<uint8_t> waterLevel; // 1..16 for dynamic water, 0 otherwise
        std::vector<uint8_t> flags;    // FLAG_ALIVE = grown tree wood/leaves
        std::vector<uint32_t> treeId;  // 0 = unbound; lazy, sized to CHUNK_VOLUME when used
        bool generated = false;
        bool dirty = true;             // needs mesh rebuild
        bool modified = false;         // user edited -> needs persistence
        bool hasMesh = false;
        bool uploaded = false;         // GL buffers are current (managed by renderer)
        bool hasWater = false;         // chunk contains dynamic water (skip scan otherwise)
        std::vector<Vertex> meshOpaque;
        std::vector<Vertex> meshTransparent;

        // Grass sod (草皮): a single-face overlay on exposed dirt faces.
        // face uses the same order as mesh faces (0 top, 2/3/4/5 sides).
        struct SodFace {
            uint8_t x, z;      // local coords of the dirt block
            uint8_t y;
            uint8_t face;      // 0, 2, 3, 4, 5
            uint8_t stage;     // 0..SOD_STAGES-1 (wither progress)
            uint64_t coveredTick = UINT64_MAX; // session tick when coverage began (runtime only)
            float rem = -1.0f; // runtime remaining durability; <0 = full
        };
        std::vector<SodFace> sodFaces;
        bool sodValid = false;        // sodFaces is authoritative (loaded or rebuilt)

        // Bark overlays (树皮): sod-like single-face sheets on any solid host.
        struct BarkFace {
            uint8_t x, z, y, face;
        };
        std::vector<BarkFace> barkFaces;

        Chunk() : blocks(cfg::CHUNK_VOLUME, (uint8_t)AIR),
                  waterLevel(cfg::CHUNK_VOLUME, 0),
                  flags(cfg::CHUNK_VOLUME, 0) {}

        static int cellIndex(int lx, int y, int lz) {
            return (y * cfg::CHUNK_Z + lz) * cfg::CHUNK_X + lx;
        }
        inline uint8_t get(int lx, int y, int lz) const {
            return blocks[(size_t)cellIndex(lx, y, lz)];
        }
        inline void set(int lx, int y, int lz, uint8_t b) {
            blocks[(size_t)cellIndex(lx, y, lz)] = b;
        }
        inline uint8_t flagAt(int lx, int y, int lz) const {
            return flags[(size_t)cellIndex(lx, y, lz)];
        }
        inline void setFlag(int lx, int y, int lz, uint8_t f) {
            flags[(size_t)cellIndex(lx, y, lz)] = f;
        }
        inline void markAlive(int lx, int y, int lz) {
            flags[(size_t)cellIndex(lx, y, lz)] |= FLAG_ALIVE;
        }
        inline uint32_t treeIdAt(int lx, int y, int lz) const {
            if (treeId.size() != (size_t)cfg::CHUNK_VOLUME) return 0;
            return treeId[(size_t)cellIndex(lx, y, lz)];
        }
        inline void setTreeId(int lx, int y, int lz, uint32_t id) {
            if (id == 0) {
                if (treeId.size() != (size_t)cfg::CHUNK_VOLUME) return;
                treeId[(size_t)cellIndex(lx, y, lz)] = 0;
                return;
            }
            if (treeId.size() != (size_t)cfg::CHUNK_VOLUME)
                treeId.assign((size_t)cfg::CHUNK_VOLUME, 0);
            treeId[(size_t)cellIndex(lx, y, lz)] = id;
        }
        inline void bindAlive(int lx, int y, int lz, uint32_t id) {
            markAlive(lx, y, lz);
            setTreeId(lx, y, lz, id);
        }
        inline uint8_t levelAt(int lx, int y, int lz) const {
            return waterLevel[(y * cfg::CHUNK_Z + lz) * cfg::CHUNK_X + lx];
        }
        inline void setLevel(int lx, int y, int lz, uint8_t l) {
            waterLevel[(y * cfg::CHUNK_Z + lz) * cfg::CHUNK_X + lx] = l;
        }
    };

    explicit World(uint32_t seed) : m_seed(seed) {}

    uint32_t seed() const { return m_seed; }
    const std::string& saveDir() const { return m_saveDir; }
    void setSaveDir(const std::string& dir) { m_saveDir = dir; }
    void setSaveEnabled(bool en) { m_saveEnabled = en; }
    void reset(uint32_t seed);

    // Block access (missing chunk => AIR).
    uint8_t getBlock(int x, int y, int z) const;
    uint8_t getFlags(int x, int y, int z) const;
    uint32_t getTreeId(int x, int y, int z) const;
    bool isAlive(int x, int y, int z) const;
    void setBlock(int x, int y, int z, uint8_t b, bool markModified, bool updateMesh = true);

    void treeFallPhysics(float dt);
    void treeFallGameTick();
    const std::vector<PhysicsIsland>& physicsIslands() const { return m_phys; }
    uint8_t getPhysBlock(int island, int x, int y, int z) const;
    bool breakPhysBlock(int island, int x, int y, int z, uint8_t& dropped);
    bool placePhysBlock(int island, int x, int y, int z, uint8_t b);
    void resolvePhysPlayer(Vec3& pos, Vec3& vel, int axis, float delta,
                           float hw, float hgt, bool& onGround) const;

    // inPlace: sit on `pos` with no velocity. Otherwise `vel` is the initial velocity.
    uint32_t spawnDrop(const Vec3& pos, uint8_t item, int count, bool inPlace = false, Vec3 vel = { 0, 0, 0 });
    void updateDrops(float dt);
    const std::vector<loot::Drop>& drops() const { return m_drops; }
    int raycastDrop(const Vec3& origin, const Vec3& dir, float maxDist, float& tHit) const;
    bool takeDrop(int index, uint8_t& item, uint8_t& count); // removes; returns false if gone
    const loot::Drop* dropById(uint32_t id) const;
    bool takeDropCountById(uint32_t id, uint8_t count);
    void replaceNetworkDrops(const std::vector<loot::Drop>& drops);
    void setDropCount(int index, uint8_t count);

    // Mining durability. applyMineHit applies the break formula once; true when the block is gone.
    // If the hit face still has sod, only sod is damaged (complete-break, no drops).
    float blockDurRemaining(int phys, int x, int y, int z) const;
    float blockDurProgress(int phys, int x, int y, int z) const; // 0 intact .. 1 about to break
    bool applyMineHit(int phys, int x, int y, int z, uint8_t heldTool, int face = -1);
    void clearBlockDur(int phys, int x, int y, int z);

    static constexpr int kMaxMineHits = 12;
    struct MineHit {
        float step = 0.0f; // durability decremented by this hit (formula step)
        uint8_t face = 0;
    };
    struct MineState {
        float rem = 0.0f;
        MineHit hits[kMaxMineHits]{};
        int nHits = 0;
        uint32_t serial = 0;
    };
    struct MineView {
        uint32_t tree = 0;
        int x = 0, y = 0, z = 0;
        float rem = 0.0f;
        uint32_t serial = 0;
        MineHit hits[kMaxMineHits]{};
        int nHits = 0;
    };
    const std::unordered_map<uint64_t, MineState>& mineStates() const { return m_blockDur; }
    static void decodeMineKey(uint64_t key, int& phys, int& x, int& y, int& z);
    void collectMineViews(std::vector<MineView>& out) const;
    void applyMineView(uint32_t tree, int x, int y, int z, float rem, const MineHit* hits, int nHits);
    void clearMineView(uint32_t tree, int x, int y, int z);
    static uint8_t quantSodRem(float rem);
    static float dequantSodRem(uint8_t q);

    bool hasSodFace(int x, int y, int z, int face) const;
    float sodDurProgress(int x, int y, int z, int face) const; // 0 intact .. 1 about to strip

    // Generate/load chunks around position, unload far ones, rebuild dirty meshes.
    void update(const Vec3& playerPos, int meshBudget);
    void buildMeshFor(Chunk& chunk, int cx, int cy, int cz);

    // Advance grass-sod withering for one game tick (budget = max faces examined).
    // Covered sod darkens one stage per WITHER_STAGE_TICKS and is removed after the
    // last stage.
    void sodTick(int budget, uint64_t tick);

    // Advance dynamic water (temporary sources) for one game tick: flow downward,
    // diffuse horizontally, and evaporate in dry air.
    void waterTick(uint64_t tick);
    uint8_t getWaterLevel(int x, int y, int z) const;

    // Authoritative chunk bytes for room sync: blocks, water, alive flags, sod.
    // Meshes and per-client render data are not included.
    struct AuthCell {
        int x = 0, y = 0, z = 0;
        uint8_t block = 0, water = 0, flags = 0;
    };
    struct AuthSod {
        uint8_t x = 0, z = 0, y = 0, face = 0, stage = 0;
        uint8_t rem = 255;
    };
    struct AuthBark {
        uint8_t x = 0, z = 0, y = 0, face = 0;
    };
    struct AuthSlice {
        int cx = 0, cy = 0, cz = 0;
        std::vector<AuthCell> cells;
        bool sod = false;
        std::vector<AuthSod> sods;
        bool bark = false;
        std::vector<AuthBark> barks;
    };
    struct NetTreeCell {
        uint8_t x = 0, y = 0, z = 0, block = 0, flags = 0;
    };
    struct NetTree {
        uint32_t id = 0;
        uint32_t rev = 0;
        bool cells = false;
        int ox = 0, oy = 0, oz = 0;
        Vec3 com, vel, omega, ax{ 1, 0, 0 }, ay{ 0, 1, 0 }, az{ 0, 0, 1 }, pivot;
        bool hold = false;
        float still = 0;
        std::vector<NetTreeCell> body;
    };
    struct AuthCellKey {
        int x = 0, y = 0, z = 0;
        bool operator==(const AuthCellKey& o) const { return x == o.x && y == o.y && z == o.z; }
    };
    struct AuthCellKeyHash {
        size_t operator()(const AuthCellKey& k) const noexcept {
            size_t h = (size_t)(uint32_t)k.x;
            h = h * 1973u + (size_t)(uint32_t)k.y;
            return h * 1973u + (size_t)(uint32_t)k.z;
        }
    };

    uint32_t chunkAuthHash(const Chunk& ch) const;
    void setAuthCapture(bool on) { m_authCapture = on; }
    std::vector<AuthSlice> flushAuth();
    void acceptSeedChunk(int cx, int cy, int cz);
    void writeAuthChunk(int cx, int cy, int cz, const uint8_t* blocks, const uint8_t* water,
                        const uint8_t* flags, const std::vector<AuthSod>& sod,
                        const std::vector<AuthBark>& bark);
    bool writeAuthDelta(int cx, int cy, int cz, const std::vector<AuthCell>& cells,
                        bool replaceSod, const std::vector<AuthSod>& sod,
                        bool replaceBark, const std::vector<AuthBark>& bark);
    void setLocalTrees(bool on) { m_localTrees = on; }
    void setTagTrees(bool on) { m_tagTrees = on; }
    void clearFallingTrees() { m_phys.clear(); }
    uint32_t treeAuthHash() const;
    uint32_t treeAuthHashOf(const std::vector<uint32_t>& ids) const;
    void exportNetTree(const PhysicsIsland& t, NetTree& out, bool withCells) const;
    bool applyNetTrees(const std::vector<NetTree>& trees, const std::vector<uint32_t>& gone);
    void setKeepEdited(bool on) { m_keepEdited = on; }
    void setMatchBounds(bool on);
    bool matchBounds() const { return m_matchBounds; }
    void setBuildCanvas(bool on);
    bool buildCanvas() const { return m_buildCanvas; }
    void ensureColumn(int cx, int cz);
    void updateAnchors(const Vec3* pos, int count, int meshBudget);
    int humidityAt(int x, int y, int z) const; // air humidity -256..255

    Chunk* getChunk(int cx, int cy, int cz);
    bool chunkExists(int cx, int cy, int cz) const;
    bool columnLoaded(int cx, int cz) const;
    const std::unordered_map<int64_t, Chunk>& chunks() const { return m_chunks; }

    // First non-air voxel along a ray (Amanatides & Woo DDA).
    bool raycast(const Vec3& origin, const Vec3& dir, float maxDist,
                 IVec3& hitBlock, IVec3& prevBlock, Vec3& hitNormal,
                 int* physIsland = nullptr, float* hitT = nullptr) const;
    int faceFromHitNormal(const Vec3& n) const;

    bool hasBarkFace(int x, int y, int z, int face) const;
    bool addBarkFace(int x, int y, int z, int face);
    bool takeBarkFace(int x, int y, int z, int face);
    int takeAllBarkAt(int x, int y, int z);

    int surfaceHeight(int x, int z);

    void saveAll();
    bool saveChunkFile(int cx, int cy, int cz, const Chunk& ch) const;
    bool loadChunkFile(int cx, int cy, int cz, Chunk& ch) const;

    int loadedChunks() const { return (int)m_chunks.size(); }
    int pendingMeshes() const { return (int)m_meshQueue.size(); }

private:
    int computeHeight(int wx, int wz) const;
    void generateColumn(int cx, int cz);
    Chunk* ensureLoadedSlice(int cx, int cy, int cz);
    float vertexAO(int wx, int wy, int wz, int nx, int ny, int nz, int ox, int oy, int oz) const;

    // Sod maintenance.
    void rebuildSod(Chunk& ch, int cx, int cy, int cz);  // clear + regrow all exposed dirt faces
    void addBorderSod(int cx, int cy, int cz);           // fill deferred side sod in neighbors
    void removeSodAt(Chunk& ch, int lx, int y, int lz);  // remove all sod attached to one cell
    void removeBarkAt(Chunk& ch, int lx, int y, int lz);

    // Water maintenance.
    void setWaterCell(int x, int y, int z, uint8_t level, bool markModified); // level 0 => air
    int airHumidity(int x, int y, int z) const;
    int downhillSteps(int x, int y, int z, int dx, int dz) const; // fewer = more downhill
    void processWaterCell(int x, int y, int z, uint8_t level, uint64_t tick);
    void detachAliveTree(int x, int y, int z, uint32_t bindId);
    void spawnFallingTree(const std::vector<IVec3>& cells, int cutX, int cutY, int cutZ);
    void writeCell(int x, int y, int z, uint8_t b, uint8_t flags, bool markModified);
    void rebuildTouched(const std::vector<IVec3>& cells);
    bool trySettleFalling(PhysicsIsland& t);
    void splitIsland(size_t index);
    void cacheOriginTrees(int ocx, int ocz);
    void touchAuth(int x, int y, int z);
    void touchAuthSod(int64_t key);
    void touchAuthBark(int64_t key);
    void tagIsland(PhysicsIsland& t);
    void clearIslandMines(size_t index);
    void forgetIslandMines(size_t index);
    void rememberEdited(int cx, int cz);
    void remeshChunk(int cx, int cy, int cz);
    void noteBlankColumn(int cx, int cz);

    uint32_t m_seed;
    std::string m_saveDir = cfg::SAVE_DIR;
    bool m_saveEnabled = true;
    std::unordered_map<int64_t, Chunk> m_chunks;
    std::deque<int64_t> m_meshQueue;
    size_t m_sodCursor = 0;   // round-robin cursor for sodTick
    std::vector<PhysicsIsland> m_phys;
    std::vector<loot::Drop> m_drops;
    uint32_t m_nextDropId = 1;
    uint32_t m_dropRng = 0xA341316Cu;
    std::unordered_map<uint64_t, MineState> m_blockDur;
    uint32_t m_mineEpoch = 1;
    struct OriginTree {
        uint32_t bindId = 0; // unique living wood/leaf bind for this tree
        std::vector<IVec3> woods;
        std::vector<IVec3> leaves;
    };
    std::unordered_map<int64_t, std::vector<OriginTree>> m_originTrees;
    bool m_authCapture = false;
    bool m_keepEdited = false;
    bool m_localTrees = true;
    bool m_tagTrees = false;
    bool m_matchBounds = false;
    bool m_buildCanvas = false;
    uint32_t m_nextTree = 1;
    std::unordered_set<int64_t> m_authBark;
    std::unordered_map<AuthCellKey, AuthCell, AuthCellKeyHash> m_authCells;
    std::unordered_set<int64_t> m_authSod;
    std::unordered_set<int64_t> m_editedCols;
};
