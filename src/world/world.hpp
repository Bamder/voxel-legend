#pragma once
#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>
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

inline int64_t chunkKey(int cx, int cz) {
    return ((int64_t)(uint32_t)cx << 32) | (uint32_t)cz;
}
inline int chunkCX(int64_t key) { return (int)(key >> 32); }
inline int chunkCZ(int64_t key) { return (int)(key & 0xFFFFFFFFu); }

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
    void spawnDrop(const Vec3& pos, uint8_t item, int count, bool inPlace = false, Vec3 vel = { 0, 0, 0 });
    void updateDrops(float dt);
    const std::vector<loot::Drop>& drops() const { return m_drops; }
    int raycastDrop(const Vec3& origin, const Vec3& dir, float maxDist, float& tHit) const;
    bool takeDrop(int index, uint8_t& item, uint8_t& count); // removes; returns false if gone
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
    };
    const std::unordered_map<uint64_t, MineState>& mineStates() const { return m_blockDur; }
    static void decodeMineKey(uint64_t key, int& phys, int& x, int& y, int& z);

    bool hasSodFace(int x, int y, int z, int face) const;
    float sodDurProgress(int x, int y, int z, int face) const; // 0 intact .. 1 about to strip

    // Generate/load chunks around position, unload far ones, rebuild dirty meshes.
    void update(const Vec3& playerPos, int meshBudget);
    void buildMeshFor(Chunk& chunk, int cx, int cz);

    // Advance grass-sod withering for one game tick (budget = max faces examined).
    // Covered sod darkens one stage per WITHER_STAGE_TICKS and is removed after the
    // last stage.
    void sodTick(int budget, uint64_t tick);

    // Advance dynamic water (temporary sources) for one game tick: flow downward,
    // diffuse horizontally, and evaporate in dry air.
    void waterTick(uint64_t tick);
    uint8_t getWaterLevel(int x, int y, int z) const;
    int humidityAt(int x, int y, int z) const; // air humidity -256..255

    Chunk* getChunk(int cx, int cz);
    bool chunkExists(int cx, int cz) const;
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
    bool saveChunkFile(int cx, int cz, const Chunk& ch) const;
    bool loadChunkFile(int cx, int cz, Chunk& ch) const;

    int loadedChunks() const { return (int)m_chunks.size(); }
    int pendingMeshes() const { return (int)m_meshQueue.size(); }

private:
    int computeHeight(int wx, int wz) const;
    void generateChunk(Chunk& ch, int cx, int cz);
    float vertexAO(int wx, int wy, int wz, int nx, int ny, int nz, int ox, int oy, int oz) const;

    // Sod maintenance.
    void rebuildSod(Chunk& ch, int cx, int cz);          // clear + regrow all exposed dirt faces
    void addBorderSod(int cx, int cz);                   // fill deferred side sod in neighbors
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
    void stampTreesInto(Chunk& dest, int dcx, int dcz);

    uint32_t m_seed;
    std::string m_saveDir = cfg::SAVE_DIR;
    bool m_saveEnabled = true;
    std::unordered_map<int64_t, Chunk> m_chunks;
    std::deque<int64_t> m_meshQueue;
    size_t m_sodCursor = 0;   // round-robin cursor for sodTick
    std::vector<PhysicsIsland> m_phys;
    std::vector<loot::Drop> m_drops;
    uint32_t m_dropRng = 0xA341316Cu;
    std::unordered_map<uint64_t, MineState> m_blockDur;
    struct OriginTree {
        uint32_t bindId = 0; // unique living wood/leaf bind for this tree
        std::vector<IVec3> woods;
        std::vector<IVec3> leaves;
    };
    std::unordered_map<int64_t, std::vector<OriginTree>> m_originTrees;
};
