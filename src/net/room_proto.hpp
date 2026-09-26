#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// Lobby + match messages. Little-endian, length-prefixed by the socket layer.
constexpr uint16_t kRoomPortDefault = 35535;
constexpr uint16_t kRoomProto = 1;

enum class RoomMsg : uint16_t {
    Hello = 1,       // lobby: u16 proto, string name
    Welcome = 2,     // u32 id
    Lobby = 3,       // port, teams, players
    JoinTeam = 4,    // i32 team
    MatchStart = 5,  // empty; guests reconnect to the dedicated server
    PlayHello = 6,   // u16 proto, string name
    PlayWelcome = 7, // id, seed, spawn, spectator, team
    PlayInput = 8,   // pose + block edits + world ack
    PlayDelta = 9    // per-client player/world increment
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

struct PlayerPoseNet {
    uint32_t id = 0;
    std::string name;
    float x = 0, y = 0, z = 0, yaw = 0, pitch = 0;
    bool spectator = false;
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

private:
    std::vector<uint8_t> d;
};

inline std::vector<uint8_t> encodeHello(const std::string& name) {
    Buf b;
    b.u16(kRoomProto);
    b.str(name);
    return b.data();
}

inline bool decodeHello(const uint8_t* p, const uint8_t* end, std::string& name) {
    uint16_t proto = 0;
    if (!Buf::u16(p, end, proto) || proto != kRoomProto) return false;
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

inline std::vector<uint8_t> encodePlayInput(float x, float y, float z, float yaw, float pitch,
                                            bool spectator, uint32_t ack,
                                            const std::vector<BlockEditNet>& edits) {
    Buf b;
    b.f32(x);
    b.f32(y);
    b.f32(z);
    b.f32(yaw);
    b.f32(pitch);
    b.u8(spectator ? 1 : 0);
    b.u32(ack);
    uint16_t n = (uint16_t)(edits.size() > 32 ? 32 : edits.size());
    b.u16(n);
    for (uint16_t i = 0; i < n; i++) {
        b.i32(edits[i].x);
        b.i32(edits[i].y);
        b.i32(edits[i].z);
        b.u8(edits[i].block);
    }
    return b.data();
}

inline bool decodePlayInput(const uint8_t* p, const uint8_t* end, float& x, float& y, float& z,
                            float& yaw, float& pitch, bool& spectator, uint32_t& ack,
                            std::vector<BlockEditNet>& edits) {
    edits.clear();
    uint8_t spec = 0;
    uint16_t n = 0;
    if (!Buf::f32(p, end, x) || !Buf::f32(p, end, y) || !Buf::f32(p, end, z) ||
        !Buf::f32(p, end, yaw) || !Buf::f32(p, end, pitch) || !Buf::u8(p, end, spec) ||
        !Buf::u32(p, end, ack) || !Buf::u16(p, end, n) || n > 32)
        return false;
    spectator = spec != 0;
    edits.resize(n);
    for (uint16_t i = 0; i < n; i++) {
        if (!Buf::i32(p, end, edits[i].x) || !Buf::i32(p, end, edits[i].y) ||
            !Buf::i32(p, end, edits[i].z) || !Buf::u8(p, end, edits[i].block))
            return false;
    }
    return true;
}

struct PlayDeltaNet {
    uint32_t baseRev = 0;
    std::vector<uint32_t> removed;
    std::vector<PlayerPoseNet> players;
    std::vector<BlockEditNet> edits;
};

inline std::vector<uint8_t> encodePlayDelta(const PlayDeltaNet& d) {
    Buf b;
    b.u32(d.baseRev);
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
        b.u8(pl.spectator ? 1 : 0);
    }
    b.u16((uint16_t)d.edits.size());
    for (const BlockEditNet& e : d.edits) {
        b.i32(e.x);
        b.i32(e.y);
        b.i32(e.z);
        b.u8(e.block);
    }
    return b.data();
}

inline bool decodePlayDelta(const uint8_t* p, const uint8_t* end, PlayDeltaNet& d) {
    d = PlayDeltaNet{};
    if (!Buf::u32(p, end, d.baseRev)) return false;
    uint16_t nr = 0;
    if (!Buf::u16(p, end, nr) || nr > 32) return false;
    d.removed.resize(nr);
    for (uint16_t i = 0; i < nr; i++)
        if (!Buf::u32(p, end, d.removed[i])) return false;
    uint16_t np = 0;
    if (!Buf::u16(p, end, np) || np > 32) return false;
    d.players.resize(np);
    for (uint16_t i = 0; i < np; i++) {
        uint8_t spec = 0;
        PlayerPoseNet& pl = d.players[i];
        if (!Buf::u32(p, end, pl.id) || !Buf::str(p, end, pl.name) || !Buf::f32(p, end, pl.x) ||
            !Buf::f32(p, end, pl.y) || !Buf::f32(p, end, pl.z) || !Buf::f32(p, end, pl.yaw) ||
            !Buf::f32(p, end, pl.pitch) || !Buf::u8(p, end, spec))
            return false;
        pl.spectator = spec != 0;
    }
    uint16_t ne = 0;
    if (!Buf::u16(p, end, ne) || ne > 48) return false;
    d.edits.resize(ne);
    for (uint16_t i = 0; i < ne; i++) {
        if (!Buf::i32(p, end, d.edits[i].x) || !Buf::i32(p, end, d.edits[i].y) ||
            !Buf::i32(p, end, d.edits[i].z) || !Buf::u8(p, end, d.edits[i].block))
            return false;
    }
    return true;
}
