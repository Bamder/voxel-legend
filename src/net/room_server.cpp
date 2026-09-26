#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "room_net.hpp"
#include "../core/config.hpp"
#include "../world/world.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

struct LoggedEdit {
    int x, y, z;
    uint8_t block;
    uint32_t rev;
};

struct SentPose {
    float x, y, z, yaw, pitch;
    bool spectator;
    bool valid;
};

struct SClient {
    NetConn conn;
    uint32_t id = 0;
    std::string name;
    int team = -1;
    bool spectator = false;
    bool known = false;
    bool sentWelcome = false;
    float x = 0, y = 0, z = 0, yaw = 0, pitch = 0;
    uint32_t ack = 0;
    std::unordered_map<uint32_t, SentPose> sent;
    std::vector<uint32_t> announced;
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

void applyEdit(World& world, std::vector<LoggedEdit>& log, uint32_t& rev, const BlockEditNet& e) {
    if (e.y < 0 || e.y >= cfg::CHUNK_H) return;
    const float S = cfg::BLOCK_SCALE;
    world.update(Vec3{ (e.x + 0.5f) * S, (e.y + 0.5f) * S, (e.z + 0.5f) * S }, 1);
    int cx = floorDiv(e.x, cfg::CHUNK_X);
    int cz = floorDiv(e.z, cfg::CHUNK_Z);
    if (!world.chunkExists(cx, cz)) return;
    if (world.getBlock(e.x, e.y, e.z) == e.block) return;
    world.setBlock(e.x, e.y, e.z, e.block, false, false);
    log.push_back(LoggedEdit{ e.x, e.y, e.z, e.block, ++rev });
    if (log.size() > 8000) log.erase(log.begin(), log.begin() + 2000);
}

bool poseChanged(const SentPose& s, const SClient& c) {
    if (!s.valid) return true;
    if (s.spectator != c.spectator) return true;
    if (std::fabs(s.x - c.x) > 0.02f || std::fabs(s.y - c.y) > 0.02f || std::fabs(s.z - c.z) > 0.02f)
        return true;
    if (std::fabs(s.yaw - c.yaw) > 0.02f || std::fabs(s.pitch - c.pitch) > 0.02f) return true;
    return false;
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
    std::vector<LoggedEdit> logEdits;
    uint32_t rev = 0;
    uint32_t nextId = 1;
    int spawnSlot = 0;
    bool worldReady = false;
    bool hadClient = false;
    long long emptySince = nowMs();
    long long started = emptySince;

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
                    c.id = nextId++;
                    c.known = true;
                    hadClient = true;
                    float ox = (float)(spawnSlot % 4) * 1.5f;
                    float oz = (float)(spawnSlot / 4) * 1.5f;
                    spawnSlot++;
                    c.x = base.x + ox;
                    c.y = base.y;
                    c.z = base.z + oz;
                    c.yaw = 0.4f;
                    c.pitch = -0.15f;
                } else if (c.sentWelcome && type == (uint16_t)RoomMsg::PlayInput) {
                    float x, y, z, yaw, pitch;
                    bool spec = false;
                    uint32_t ack = 0;
                    std::vector<BlockEditNet> edits;
                    if (!decodePlayInput(p, end, x, y, z, yaw, pitch, spec, ack, edits)) continue;
                    c.x = x;
                    c.y = y;
                    c.z = z;
                    c.yaw = yaw;
                    c.pitch = pitch;
                    c.spectator = spec || c.team == 0;
                    if (ack > c.ack) c.ack = ack;
                    if (!c.spectator) {
                        for (const BlockEditNet& e : edits) applyEdit(world, logEdits, rev, e);
                    }
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
            for (SClient& c : clients) {
                if (!c.sentWelcome) continue;
                world.update(Vec3{ c.x, c.y, c.z }, 1);
            }
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
            std::vector<uint32_t> ids;
            for (const SClient& c : clients)
                if (c.sentWelcome) ids.push_back(c.id);

            for (SClient& c : clients) {
                if (!c.sentWelcome) continue;
                PlayDeltaNet d;
                d.baseRev = 0;
                uint32_t want = c.ack + 1;
                bool started = false;
                if (!logEdits.empty() && logEdits.front().rev > want) want = logEdits.front().rev;
                for (const LoggedEdit& e : logEdits) {
                    if (e.rev < want) continue;
                    if (e.rev != want) break;
                    if (!started) {
                        d.baseRev = e.rev;
                        started = true;
                    }
                    d.edits.push_back(BlockEditNet{ e.x, e.y, e.z, e.block });
                    want++;
                    if (d.edits.size() >= 48) break;
                }
                for (uint32_t old : c.announced) {
                    bool still = false;
                    for (uint32_t id : ids)
                        if (id == old) still = true;
                    if (!still && old != c.id) d.removed.push_back(old);
                }
                c.announced.clear();
                for (const SClient& o : clients) {
                    if (!o.sentWelcome || o.id == c.id) continue;
                    c.announced.push_back(o.id);
                    SentPose prev = c.sent[o.id];
                    if (!poseChanged(prev, o)) continue;
                    PlayerPoseNet pose;
                    pose.id = o.id;
                    pose.name = o.name;
                    pose.x = o.x;
                    pose.y = o.y;
                    pose.z = o.z;
                    pose.yaw = o.yaw;
                    pose.pitch = o.pitch;
                    pose.spectator = o.spectator;
                    d.players.push_back(pose);
                    c.sent[o.id] = SentPose{ o.x, o.y, o.z, o.yaw, o.pitch, o.spectator, true };
                }
                if (!d.edits.empty() || !d.players.empty() || !d.removed.empty())
                    c.conn.send((uint16_t)RoomMsg::PlayDelta, encodePlayDelta(d));
                c.conn.pump();
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
