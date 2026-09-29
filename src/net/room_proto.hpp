#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <array>
#include <algorithm>
#include <cmath>
#include "../core/config.hpp"
#include "../world/blocks.hpp"
#include "../world/vitals.hpp"

// Lobby + match messages. Little-endian, length-prefixed by the socket layer.
constexpr uint16_t kRoomPortDefault = 35535;
constexpr uint32_t kRoomProto = 2609290101u;

// PlayInput flags. The server steps locomotion from these; it does not take the client's clock.
constexpr uint8_t kPfSprint = 1;
constexpr uint8_t kPfFly = 2;
constexpr uint8_t kPfGround = 4;

// Movement intent, never a client position or simulation timestep.
constexpr uint8_t kMoveForward = 1, kMoveBack = 2, kMoveLeft = 4, kMoveRight = 8;
constexpr uint8_t kMoveJump = 16, kMoveSneak = 32, kMoveSprint = 64;

inline bool validWireSlot(const ItemSlot& slot) {
    return slot.block == AIR ? slot.count == 0
        : slot.block < BLOCK_COUNT && slot.count > 0 && slot.count <= cfg::MAX_STACK;
}

constexpr uint8_t kStrikeNone = 0;
constexpr uint8_t kStrikePunch = 1;
constexpr uint8_t kStrikeAxe = 2;
constexpr uint8_t kStrikePick = 3;

enum class RoomMsg : uint16_t {
    Hello = 1,       // lobby: u32 proto, string name
    Welcome = 2,     // u32 id
    Lobby = 3,       // port, teams, players
    JoinTeam = 4,    // i32 team
    MatchStart = 5,  // empty; guests reconnect to the dedicated server
    PlayHello = 6,   // u32 proto, string name
    PlayWelcome = 7, // id, seed, spawn, spectator, team
    PlayInput = 8,   // pose, avatar inputs, block edits, chunk resync asks
    PlayDelta = 9,   // server tick + player state + chunk baseline / delta / hash
    Deploy = 10,     // client: set or cancel a deploy pin
    DeploySync = 11  // server: same-team pins (aiming X or fading dot)
};

struct DeployPinNet {
    uint32_t id = 0;
    std::string name;
    int bx = 0, bz = 0;
    uint8_t phase = 0; // 1 = aiming (countdown in t), 2 = landed (fade 1..0 in t)
    float t = 0.0f;
};

struct RoomTeamNet {
    std::string name;
    float r = 1.0f, g = 1.0f, b = 1.0f;
    bool spectator = false;
};

struct RoomPlayerNet {
    uint32_t id = 0;
    std::string name;
    int team = -1;
    bool host = false;
};

struct BlockEditNet {
    int x = 0, y = 0, z = 0;
    uint8_t block = 0;
};

// One player's visible state at a server tick.
// clip/frameQ/strike/strikeQ is the animation frame drawn for that tick
// (clip id + quantized frame, plus the strike overlay).
struct PlayerPoseNet {
    uint32_t id = 0;
    std::string name;
    float x = 0, y = 0, z = 0, yaw = 0, pitch = 0, bodyYaw = 0;
    bool spectator = false;
    uint8_t clip = 1;
    uint16_t frameQ = 0;
    uint8_t strike = 0;
    uint16_t strikeQ = 0;
    uint8_t heldL = 0, heldR = 0, carried = 0;
    uint8_t wearU = 0, wearL = 0, wearS = 0;
    float health[vitals::Count] = {1,1,1,1,1,1,1};
    uint8_t status = 0;
    uint8_t hitFlash = 0;
    bool dead = false;
};

struct PlayInputNet {
    uint8_t movement = 0;
    float x = 0, y = 0, z = 0, yaw = 0, pitch = 0, bodyYaw = 0;
    float vx = 0, vz = 0;
    bool spectator = false;
    uint32_t ack = 0;
    uint8_t flags = 0;
    uint8_t heldL = 0, heldR = 0, carried = 0;
    uint8_t wearU = 0, wearL = 0, wearS = 0;
    uint8_t strikeKind = 0;
    float strikeCharge = 0, strikeCool = 0, mineCharge = 0, mineCooldown = 0;
    bool pickRaised = false;
    std::vector<BlockEditNet> edits;
    struct ChunkAsk { int cx = 0, cy = 0, cz = 0; };
    std::vector<ChunkAsk> resync;
    struct BarkEdit { int x = 0, y = 0, z = 0; uint8_t face = 0; bool place = false; };
    std::vector<BarkEdit> bark;
    bool treeResync = false;
    struct MineEdit {
        int x = 0, y = 0, z = 0;
        uint8_t face = 0;
        uint8_t tool = 0;
        uint32_t tree = 0;
    };
    std::vector<MineEdit> mines;
    uint8_t selectedLeft = 0, selectedRight = 0;
    uint32_t attackSequence = 0;
    uint8_t attackHand = 1; // 0 left, 1 right
    uint32_t castSequence = 0;
    uint8_t castHand = 1; // 0 left, 1 right
    uint32_t pickupSequence = 0;
    uint32_t pickupDrop = 0;
    uint32_t layoutSequence = 0;
    uint32_t layoutBaseRevision = 0;
    std::array<ItemSlot, cfg::INVENTORY_SLOTS> layout{};
    uint32_t combatAck = 0;
    uint32_t arcaneAck = 0;
};

struct AuthCellNet {
    int x = 0, y = 0, z = 0;
    uint8_t block = 0, water = 0, flags = 0;
};

struct AuthSodNet {
    uint8_t x = 0, z = 0, y = 0, face = 0, stage = 0;
    uint8_t rem = 255; // 255 = full; else remaining durability / max * 254
};

struct MineHitNet {
    float step = 0;
    uint8_t face = 0;
};

struct MineNet {
    uint32_t tree = 0;
    int x = 0, y = 0, z = 0;
    float rem = 0;
    uint32_t serial = 0;
    std::vector<MineHitNet> hits;
};

struct MineGoneNet {
    uint32_t tree = 0;
    int x = 0, y = 0, z = 0;
};

struct AuthBarkNet {
    uint8_t x = 0, z = 0, y = 0, face = 0;
};

struct TreeCellNet {
    uint8_t x = 0, y = 0, z = 0, block = 0, flags = 0;
};

struct TreeNet {
    uint32_t id = 0;
    uint32_t rev = 0;
    bool cells = false;
    int ox = 0, oy = 0, oz = 0;
    float cx = 0, cy = 0, cz = 0;
    float vx = 0, vy = 0, vz = 0;
    float wx = 0, wy = 0, wz = 0;
    float ax = 1, ay = 0, az = 0;
    float bx = 0, by = 1, bz = 0;
    float dx = 0, dy = 0, dz = 1;
    float px = 0, py = 0, pz = 0;
    bool hold = false;
    float still = 0;
    std::vector<TreeCellNet> body;
};

struct ChunkBaseNet {
    int cx = 0, cy = 0, cz = 0;
    uint32_t rev = 0;
    bool pristine = false;
    std::vector<uint8_t> blocks;
    std::vector<uint8_t> water;
    std::vector<uint8_t> flags;
    std::vector<AuthSodNet> sod;
    std::vector<AuthBarkNet> bark;
};

struct ChunkDeltaNet {
    int cx = 0, cy = 0, cz = 0;
    uint32_t rev = 0;
    std::vector<AuthCellNet> cells;
    bool sod = false;
    std::vector<AuthSodNet> sods;
    bool bark = false;
    std::vector<AuthBarkNet> barks;
};

struct ChunkHashNet {
    int cx = 0, cy = 0, cz = 0;
    uint32_t rev = 0;
    uint32_t hash = 0;
};

class Buf {
public:
    void u8(uint8_t v) { d.push_back(v); }
    void u16(uint16_t v) {
        d.push_back((uint8_t)(v & 255));
        d.push_back((uint8_t)((v >> 8) & 255));
    }
    void u32(uint32_t v) {
        d.push_back((uint8_t)(v & 255));
        d.push_back((uint8_t)((v >> 8) & 255));
        d.push_back((uint8_t)((v >> 16) & 255));
        d.push_back((uint8_t)((v >> 24) & 255));
    }
    void i32(int32_t v) { u32((uint32_t)v); }
    void f32(float v) {
        uint32_t u = 0;
        std::memcpy(&u, &v, 4);
        u32(u);
    }
    void str(const std::string& s) {
        uint16_t n = (uint16_t)(s.size() > 96 ? 96 : s.size());
        u16(n);
        d.insert(d.end(), s.begin(), s.begin() + n);
    }
    void bytes(const uint8_t* p, size_t n) { if (n) d.insert(d.end(), p, p + n); }
    const std::vector<uint8_t>& data() const { return d; }

    static bool u8(const uint8_t*& p, const uint8_t* end, uint8_t& o) {
        if (p >= end) return false;
        o = *p++;
        return true;
    }
    static bool u16(const uint8_t*& p, const uint8_t* end, uint16_t& o) {
        if (end - p < 2) return false;
        o = (uint16_t)p[0] | ((uint16_t)p[1] << 8);
        p += 2;
        return true;
    }
    static bool u32(const uint8_t*& p, const uint8_t* end, uint32_t& o) {
        if (end - p < 4) return false;
        o = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
        p += 4;
        return true;
    }
    static bool i32(const uint8_t*& p, const uint8_t* end, int32_t& o) {
        uint32_t u = 0;
        if (!u32(p, end, u)) return false;
        o = (int32_t)u;
        return true;
    }
    static bool f32(const uint8_t*& p, const uint8_t* end, float& o) {
        uint32_t u = 0;
        if (!u32(p, end, u)) return false;
        std::memcpy(&o, &u, 4);
        return true;
    }
    static bool str(const uint8_t*& p, const uint8_t* end, std::string& o) {
        uint16_t n = 0;
        if (!u16(p, end, n) || n > 96 || end - p < n) return false;
        o.assign((const char*)p, (const char*)p + n);
        p += n;
        return true;
    }
    static bool bytes(const uint8_t*& p, const uint8_t* end, uint8_t* dst, size_t n) {
        if (n > (size_t)(end - p)) return false;
        if (n) std::memcpy(dst, p, n);
        p += n;
        return true;
    }

private:
    std::vector<uint8_t> d;
};

inline std::vector<uint8_t> encodeHello(const std::string& name) {
    Buf b;
    b.u32(kRoomProto);
    b.str(name);
    return b.data();
}

inline bool decodeHello(const uint8_t* p, const uint8_t* end, std::string& name) {
    uint32_t proto = 0;
    if (!Buf::u32(p, end, proto) || proto != kRoomProto) return false;
    return Buf::str(p, end, name);
}

inline std::vector<uint8_t> encodeWelcome(uint32_t id) {
    Buf b;
    b.u32(id);
    return b.data();
}

inline bool decodeWelcome(const uint8_t* p, const uint8_t* end, uint32_t& id) {
    return Buf::u32(p, end, id);
}

inline std::vector<uint8_t> encodeLobby(uint16_t port, uint32_t hostId,
                                        const std::vector<RoomTeamNet>& teams,
                                        const std::vector<RoomPlayerNet>& players) {
    Buf b;
    b.u16(port);
    b.u32(hostId);
    b.u16((uint16_t)teams.size());
    for (const RoomTeamNet& t : teams) {
        b.str(t.name);
        b.f32(t.r);
        b.f32(t.g);
        b.f32(t.b);
        b.u8(t.spectator ? 1 : 0);
    }
    b.u16((uint16_t)players.size());
    for (const RoomPlayerNet& pl : players) {
        b.u32(pl.id);
        b.str(pl.name);
        b.i32(pl.team);
        b.u8(pl.host ? 1 : 0);
    }
    return b.data();
}

inline bool decodeLobby(const uint8_t* p, const uint8_t* end, uint16_t& port, uint32_t& hostId,
                        std::vector<RoomTeamNet>& teams, std::vector<RoomPlayerNet>& players) {
    teams.clear();
    players.clear();
    if (!Buf::u16(p, end, port) || !Buf::u32(p, end, hostId)) return false;
    uint16_t nt = 0;
    if (!Buf::u16(p, end, nt) || nt > 16) return false;
    teams.resize(nt);
    for (uint16_t i = 0; i < nt; i++) {
        uint8_t spec = 0;
        if (!Buf::str(p, end, teams[i].name) || !Buf::f32(p, end, teams[i].r) ||
            !Buf::f32(p, end, teams[i].g) || !Buf::f32(p, end, teams[i].b) ||
            !Buf::u8(p, end, spec))
            return false;
        teams[i].spectator = spec != 0;
    }
    uint16_t np = 0;
    if (!Buf::u16(p, end, np) || np > 32) return false;
    players.resize(np);
    for (uint16_t i = 0; i < np; i++) {
        uint8_t host = 0;
        if (!Buf::u32(p, end, players[i].id) || !Buf::str(p, end, players[i].name) ||
            !Buf::i32(p, end, players[i].team) || !Buf::u8(p, end, host))
            return false;
        players[i].host = host != 0;
    }
    return true;
}

inline std::vector<uint8_t> encodeJoinTeam(int team) {
    Buf b;
    b.i32(team);
    return b.data();
}

inline bool decodeJoinTeam(const uint8_t* p, const uint8_t* end, int& team) {
    int32_t t = 0;
    if (!Buf::i32(p, end, t)) return false;
    team = (int)t;
    return true;
}

inline std::vector<uint8_t> encodeDeploy(uint8_t action, int bx, int bz) {
    Buf b;
    b.u8(action);
    b.i32(bx);
    b.i32(bz);
    return b.data();
}

inline bool decodeDeploy(const uint8_t* p, const uint8_t* end, uint8_t& action, int& bx, int& bz) {
    int32_t x = 0, z = 0;
    if (!Buf::u8(p, end, action) || !Buf::i32(p, end, x) || !Buf::i32(p, end, z)) return false;
    bx = (int)x;
    bz = (int)z;
    return true;
}

inline std::vector<uint8_t> encodeDeploySync(const std::vector<DeployPinNet>& pins) {
    Buf b;
    uint8_t n = (uint8_t)(pins.size() > 16 ? 16 : pins.size());
    b.u8(n);
    for (uint8_t i = 0; i < n; i++) {
        b.u32(pins[i].id);
        b.str(pins[i].name);
        b.i32(pins[i].bx);
        b.i32(pins[i].bz);
        b.u8(pins[i].phase);
        b.f32(pins[i].t);
    }
    return b.data();
}

inline bool decodeDeploySync(const uint8_t* p, const uint8_t* end, std::vector<DeployPinNet>& pins) {
    pins.clear();
    uint8_t n = 0;
    if (!Buf::u8(p, end, n)) return false;
    pins.reserve(n);
    for (uint8_t i = 0; i < n; i++) {
        DeployPinNet pin;
        int32_t x = 0, z = 0;
        if (!Buf::u32(p, end, pin.id) || !Buf::str(p, end, pin.name) || !Buf::i32(p, end, x) ||
            !Buf::i32(p, end, z) || !Buf::u8(p, end, pin.phase) || !Buf::f32(p, end, pin.t))
            return false;
        pin.bx = (int)x;
        pin.bz = (int)z;
        pins.push_back(std::move(pin));
    }
    return true;
}

inline std::vector<uint8_t> encodePlayWelcome(uint32_t id, uint32_t seed, float x, float y, float z,
                                              bool spectator, int team) {
    Buf b;
    b.u32(id);
    b.u32(seed);
    b.f32(x);
    b.f32(y);
    b.f32(z);
    b.u8(spectator ? 1 : 0);
    b.i32(team);
    return b.data();
}

inline bool decodePlayWelcome(const uint8_t* p, const uint8_t* end, uint32_t& id, uint32_t& seed,
                              float& x, float& y, float& z, bool& spectator, int& team) {
    uint8_t spec = 0;
    int32_t tm = 0;
    if (!Buf::u32(p, end, id) || !Buf::u32(p, end, seed) || !Buf::f32(p, end, x) ||
        !Buf::f32(p, end, y) || !Buf::f32(p, end, z) || !Buf::u8(p, end, spec) ||
        !Buf::i32(p, end, tm))
        return false;
    spectator = spec != 0;
    team = (int)tm;
    return true;
}

inline std::vector<uint8_t> encodePlayInput(const PlayInputNet& in) {
    Buf b;
    b.u8(in.movement);
    b.f32(in.x);
    b.f32(in.y);
    b.f32(in.z);
    b.f32(in.yaw);
    b.f32(in.pitch);
    b.u8(in.spectator ? 1 : 0);
    b.u32(in.ack);
    uint16_t n = (uint16_t)(in.edits.size() > 32 ? 32 : in.edits.size());
    b.u16(n);
    for (uint16_t i = 0; i < n; i++) {
        b.i32(in.edits[i].x);
        b.i32(in.edits[i].y);
        b.i32(in.edits[i].z);
        b.u8(in.edits[i].block);
    }
    b.f32(in.bodyYaw);
    b.f32(in.vx);
    b.f32(in.vz);
    b.u8(in.flags);
    b.u8(in.heldL);
    b.u8(in.heldR);
    b.u8(in.carried);
    b.u8(in.wearU);
    b.u8(in.wearL);
    b.u8(in.wearS);
    b.u8(in.strikeKind);
    b.f32(in.strikeCharge);
    b.f32(in.strikeCool);
    b.f32(in.mineCharge);
    b.f32(in.mineCooldown);
    b.u8(in.pickRaised ? 1 : 0);
    uint8_t nr = (uint8_t)(in.resync.size() > 8 ? 8 : in.resync.size());
    b.u8(nr);
    for (uint8_t i = 0; i < nr; i++) {
        b.i32(in.resync[i].cx);
        b.i32(in.resync[i].cy);
        b.i32(in.resync[i].cz);
    }
    uint8_t nbk = (uint8_t)(in.bark.size() > 8 ? 8 : in.bark.size());
    b.u8(nbk);
    for (uint8_t i = 0; i < nbk; i++) {
        b.i32(in.bark[i].x);
        b.i32(in.bark[i].y);
        b.i32(in.bark[i].z);
        b.u8(in.bark[i].face);
        b.u8(in.bark[i].place ? 1 : 0);
    }
    b.u8(in.treeResync ? 1 : 0);
    uint8_t nm = (uint8_t)(in.mines.size() > 8 ? 8 : in.mines.size());
    b.u8(nm);
    for (uint8_t i = 0; i < nm; i++) {
        b.i32(in.mines[i].x);
        b.i32(in.mines[i].y);
        b.i32(in.mines[i].z);
        b.u8(in.mines[i].face);
        b.u8(in.mines[i].tool);
        b.u32(in.mines[i].tree);
    }
    b.u8(in.selectedLeft); b.u8(in.selectedRight);
    b.u32(in.attackSequence); b.u8(in.attackHand);
    b.u32(in.castSequence); b.u8(in.castHand);
    b.u32(in.pickupSequence); b.u32(in.pickupDrop);
    b.u32(in.layoutSequence); b.u32(in.layoutBaseRevision);
    for (const auto& slot : in.layout) { b.u8(slot.block); b.u8(slot.count); }
    b.u32(in.combatAck);
    b.u32(in.arcaneAck);
    return b.data();
}

inline bool decodePlayInput(const uint8_t* p, const uint8_t* end, PlayInputNet& in) {
    in = PlayInputNet{};
    if (!Buf::u8(p, end, in.movement) || (in.movement & 128)) return false;
    uint8_t spec = 0;
    uint16_t n = 0;
    uint8_t raised = 0;
    if (!Buf::f32(p, end, in.x) || !Buf::f32(p, end, in.y) || !Buf::f32(p, end, in.z) ||
        !Buf::f32(p, end, in.yaw) || !Buf::f32(p, end, in.pitch) || !Buf::u8(p, end, spec) ||
        !Buf::u32(p, end, in.ack) || !Buf::u16(p, end, n) || n > 32)
        return false;
    in.spectator = spec != 0;
    in.edits.resize(n);
    for (uint16_t i = 0; i < n; i++) {
        if (!Buf::i32(p, end, in.edits[i].x) || !Buf::i32(p, end, in.edits[i].y) ||
            !Buf::i32(p, end, in.edits[i].z) || !Buf::u8(p, end, in.edits[i].block))
            return false;
    }
    if (!Buf::f32(p, end, in.bodyYaw) || !Buf::f32(p, end, in.vx) || !Buf::f32(p, end, in.vz) ||
        !Buf::u8(p, end, in.flags) || !Buf::u8(p, end, in.heldL) || !Buf::u8(p, end, in.heldR) ||
        !Buf::u8(p, end, in.carried) || !Buf::u8(p, end, in.wearU) || !Buf::u8(p, end, in.wearL) ||
        !Buf::u8(p, end, in.wearS) || !Buf::u8(p, end, in.strikeKind) ||
        !Buf::f32(p, end, in.strikeCharge) || !Buf::f32(p, end, in.strikeCool) ||
        !Buf::f32(p, end, in.mineCharge) || !Buf::f32(p, end, in.mineCooldown) ||
        !Buf::u8(p, end, raised))
        return false;
    in.pickRaised = raised != 0;
    uint8_t nr = 0;
    if (!Buf::u8(p, end, nr) || nr > 8) return false;
    in.resync.resize(nr);
    for (uint8_t i = 0; i < nr; i++) {
        if (!Buf::i32(p, end, in.resync[i].cx) || !Buf::i32(p, end, in.resync[i].cy) ||
            !Buf::i32(p, end, in.resync[i].cz))
            return false;
    }
    uint8_t nbk = 0;
    if (!Buf::u8(p, end, nbk) || nbk > 8) return false;
    in.bark.resize(nbk);
    for (uint8_t i = 0; i < nbk; i++) {
        uint8_t place = 0;
        if (!Buf::i32(p, end, in.bark[i].x) || !Buf::i32(p, end, in.bark[i].y) ||
            !Buf::i32(p, end, in.bark[i].z) || !Buf::u8(p, end, in.bark[i].face) ||
            !Buf::u8(p, end, place))
            return false;
        in.bark[i].place = place != 0;
    }
    uint8_t tr = 0;
    if (!Buf::u8(p, end, tr)) return false;
    in.treeResync = tr != 0;
    uint8_t nm = 0;
    if (!Buf::u8(p, end, nm) || nm > 8) return false;
    in.mines.resize(nm);
    for (uint8_t i = 0; i < nm; i++) {
        if (!Buf::i32(p, end, in.mines[i].x) || !Buf::i32(p, end, in.mines[i].y) ||
            !Buf::i32(p, end, in.mines[i].z) || !Buf::u8(p, end, in.mines[i].face) ||
            !Buf::u8(p, end, in.mines[i].tool) || !Buf::u32(p, end, in.mines[i].tree))
            return false;
    }
    if (!Buf::u8(p, end, in.selectedLeft) || in.selectedLeft >= cfg::HAND_SLOTS ||
        !Buf::u8(p, end, in.selectedRight) || in.selectedRight >= cfg::HAND_SLOTS ||
        !Buf::u32(p, end, in.attackSequence) || !Buf::u8(p, end, in.attackHand) || in.attackHand > 1 ||
        !Buf::u32(p, end, in.castSequence) || !Buf::u8(p, end, in.castHand) || in.castHand > 1 ||
        !Buf::u32(p, end, in.pickupSequence) || !Buf::u32(p, end, in.pickupDrop) ||
        !Buf::u32(p, end, in.layoutSequence) || !Buf::u32(p, end, in.layoutBaseRevision)) return false;
    for (auto& slot : in.layout)
        if (!Buf::u8(p, end, slot.block) || !Buf::u8(p, end, slot.count) || !validWireSlot(slot)) return false;
    if (!Buf::u32(p, end, in.combatAck) || !Buf::u32(p, end, in.arcaneAck)) return false;
    return true;
}

struct BodyStateNet {
    float x = 0, y = 0, z = 0, vx = 0, vy = 0, vz = 0;
    vitals::Vitals vitals;
    vitals::Fatigue fatigue;
    uint8_t flags = 0; // bit0: landed; bit1: grounded; bit2: in water
};

struct DropNet {
    uint32_t id = 0;
    float x = 0, y = 0, z = 0;
    float vx = 0, vy = 0, vz = 0;
    float axx = 1, axy = 0, axz = 0;
    float ayx = 0, ayy = 1, ayz = 0;
    float azx = 0, azy = 0, azz = 1;
    float avx = 0, avy = 0, avz = 0;
    float age = 0;
    uint8_t item = AIR, count = 0;
    bool grounded = false;
};

enum class CombatEventKind : uint8_t { Hit = 1, Death = 2 };
struct CombatEventNet {
    uint32_t serial = 0, attacker = 0, target = 0;
    CombatEventKind kind = CombatEventKind::Hit;
    int8_t limb = -1;
    uint16_t amount = 0; // normalized health removed, * 10000
};

inline constexpr uint8_t kStatusBurning = 1u;

struct ArcaneProjectileNet {
    uint32_t id = 0, owner = 0;
    float x = 0, y = 0, z = 0;
    float vx = 0, vy = 0, vz = 0;
};

enum class ArcaneEventKind : uint8_t { FireballExplode = 1 };
struct ArcaneEventNet {
    uint32_t serial = 0;
    ArcaneEventKind kind = ArcaneEventKind::FireballExplode;
    float x = 0, y = 0, z = 0;
};

struct PlayDeltaNet {
    BodyStateNet body; // recipient's authoritative body, fixed-size wire layout
    uint32_t serverTick = 0;
    std::vector<uint32_t> removed;
    std::vector<PlayerPoseNet> players;
    std::vector<ChunkBaseNet> bases;
    std::vector<ChunkDeltaNet> deltas;
    std::vector<ChunkHashNet> checks;
    std::vector<TreeNet> trees;
    std::vector<uint32_t> treesGone;
    bool treeCheck = false;
    uint32_t treeHash = 0;
    std::vector<MineNet> mines;
    std::vector<MineGoneNet> mineGone;
    uint32_t inventoryRevision = 0;
    uint32_t inventoryLayoutAck = 0;
    std::array<ItemSlot, cfg::INVENTORY_SLOTS> inventory{};
    std::vector<DropNet> drops;
    std::vector<CombatEventNet> combat;
    std::vector<ArcaneProjectileNet> projectiles;
    std::vector<ArcaneEventNet> arcane;
};

inline std::vector<uint8_t> encodePlayDelta(const PlayDeltaNet& d) {
    Buf b;
    b.u32(d.serverTick);
    b.f32(d.body.x); b.f32(d.body.y); b.f32(d.body.z);
    b.f32(d.body.vx); b.f32(d.body.vy); b.f32(d.body.vz);
    b.u8(d.body.flags);
    for (const auto& limb : d.body.vitals.limb) {
        b.f32(limb.health); b.f32(limb.stamina);
    }
    b.f32(d.body.vitals.hunger); b.f32(d.body.vitals.thirst);
    b.f32(d.body.vitals.cardio); b.f32(d.body.vitals.inspire);
    for (int i = 0; i < vitals::Count; ++i) {
        b.u8(d.body.fatigue.emptied[i] ? 1 : 0);
        b.f32(d.body.fatigue.recoverDelay[i]);
    }
    b.i32(d.body.fatigue.jumpChain); b.f32(d.body.fatigue.jumpChainTimer);
    b.u8(d.body.fatigue.jumpChainArmed ? 1 : 0);
    b.u16((uint16_t)d.removed.size());
    for (uint32_t id : d.removed) b.u32(id);
    b.u16((uint16_t)d.players.size());
    for (const PlayerPoseNet& pl : d.players) {
        b.u32(pl.id);
        b.str(pl.name);
        b.f32(pl.x);
        b.f32(pl.y);
        b.f32(pl.z);
        b.f32(pl.yaw);
        b.f32(pl.pitch);
        b.f32(pl.bodyYaw);
        b.u8(pl.spectator ? 1 : 0);
        b.u8(pl.clip);
        b.u16(pl.frameQ);
        b.u8(pl.strike);
        b.u16(pl.strikeQ);
        b.u8(pl.heldL);
        b.u8(pl.heldR);
        b.u8(pl.carried);
        b.u8(pl.wearU);
        b.u8(pl.wearL);
        b.u8(pl.wearS);
        for (float health : pl.health) b.f32(health);
        b.u8(pl.status);
        b.u8(pl.hitFlash);
        b.u8(pl.dead ? 1 : 0);
    }
    b.u16((uint16_t)d.bases.size());
    for (const ChunkBaseNet& base : d.bases) {
        b.i32(base.cx);
        b.i32(base.cy);
        b.i32(base.cz);
        b.u32(base.rev);
        b.u8(base.pristine ? 1 : 0);
        if (base.pristine) continue;
        b.bytes(base.blocks.data(), base.blocks.size());
        b.bytes(base.water.data(), base.water.size());
        b.bytes(base.flags.data(), base.flags.size());
        b.u16((uint16_t)base.sod.size());
        for (const AuthSodNet& s : base.sod) {
            b.u8(s.x);
            b.u8(s.z);
            b.u8(s.y);
            b.u8(s.face);
            b.u8(s.stage);
            b.u8(s.rem);
        }
        b.u16((uint16_t)base.bark.size());
        for (const AuthBarkNet& bk : base.bark) {
            b.u8(bk.x);
            b.u8(bk.z);
            b.u8(bk.y);
            b.u8(bk.face);
        }
    }
    b.u16((uint16_t)d.deltas.size());
    for (const ChunkDeltaNet& delta : d.deltas) {
        b.i32(delta.cx);
        b.i32(delta.cy);
        b.i32(delta.cz);
        b.u32(delta.rev);
        b.u16((uint16_t)delta.cells.size());
        for (const AuthCellNet& c : delta.cells) {
            b.i32(c.x);
            b.i32(c.y);
            b.i32(c.z);
            b.u8(c.block);
            b.u8(c.water);
            b.u8(c.flags);
        }
        b.u8(delta.sod ? 1 : 0);
        if (delta.sod) {
            b.u16((uint16_t)delta.sods.size());
            for (const AuthSodNet& s : delta.sods) {
                b.u8(s.x);
                b.u8(s.z);
                b.u8(s.y);
                b.u8(s.face);
                b.u8(s.stage);
                b.u8(s.rem);
            }
        }
        b.u8(delta.bark ? 1 : 0);
        if (delta.bark) {
            b.u16((uint16_t)delta.barks.size());
            for (const AuthBarkNet& bk : delta.barks) {
                b.u8(bk.x);
                b.u8(bk.z);
                b.u8(bk.y);
                b.u8(bk.face);
            }
        }
    }
    b.u16((uint16_t)d.checks.size());
    for (const ChunkHashNet& chk : d.checks) {
        b.i32(chk.cx);
        b.i32(chk.cy);
        b.i32(chk.cz);
        b.u32(chk.rev);
        b.u32(chk.hash);
    }
    b.u16((uint16_t)d.trees.size());
    for (const TreeNet& t : d.trees) {
        b.u32(t.id);
        b.u32(t.rev);
        b.u8(t.cells ? 1 : 0);
        b.f32(t.cx); b.f32(t.cy); b.f32(t.cz);
        b.f32(t.vx); b.f32(t.vy); b.f32(t.vz);
        b.f32(t.wx); b.f32(t.wy); b.f32(t.wz);
        b.f32(t.ax); b.f32(t.ay); b.f32(t.az);
        b.f32(t.bx); b.f32(t.by); b.f32(t.bz);
        b.f32(t.dx); b.f32(t.dy); b.f32(t.dz);
        b.f32(t.px); b.f32(t.py); b.f32(t.pz);
        b.u8(t.hold ? 1 : 0);
        b.f32(t.still);
        if (!t.cells) continue;
        b.i32(t.ox); b.i32(t.oy); b.i32(t.oz);
        b.u16((uint16_t)t.body.size());
        for (const TreeCellNet& c : t.body) {
            b.u8(c.x); b.u8(c.y); b.u8(c.z); b.u8(c.block); b.u8(c.flags);
        }
    }
    b.u16((uint16_t)d.treesGone.size());
    for (uint32_t id : d.treesGone) b.u32(id);
    b.u8(d.treeCheck ? 1 : 0);
    if (d.treeCheck) b.u32(d.treeHash);
    uint8_t nm = (uint8_t)(d.mines.size() > 32 ? 32 : d.mines.size());
    b.u8(nm);
    for (uint8_t i = 0; i < nm; i++) {
        const MineNet& m = d.mines[i];
        b.u32(m.tree);
        b.i32(m.x);
        b.i32(m.y);
        b.i32(m.z);
        b.f32(m.rem);
        uint8_t nh = (uint8_t)(m.hits.size() > 12 ? 12 : m.hits.size());
        b.u8(nh);
        for (uint8_t h = 0; h < nh; h++) {
            b.f32(m.hits[h].step);
            b.u8(m.hits[h].face);
        }
    }
    uint8_t ngone = (uint8_t)(d.mineGone.size() > 32 ? 32 : d.mineGone.size());
    b.u8(ngone);
    for (uint8_t i = 0; i < ngone; i++) {
        b.u32(d.mineGone[i].tree);
        b.i32(d.mineGone[i].x);
        b.i32(d.mineGone[i].y);
        b.i32(d.mineGone[i].z);
    }
    b.u32(d.inventoryRevision); b.u32(d.inventoryLayoutAck);
    for (const auto& slot : d.inventory) { b.u8(slot.block); b.u8(slot.count); }
    uint16_t ndrop = (uint16_t)std::min<size_t>(d.drops.size(), 128);
    b.u16(ndrop);
    for (uint16_t i = 0; i < ndrop; ++i) {
        const auto& drop = d.drops[i];
        b.u32(drop.id); b.f32(drop.x); b.f32(drop.y); b.f32(drop.z);
        b.f32(drop.vx); b.f32(drop.vy); b.f32(drop.vz);
        b.f32(drop.axx); b.f32(drop.axy); b.f32(drop.axz);
        b.f32(drop.ayx); b.f32(drop.ayy); b.f32(drop.ayz);
        b.f32(drop.azx); b.f32(drop.azy); b.f32(drop.azz);
        b.f32(drop.avx); b.f32(drop.avy); b.f32(drop.avz); b.f32(drop.age);
        b.u8(drop.item); b.u8(drop.count); b.u8(drop.grounded ? 1 : 0);
    }
    uint8_t ncombat = (uint8_t)std::min<size_t>(d.combat.size(), 32);
    b.u8(ncombat);
    for (uint8_t i = 0; i < ncombat; ++i) {
        const auto& event = d.combat[i];
        b.u32(event.serial); b.u8((uint8_t)event.kind); b.u32(event.attacker); b.u32(event.target);
        b.u8((uint8_t)event.limb); b.u16(event.amount);
    }
    uint8_t nprojectiles = (uint8_t)std::min<size_t>(d.projectiles.size(), 64);
    b.u8(nprojectiles);
    for (uint8_t i = 0; i < nprojectiles; ++i) {
        const auto& projectile = d.projectiles[i];
        b.u32(projectile.id); b.u32(projectile.owner);
        b.f32(projectile.x); b.f32(projectile.y); b.f32(projectile.z);
        b.f32(projectile.vx); b.f32(projectile.vy); b.f32(projectile.vz);
    }
    uint8_t narcane = (uint8_t)std::min<size_t>(d.arcane.size(), 32);
    b.u8(narcane);
    for (uint8_t i = 0; i < narcane; ++i) {
        const auto& event = d.arcane[i];
        b.u32(event.serial); b.u8((uint8_t)event.kind);
        b.f32(event.x); b.f32(event.y); b.f32(event.z);
    }
    return b.data();
}

inline bool decodePlayDelta(const uint8_t* p, const uint8_t* end, PlayDeltaNet& d) {
    d = PlayDeltaNet{};
    if (!Buf::u32(p, end, d.serverTick)) return false;
    auto finiteFloat = [&](float& value) {
        return Buf::f32(p, end, value) && std::isfinite(value);
    };
    auto unitFloat = [&](float& value) {
        return finiteFloat(value) && value >= 0 && value <= 1;
    };
    if (!finiteFloat(d.body.x) || !finiteFloat(d.body.y) || !finiteFloat(d.body.z) ||
        !finiteFloat(d.body.vx) || !finiteFloat(d.body.vy) || !finiteFloat(d.body.vz) ||
        !Buf::u8(p, end, d.body.flags) || (d.body.flags & ~7)) return false;
    for (auto& limb : d.body.vitals.limb)
        if (!unitFloat(limb.health) || !unitFloat(limb.stamina)) return false;
    if (!unitFloat(d.body.vitals.hunger) || !unitFloat(d.body.vitals.thirst) ||
        !unitFloat(d.body.vitals.cardio) || !unitFloat(d.body.vitals.inspire)) return false;
    for (int i = 0; i < vitals::Count; ++i) {
        uint8_t emptied = 0;
        if (!Buf::u8(p, end, emptied) || emptied > 1 ||
            !finiteFloat(d.body.fatigue.recoverDelay[i]) || d.body.fatigue.recoverDelay[i] < 0) return false;
        d.body.fatigue.emptied[i] = emptied != 0;
    }
    uint8_t armed = 0;
    if (!Buf::i32(p, end, d.body.fatigue.jumpChain) || d.body.fatigue.jumpChain < 0 ||
        !finiteFloat(d.body.fatigue.jumpChainTimer) || d.body.fatigue.jumpChainTimer < 0 ||
        !Buf::u8(p, end, armed) || armed > 1) return false;
    d.body.fatigue.jumpChainArmed = armed != 0;
    uint16_t nr = 0;
    if (!Buf::u16(p, end, nr) || nr > 64) return false;
    d.removed.resize(nr);
    for (uint16_t i = 0; i < nr; i++)
        if (!Buf::u32(p, end, d.removed[i])) return false;
    uint16_t np = 0;
    if (!Buf::u16(p, end, np) || np > 64) return false;
    d.players.resize(np);
    for (uint16_t i = 0; i < np; i++) {
        uint8_t spec = 0, dead = 0;
        PlayerPoseNet& pl = d.players[i];
        if (!Buf::u32(p, end, pl.id) || !Buf::str(p, end, pl.name) || !Buf::f32(p, end, pl.x) ||
            !Buf::f32(p, end, pl.y) || !Buf::f32(p, end, pl.z) || !Buf::f32(p, end, pl.yaw) ||
            !Buf::f32(p, end, pl.pitch) || !Buf::f32(p, end, pl.bodyYaw) || !Buf::u8(p, end, spec) ||
            !Buf::u8(p, end, pl.clip) || !Buf::u16(p, end, pl.frameQ) ||
            !Buf::u8(p, end, pl.strike) || !Buf::u16(p, end, pl.strikeQ) ||
            !Buf::u8(p, end, pl.heldL) || !Buf::u8(p, end, pl.heldR) || !Buf::u8(p, end, pl.carried) ||
            !Buf::u8(p, end, pl.wearU) || !Buf::u8(p, end, pl.wearL) || !Buf::u8(p, end, pl.wearS))
            return false;
        for (float& health : pl.health)
            if (!Buf::f32(p, end, health) || !std::isfinite(health) || health < 0 || health > 1) return false;
        if (!Buf::u8(p, end, pl.status) || (pl.status & ~kStatusBurning) ||
            !Buf::u8(p, end, pl.hitFlash) || pl.hitFlash > 1 ||
            !Buf::u8(p, end, dead) || dead > 1) return false;
        pl.spectator = spec != 0;
        pl.dead = dead != 0;
    }
    uint16_t nb = 0;
    if (!Buf::u16(p, end, nb) || nb > 32) return false;
    d.bases.resize(nb);
    for (uint16_t i = 0; i < nb; i++) {
        ChunkBaseNet& base = d.bases[i];
        uint8_t pristine = 0;
        if (!Buf::i32(p, end, base.cx) || !Buf::i32(p, end, base.cy) || !Buf::i32(p, end, base.cz) ||
            !Buf::u32(p, end, base.rev) || !Buf::u8(p, end, pristine))
            return false;
        base.pristine = pristine != 0;
        if (base.pristine) continue;
        base.blocks.resize((size_t)cfg::CHUNK_VOLUME);
        base.water.resize((size_t)cfg::CHUNK_VOLUME);
        base.flags.resize((size_t)cfg::CHUNK_VOLUME);
        if (!Buf::bytes(p, end, base.blocks.data(), base.blocks.size()) ||
            !Buf::bytes(p, end, base.water.data(), base.water.size()) ||
            !Buf::bytes(p, end, base.flags.data(), base.flags.size()))
            return false;
        uint16_t ns = 0;
        if (!Buf::u16(p, end, ns) || ns > 20000) return false;
        base.sod.resize(ns);
        for (uint16_t s = 0; s < ns; s++) {
            if (!Buf::u8(p, end, base.sod[s].x) || !Buf::u8(p, end, base.sod[s].z) ||
                !Buf::u8(p, end, base.sod[s].y) || !Buf::u8(p, end, base.sod[s].face) ||
                !Buf::u8(p, end, base.sod[s].stage) ||
                !Buf::u8(p, end, base.sod[s].rem))
                return false;
        }
        uint16_t nk = 0;
        if (!Buf::u16(p, end, nk) || nk > 20000) return false;
        base.bark.resize(nk);
        for (uint16_t k = 0; k < nk; k++) {
            if (!Buf::u8(p, end, base.bark[k].x) || !Buf::u8(p, end, base.bark[k].z) ||
                !Buf::u8(p, end, base.bark[k].y) || !Buf::u8(p, end, base.bark[k].face))
                return false;
        }
    }
    uint16_t nd = 0;
    if (!Buf::u16(p, end, nd) || nd > 64) return false;
    d.deltas.resize(nd);
    for (uint16_t i = 0; i < nd; i++) {
        ChunkDeltaNet& delta = d.deltas[i];
        uint16_t nc = 0;
        if (!Buf::i32(p, end, delta.cx) || !Buf::i32(p, end, delta.cy) || !Buf::i32(p, end, delta.cz) ||
            !Buf::u32(p, end, delta.rev) || !Buf::u16(p, end, nc) || nc > 8192)
            return false;
        delta.cells.resize(nc);
        for (uint16_t c = 0; c < nc; c++) {
            if (!Buf::i32(p, end, delta.cells[c].x) || !Buf::i32(p, end, delta.cells[c].y) ||
                !Buf::i32(p, end, delta.cells[c].z) || !Buf::u8(p, end, delta.cells[c].block) ||
                !Buf::u8(p, end, delta.cells[c].water) || !Buf::u8(p, end, delta.cells[c].flags))
                return false;
        }
        uint8_t sod = 0;
        if (!Buf::u8(p, end, sod)) return false;
        delta.sod = sod != 0;
        if (delta.sod) {
            uint16_t ns = 0;
            if (!Buf::u16(p, end, ns) || ns > 20000) return false;
            delta.sods.resize(ns);
            for (uint16_t s = 0; s < ns; s++) {
                if (!Buf::u8(p, end, delta.sods[s].x) || !Buf::u8(p, end, delta.sods[s].z) ||
                    !Buf::u8(p, end, delta.sods[s].y) || !Buf::u8(p, end, delta.sods[s].face) ||
                    !Buf::u8(p, end, delta.sods[s].stage) ||
                    !Buf::u8(p, end, delta.sods[s].rem))
                    return false;
            }
        }
        uint8_t bark = 0;
        if (!Buf::u8(p, end, bark)) return false;
        delta.bark = bark != 0;
        if (delta.bark) {
            uint16_t nk = 0;
            if (!Buf::u16(p, end, nk) || nk > 20000) return false;
            delta.barks.resize(nk);
            for (uint16_t k = 0; k < nk; k++) {
                if (!Buf::u8(p, end, delta.barks[k].x) || !Buf::u8(p, end, delta.barks[k].z) ||
                    !Buf::u8(p, end, delta.barks[k].y) || !Buf::u8(p, end, delta.barks[k].face))
                    return false;
            }
        }
    }
    uint16_t nh = 0;
    if (!Buf::u16(p, end, nh) || nh > 32) return false;
    d.checks.resize(nh);
    for (uint16_t i = 0; i < nh; i++) {
        if (!Buf::i32(p, end, d.checks[i].cx) || !Buf::i32(p, end, d.checks[i].cy) ||
            !Buf::i32(p, end, d.checks[i].cz) || !Buf::u32(p, end, d.checks[i].rev) ||
            !Buf::u32(p, end, d.checks[i].hash))
            return false;
    }
    uint16_t nt = 0;
    if (!Buf::u16(p, end, nt) || nt > 32) return false;
    d.trees.resize(nt);
    for (uint16_t i = 0; i < nt; i++) {
        TreeNet& t = d.trees[i];
        uint8_t full = 0, hold = 0;
        if (!Buf::u32(p, end, t.id) || !Buf::u32(p, end, t.rev) || !Buf::u8(p, end, full) ||
            !Buf::f32(p, end, t.cx) || !Buf::f32(p, end, t.cy) || !Buf::f32(p, end, t.cz) ||
            !Buf::f32(p, end, t.vx) || !Buf::f32(p, end, t.vy) || !Buf::f32(p, end, t.vz) ||
            !Buf::f32(p, end, t.wx) || !Buf::f32(p, end, t.wy) || !Buf::f32(p, end, t.wz) ||
            !Buf::f32(p, end, t.ax) || !Buf::f32(p, end, t.ay) || !Buf::f32(p, end, t.az) ||
            !Buf::f32(p, end, t.bx) || !Buf::f32(p, end, t.by) || !Buf::f32(p, end, t.bz) ||
            !Buf::f32(p, end, t.dx) || !Buf::f32(p, end, t.dy) || !Buf::f32(p, end, t.dz) ||
            !Buf::f32(p, end, t.px) || !Buf::f32(p, end, t.py) || !Buf::f32(p, end, t.pz) ||
            !Buf::u8(p, end, hold) || !Buf::f32(p, end, t.still))
            return false;
        t.cells = full != 0;
        t.hold = hold != 0;
        if (!t.cells) continue;
        uint16_t nc = 0;
        if (!Buf::i32(p, end, t.ox) || !Buf::i32(p, end, t.oy) || !Buf::i32(p, end, t.oz) ||
            !Buf::u16(p, end, nc) || nc > 8000)
            return false;
        t.body.resize(nc);
        for (uint16_t c = 0; c < nc; c++) {
            if (!Buf::u8(p, end, t.body[c].x) || !Buf::u8(p, end, t.body[c].y) ||
                !Buf::u8(p, end, t.body[c].z) || !Buf::u8(p, end, t.body[c].block) ||
                !Buf::u8(p, end, t.body[c].flags))
                return false;
        }
    }
    uint16_t ng = 0;
    if (!Buf::u16(p, end, ng) || ng > 32) return false;
    d.treesGone.resize(ng);
    for (uint16_t i = 0; i < ng; i++)
        if (!Buf::u32(p, end, d.treesGone[i])) return false;
    uint8_t tc = 0;
    if (!Buf::u8(p, end, tc)) return false;
    d.treeCheck = tc != 0;
    if (d.treeCheck && !Buf::u32(p, end, d.treeHash)) return false;
    uint8_t nm = 0;
    if (!Buf::u8(p, end, nm) || nm > 32) return false;
    d.mines.resize(nm);
    for (uint8_t i = 0; i < nm; i++) {
        MineNet& m = d.mines[i];
        uint8_t nh = 0;
        if (!Buf::u32(p, end, m.tree) || !Buf::i32(p, end, m.x) || !Buf::i32(p, end, m.y) ||
            !Buf::i32(p, end, m.z) || !Buf::f32(p, end, m.rem) || !Buf::u8(p, end, nh) || nh > 12)
            return false;
        m.hits.resize(nh);
        for (uint8_t h = 0; h < nh; h++) {
            if (!Buf::f32(p, end, m.hits[h].step) || !Buf::u8(p, end, m.hits[h].face))
                return false;
        }
    }
    uint8_t ngone = 0;
    if (!Buf::u8(p, end, ngone) || ngone > 32) return false;
    d.mineGone.resize(ngone);
    for (uint8_t i = 0; i < ngone; i++) {
        if (!Buf::u32(p, end, d.mineGone[i].tree) || !Buf::i32(p, end, d.mineGone[i].x) ||
            !Buf::i32(p, end, d.mineGone[i].y) || !Buf::i32(p, end, d.mineGone[i].z))
            return false;
    }
    if (!Buf::u32(p, end, d.inventoryRevision) || !Buf::u32(p, end, d.inventoryLayoutAck)) return false;
    for (auto& slot : d.inventory)
        if (!Buf::u8(p, end, slot.block) || !Buf::u8(p, end, slot.count) || !validWireSlot(slot)) return false;
    uint16_t ndrop = 0;
    if (!Buf::u16(p, end, ndrop) || ndrop > 128) return false;
    d.drops.resize(ndrop);
    for (auto& drop : d.drops) {
        uint8_t grounded = 0;
        if (!Buf::u32(p, end, drop.id) || !drop.id ||
            !Buf::f32(p, end, drop.x) || !Buf::f32(p, end, drop.y) || !Buf::f32(p, end, drop.z) ||
            !Buf::f32(p, end, drop.vx) || !Buf::f32(p, end, drop.vy) || !Buf::f32(p, end, drop.vz) ||
            !Buf::f32(p, end, drop.axx) || !Buf::f32(p, end, drop.axy) || !Buf::f32(p, end, drop.axz) ||
            !Buf::f32(p, end, drop.ayx) || !Buf::f32(p, end, drop.ayy) || !Buf::f32(p, end, drop.ayz) ||
            !Buf::f32(p, end, drop.azx) || !Buf::f32(p, end, drop.azy) || !Buf::f32(p, end, drop.azz) ||
            !Buf::f32(p, end, drop.avx) || !Buf::f32(p, end, drop.avy) || !Buf::f32(p, end, drop.avz) ||
            !Buf::f32(p, end, drop.age) ||
            !std::isfinite(drop.x) || !std::isfinite(drop.y) || !std::isfinite(drop.z) ||
            !std::isfinite(drop.vx) || !std::isfinite(drop.vy) || !std::isfinite(drop.vz) ||
            !std::isfinite(drop.axx) || !std::isfinite(drop.axy) || !std::isfinite(drop.axz) ||
            !std::isfinite(drop.ayx) || !std::isfinite(drop.ayy) || !std::isfinite(drop.ayz) ||
            !std::isfinite(drop.azx) || !std::isfinite(drop.azy) || !std::isfinite(drop.azz) ||
            !std::isfinite(drop.avx) || !std::isfinite(drop.avy) || !std::isfinite(drop.avz) ||
            !std::isfinite(drop.age) || drop.age < 0.0f ||
            !Buf::u8(p, end, drop.item) || drop.item == AIR || drop.item >= BLOCK_COUNT ||
            !Buf::u8(p, end, drop.count) || !drop.count || drop.count > cfg::MAX_STACK ||
            !Buf::u8(p, end, grounded) || grounded > 1) return false;
        drop.grounded = grounded != 0;
    }
    uint8_t ncombat = 0;
    if (!Buf::u8(p, end, ncombat) || ncombat > 32) return false;
    d.combat.resize(ncombat);
    for (auto& event : d.combat) {
        uint8_t kind = 0, limb = 0;
        if (!Buf::u32(p, end, event.serial) || !event.serial || !Buf::u8(p, end, kind) ||
            kind < (uint8_t)CombatEventKind::Hit || kind > (uint8_t)CombatEventKind::Death ||
            !Buf::u32(p, end, event.attacker) || !Buf::u32(p, end, event.target) ||
            !Buf::u8(p, end, limb) || !Buf::u16(p, end, event.amount)) return false;
        event.kind = (CombatEventKind)kind; event.limb = (int8_t)limb;
    }
    uint8_t nprojectiles = 0;
    if (!Buf::u8(p, end, nprojectiles) || nprojectiles > 64) return false;
    d.projectiles.resize(nprojectiles);
    for (auto& projectile : d.projectiles) {
        if (!Buf::u32(p, end, projectile.id) || !projectile.id ||
            !Buf::u32(p, end, projectile.owner) || !projectile.owner ||
            !Buf::f32(p, end, projectile.x) || !Buf::f32(p, end, projectile.y) ||
            !Buf::f32(p, end, projectile.z) || !Buf::f32(p, end, projectile.vx) ||
            !Buf::f32(p, end, projectile.vy) || !Buf::f32(p, end, projectile.vz) ||
            !std::isfinite(projectile.x) || !std::isfinite(projectile.y) || !std::isfinite(projectile.z) ||
            !std::isfinite(projectile.vx) || !std::isfinite(projectile.vy) || !std::isfinite(projectile.vz) ||
            std::fabs(projectile.x) > 100000 || std::fabs(projectile.y) > 100000 ||
            std::fabs(projectile.z) > 100000 || std::fabs(projectile.vx) > 1000 ||
            std::fabs(projectile.vy) > 1000 || std::fabs(projectile.vz) > 1000) return false;
    }
    uint8_t narcane = 0;
    if (!Buf::u8(p, end, narcane) || narcane > 32) return false;
    d.arcane.resize(narcane);
    for (auto& event : d.arcane) {
        uint8_t kind = 0;
        if (!Buf::u32(p, end, event.serial) || !event.serial || !Buf::u8(p, end, kind) ||
            kind != (uint8_t)ArcaneEventKind::FireballExplode ||
            !Buf::f32(p, end, event.x) || !Buf::f32(p, end, event.y) || !Buf::f32(p, end, event.z) ||
            !std::isfinite(event.x) || !std::isfinite(event.y) || !std::isfinite(event.z) ||
            std::fabs(event.x) > 100000 || std::fabs(event.y) > 100000 ||
            std::fabs(event.z) > 100000) return false;
        event.kind = (ArcaneEventKind)kind;
    }
    return true;
}
