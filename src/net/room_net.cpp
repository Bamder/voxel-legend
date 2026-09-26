#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "room_net.hpp"
#include "../world/saves.hpp"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

namespace {

constexpr uintptr_t kInvalid = ~(uintptr_t)0;

void netStartup() {
    static bool once = false;
    if (once) return;
    WSADATA w{};
    WSAStartup(MAKEWORD(2, 2), &w);
    once = true;
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

bool writeFile(const std::string& utf8, const std::vector<uint8_t>& bytes) {
    std::ofstream out(saves::utf8Path(utf8), std::ios::binary);
    if (!out) return false;
    if (!bytes.empty()) out.write((const char*)bytes.data(), (std::streamsize)bytes.size());
    return (bool)out;
}

bool readFile(const std::string& utf8, std::vector<uint8_t>& bytes) {
    std::ifstream in(saves::utf8Path(utf8), std::ios::binary);
    if (!in) return false;
    in.seekg(0, std::ios::end);
    std::streamoff n = in.tellg();
    if (n < 0 || n > 1024 * 1024) return false;
    in.seekg(0, std::ios::beg);
    bytes.resize((size_t)n);
    if (n > 0) in.read((char*)bytes.data(), n);
    return (bool)in || n == 0;
}

} // namespace

NetConn::NetConn() : sock(kInvalid) {}

NetConn::NetConn(uintptr_t s) : sock(s) {}

NetConn::~NetConn() { close(); }

NetConn::NetConn(NetConn&& o) noexcept : sock(o.sock), deadFlag(o.deadFlag), in(std::move(o.in)), out(std::move(o.out)) {
    o.sock = kInvalid;
    o.deadFlag = false;
}

NetConn& NetConn::operator=(NetConn&& o) noexcept {
    if (this != &o) {
        close();
        sock = o.sock;
        deadFlag = o.deadFlag;
        in = std::move(o.in);
        out = std::move(o.out);
        o.sock = kInvalid;
        o.deadFlag = false;
    }
    return *this;
}

bool NetConn::valid() const { return sock != kInvalid; }
bool NetConn::dead() const { return deadFlag || sock == kInvalid; }

void NetConn::close() {
    if (sock != kInvalid) {
        closesocket((SOCKET)sock);
        sock = kInvalid;
    }
    in.clear();
    out.clear();
    deadFlag = false;
}

void NetConn::closeGraceful() {
    if (sock == kInvalid) return;
    u_long mode = 0;
    ioctlsocket((SOCKET)sock, FIONBIO, &mode);
    DWORD sendWait = 500;
    setsockopt((SOCKET)sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&sendWait, sizeof(sendWait));
    while (!out.empty()) {
        int n = ::send((SOCKET)sock, (const char*)out.data(), (int)out.size(), 0);
        if (n > 0) out.erase(out.begin(), out.begin() + n);
        else break;
    }
    linger lin{};
    lin.l_onoff = 1;
    lin.l_linger = 1;
    setsockopt((SOCKET)sock, SOL_SOCKET, SO_LINGER, (const char*)&lin, sizeof(lin));
    shutdown((SOCKET)sock, SD_SEND);
    closesocket((SOCKET)sock);
    sock = kInvalid;
    in.clear();
    out.clear();
    deadFlag = false;
}

void NetConn::pump() {
    if (sock == kInvalid || deadFlag) return;
    uint8_t buf[4096];
    for (;;) {
        int n = recv((SOCKET)sock, (char*)buf, (int)sizeof(buf), 0);
        if (n > 0) {
            in.insert(in.end(), buf, buf + n);
            if (in.size() > (1u << 20)) {
                deadFlag = true;
                return;
            }
        } else if (n == 0) {
            deadFlag = true;
            return;
        } else {
            int e = WSAGetLastError();
            if (e == WSAEWOULDBLOCK) break;
            deadFlag = true;
            return;
        }
    }
    while (!out.empty()) {
        int n = ::send((SOCKET)sock, (const char*)out.data(), (int)out.size(), 0);
        if (n > 0) {
            out.erase(out.begin(), out.begin() + n);
        } else {
            int e = WSAGetLastError();
            if (e == WSAEWOULDBLOCK) break;
            deadFlag = true;
            return;
        }
    }
}

bool NetConn::pop(uint16_t& type, std::vector<uint8_t>& payload) {
    if (in.size() < 6) return false;
    uint32_t len = (uint32_t)in[0] | ((uint32_t)in[1] << 8) | ((uint32_t)in[2] << 16) | ((uint32_t)in[3] << 24);
    if (len > (1u << 20)) {
        deadFlag = true;
        return false;
    }
    if (in.size() < 6u + len) return false;
    type = (uint16_t)in[4] | ((uint16_t)in[5] << 8);
    payload.assign(in.begin() + 6, in.begin() + 6 + (int)len);
    in.erase(in.begin(), in.begin() + 6 + (int)len);
    return true;
}

void NetConn::send(uint16_t type, const std::vector<uint8_t>& payload) {
    if (sock == kInvalid || deadFlag) return;
    if (payload.size() > (1u << 20)) return;
    uint32_t len = (uint32_t)payload.size();
    uint8_t h[6] = {
        (uint8_t)(len & 255), (uint8_t)((len >> 8) & 255), (uint8_t)((len >> 16) & 255),
        (uint8_t)((len >> 24) & 255), (uint8_t)(type & 255), (uint8_t)((type >> 8) & 255)
    };
    out.insert(out.end(), h, h + 6);
    out.insert(out.end(), payload.begin(), payload.end());
}

bool NetConn::flushOut(int timeoutMs) {
    long long end = nowMs() + timeoutMs;
    while (!out.empty() && valid() && !dead() && nowMs() < end) {
        pump();
        if (!out.empty()) Sleep(5);
    }
    return out.empty() && !dead();
}

bool LobbyHost::open(uint16_t port, std::string& err) {
    netStartup();
    close();
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        err = "无法创建套接字";
        return false;
    }
    int reuse = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(s, (sockaddr*)&addr, sizeof(addr)) != 0 || listen(s, 8) != 0) {
        closesocket(s);
        err = "端口被占用或无法监听";
        return false;
    }
    setNonBlock(s);
    listenSock = (uintptr_t)s;
    boundPort = port;
    nextId = 2;
    return true;
}

void LobbyHost::close() {
    // Drop the listen socket first so a guest reconnecting for the match
    // cannot land back on this lobby while its start packet is still flushing.
    if (listenSock != kInvalid) {
        closesocket((SOCKET)listenSock);
        listenSock = kInvalid;
    }
    for (Remote& r : remotes) r.conn.closeGraceful();
    remotes.clear();
    joins.clear();
    leaves.clear();
    teams.clear();
    boundPort = 0;
}

void LobbyHost::poll() {
    if (listenSock == kInvalid) return;
    for (;;) {
        SOCKET c = accept((SOCKET)listenSock, nullptr, nullptr);
        if (c == INVALID_SOCKET) break;
        setNonBlock(c);
        Remote r;
        r.conn = NetConn((uintptr_t)c);
        remotes.push_back(std::move(r));
    }
    for (size_t i = 0; i < remotes.size();) {
        Remote& r = remotes[i];
        r.conn.pump();
        if (r.conn.dead()) {
            if (r.welcomed) leaves.push_back(r.id);
            r.conn.close();
            remotes.erase(remotes.begin() + (int)i);
            continue;
        }
        uint16_t type = 0;
        std::vector<uint8_t> payload;
        while (r.conn.pop(type, payload)) {
            const uint8_t* p = payload.data();
            const uint8_t* end = p + payload.size();
            if (!r.welcomed) {
                std::string name;
                if (type == (uint16_t)RoomMsg::Hello && decodeHello(p, end, name)) {
                    if (name.empty()) name = "玩家";
                    r.id = nextId++;
                    r.welcomed = true;
                    r.conn.send((uint16_t)RoomMsg::Welcome, encodeWelcome(r.id));
                    joins.push_back(Join{ r.id, name });
                } else {
                    r.conn.close();
                }
            } else if (type == (uint16_t)RoomMsg::JoinTeam) {
                int team = -1;
                if (decodeJoinTeam(p, end, team)) teams.push_back(TeamCmd{ r.id, team });
            }
        }
        if (r.conn.dead()) {
            if (r.welcomed) leaves.push_back(r.id);
            r.conn.close();
            remotes.erase(remotes.begin() + (int)i);
            continue;
        }
        i++;
    }
}

void LobbyHost::broadcast(const std::vector<RoomTeamNet>& teamList, const std::vector<RoomPlayerNet>& players) {
    std::vector<uint8_t> msg = encodeLobby(boundPort, 1, teamList, players);
    for (Remote& r : remotes) {
        if (!r.welcomed) continue;
        r.conn.send((uint16_t)RoomMsg::Lobby, msg);
        r.conn.pump();
    }
}

void LobbyHost::sendMatchStart() {
    std::vector<uint8_t> empty;
    for (Remote& r : remotes) {
        if (!r.welcomed) continue;
        r.conn.send((uint16_t)RoomMsg::MatchStart, empty);
    }
}

bool LobbyHost::flushOut(int timeoutMs) {
    long long end = nowMs() + timeoutMs;
    while (nowMs() < end) {
        bool pending = false;
        for (Remote& r : remotes) {
            r.conn.pump();
            if (r.conn.busy() && !r.conn.dead()) pending = true;
        }
        if (!pending) return true;
        Sleep(5);
    }
    return false;
}

std::vector<LobbyHost::Join> LobbyHost::takeJoins() {
    std::vector<Join> o;
    o.swap(joins);
    return o;
}
std::vector<uint32_t> LobbyHost::takeLeaves() {
    std::vector<uint32_t> o;
    o.swap(leaves);
    return o;
}
std::vector<LobbyHost::TeamCmd> LobbyHost::takeTeamCmds() {
    std::vector<TeamCmd> o;
    o.swap(teams);
    return o;
}

bool LobbyGuest::connect(const std::string& host, uint16_t port, const std::string& name, std::string& err) {
    netStartup();
    close();
    hostAddr = host.empty() ? "127.0.0.1" : host;
    hostPort = port;
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* res = nullptr;
    std::string portStr = std::to_string(port);
    if (getaddrinfo(hostAddr.c_str(), portStr.c_str(), &hints, &res) != 0 || !res) {
        err = "地址无效";
        return false;
    }
    SOCKET s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s == INVALID_SOCKET) {
        freeaddrinfo(res);
        err = "无法创建套接字";
        return false;
    }
    setNonBlock(s);
    int cr = ::connect(s, res->ai_addr, (int)res->ai_addrlen);
    freeaddrinfo(res);
    if (cr != 0 && WSAGetLastError() != WSAEWOULDBLOCK) {
        closesocket(s);
        err = "无法连接主机";
        return false;
    }
    fd_set wset;
    FD_ZERO(&wset);
    FD_SET(s, &wset);
    timeval tv{};
    tv.tv_sec = 3;
    int sel = select(0, nullptr, &wset, nullptr, &tv);
    if (sel <= 0) {
        closesocket(s);
        err = "连接超时";
        return false;
    }
    int soerr = 0;
    int slen = sizeof(soerr);
    getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&soerr, &slen);
    if (soerr != 0) {
        closesocket(s);
        err = "无法连接主机";
        return false;
    }
    conn = NetConn((uintptr_t)s);
    std::string who = name.empty() ? "玩家" : name;
    conn.send((uint16_t)RoomMsg::Hello, encodeHello(who));
    long long end = nowMs() + 2000;
    while (nowMs() < end && !up) {
        conn.pump();
        if (conn.dead()) {
            err = "与主机断开";
            close();
            return false;
        }
        uint16_t type = 0;
        std::vector<uint8_t> payload;
        while (conn.pop(type, payload)) {
            const uint8_t* p = payload.data();
            const uint8_t* e = p + payload.size();
            if (type == (uint16_t)RoomMsg::Welcome) {
                if (!decodeWelcome(p, e, id)) {
                    err = "房间协议不匹配";
                    close();
                    return false;
                }
                up = true;
            } else if (type == (uint16_t)RoomMsg::Lobby) {
                uint32_t hostId = 0;
                if (decodeLobby(p, e, lobbyPort, hostId, teams, players)) lobbyFresh = true;
            }
        }
        if (!up) Sleep(5);
    }
    if (!up) {
        err = "主机无响应";
        close();
        return false;
    }
    return true;
}

void LobbyGuest::close() {
    conn.close();
    up = false;
    starting = false;
    lobbyFresh = false;
    id = 0;
    teams.clear();
    players.clear();
}

void LobbyGuest::poll() {
    // A match-start packet often arrives in the same read as the host's FIN.
    // Drain the buffer before treating the socket as a plain disconnect.
    if (conn.valid()) conn.pump();
    uint16_t type = 0;
    std::vector<uint8_t> payload;
    while (conn.pop(type, payload)) {
        const uint8_t* p = payload.data();
        const uint8_t* e = p + payload.size();
        if (type == (uint16_t)RoomMsg::Lobby) {
            uint32_t hostId = 0;
            if (decodeLobby(p, e, lobbyPort, hostId, teams, players)) lobbyFresh = true;
        } else if (type == (uint16_t)RoomMsg::MatchStart) {
            starting = true;
        }
    }
    if (!conn.valid() || conn.dead()) up = false;
}

bool LobbyGuest::takeLobby(uint16_t& port, std::vector<RoomTeamNet>& outTeams, std::vector<RoomPlayerNet>& outPlayers) {
    if (!lobbyFresh) return false;
    lobbyFresh = false;
    port = lobbyPort;
    outTeams = teams;
    outPlayers = players;
    return true;
}

void LobbyGuest::sendJoinTeam(int team) {
    if (!up) return;
    conn.send((uint16_t)RoomMsg::JoinTeam, encodeJoinTeam(team));
    conn.pump();
}

bool GameClient::dial(std::string& err) {
    if (dialSock != kInvalid) {
        closesocket((SOCKET)dialSock);
        dialSock = kInvalid;
    }
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* res = nullptr;
    std::string portStr = std::to_string(hostPort);
    if (getaddrinfo(hostAddr.c_str(), portStr.c_str(), &hints, &res) != 0 || !res) {
        err = "地址无效";
        return false;
    }
    SOCKET s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s == INVALID_SOCKET) {
        freeaddrinfo(res);
        err = "无法创建套接字";
        return false;
    }
    setNonBlock(s);
    int cr = ::connect(s, res->ai_addr, (int)res->ai_addrlen);
    int werr = WSAGetLastError();
    freeaddrinfo(res);
    if (cr != 0 && werr != WSAEWOULDBLOCK) {
        closesocket(s);
        err = "无法连接服务器";
        return false;
    }
    dialSock = (uintptr_t)s;
    phase = Phase::Connecting;
    return true;
}

void GameClient::fail(const std::string& reason) {
    why = reason;
    dead = true;
    phase = Phase::Dead;
    if (dialSock != kInvalid) {
        closesocket((SOCKET)dialSock);
        dialSock = kInvalid;
    }
    conn.close();
}

bool GameClient::startConnect(const std::string& host, uint16_t port, const std::string& name, std::string& err) {
    netStartup();
    close();
    hostAddr = host.empty() ? "127.0.0.1" : host;
    hostPort = port;
    playerName = name.empty() ? "玩家" : name;
    dead = false;
    haveWelcome = false;
    why.clear();
    deadlineMs = nowMs() + 20000;
    retryAtMs = 0;
    if (!dial(err)) {
        fail(err.empty() ? "无法连接服务器" : err);
        return false;
    }
    return true;
}

void GameClient::close() {
    if (dialSock != kInvalid) {
        closesocket((SOCKET)dialSock);
        dialSock = kInvalid;
    }
    conn.close();
    phase = Phase::Idle;
    haveWelcome = false;
    dead = false;
    why.clear();
    deltas.clear();
    id = 0;
}

bool GameClient::connecting() const {
    return phase == Phase::Connecting || phase == Phase::WaitWelcome;
}

void GameClient::pumpWelcome() {
    conn.pump();
    if (conn.dead()) {
        if (nowMs() < deadlineMs && !haveWelcome) {
            conn.close();
            retryAtMs = nowMs() + 200;
            phase = Phase::Connecting;
            dialSock = kInvalid;
            std::string err;
            if (!dial(err)) fail(err.empty() ? "无法连接服务器" : err);
            return;
        }
        fail("与服务器断开");
        return;
    }
    uint16_t type = 0;
    std::vector<uint8_t> payload;
    while (conn.pop(type, payload)) {
        const uint8_t* p = payload.data();
        const uint8_t* e = p + payload.size();
        if (type == (uint16_t)RoomMsg::PlayWelcome && !haveWelcome) {
            if (!decodePlayWelcome(p, e, id, worldSeed, sx, sy, sz, spec, teamId)) {
                fail("服务器协议不匹配");
                return;
            }
            haveWelcome = true;
            phase = Phase::Play;
        } else if (type == (uint16_t)RoomMsg::PlayDelta && haveWelcome) {
            PlayDeltaNet d;
            if (decodePlayDelta(p, e, d)) deltas.push_back(std::move(d));
        }
    }
}

void GameClient::poll() {
    if (phase == Phase::Dead || phase == Phase::Idle) return;
    if (nowMs() > deadlineMs && !haveWelcome) {
        fail("无法连接服务器");
        return;
    }
    if (phase == Phase::Connecting) {
        if (dialSock == kInvalid) {
            if (nowMs() < retryAtMs) return;
            std::string err;
            if (!dial(err)) fail(err.empty() ? "无法连接服务器" : err);
            return;
        }
        fd_set wset;
        FD_ZERO(&wset);
        FD_SET((SOCKET)dialSock, &wset);
        timeval tv{};
        int sel = select(0, nullptr, &wset, nullptr, &tv);
        if (sel <= 0) return;
        int soerr = 0;
        int slen = sizeof(soerr);
        getsockopt((SOCKET)dialSock, SOL_SOCKET, SO_ERROR, (char*)&soerr, &slen);
        if (soerr != 0) {
            closesocket((SOCKET)dialSock);
            dialSock = kInvalid;
            retryAtMs = nowMs() + 200;
            return;
        }
        conn = NetConn(dialSock);
        dialSock = kInvalid;
        conn.send((uint16_t)RoomMsg::PlayHello, encodeHello(playerName));
        phase = Phase::WaitWelcome;
    }
    if (phase == Phase::WaitWelcome || phase == Phase::Play) pumpWelcome();
}

void GameClient::sendInput(float x, float y, float z, float yaw, float pitch, bool spectator, uint32_t ack,
                           const std::vector<BlockEditNet>& edits) {
    if (phase != Phase::Play) return;
    conn.send((uint16_t)RoomMsg::PlayInput, encodePlayInput(x, y, z, yaw, pitch, spectator, ack, edits));
    conn.pump();
}

std::vector<PlayDeltaNet> GameClient::takeDeltas() {
    std::vector<PlayDeltaNet> o;
    o.swap(deltas);
    return o;
}

ServerProcess::~ServerProcess() { kill(); }

ServerProcess::ServerProcess(ServerProcess&& o) noexcept : process(o.process), thread(o.thread) {
    o.process = nullptr;
    o.thread = nullptr;
}

ServerProcess& ServerProcess::operator=(ServerProcess&& o) noexcept {
    if (this != &o) {
        kill();
        process = o.process;
        thread = o.thread;
        o.process = nullptr;
        o.thread = nullptr;
    }
    return *this;
}

void ServerProcess::kill() {
    if (process) {
        TerminateProcess((HANDLE)process, 0);
        CloseHandle((HANDLE)process);
        process = nullptr;
    }
    if (thread) {
        CloseHandle((HANDLE)thread);
        thread = nullptr;
    }
}

bool writeRoomHandoff(const std::string& utf8Path, const std::vector<RoomTeamNet>& teams,
                      const std::vector<RoomPlayerNet>& players, std::string& err) {
    Buf b;
    b.u32(0x534C5652u); // 'RVLS' little-endian marker
    b.u16(kRoomProto);
    b.u16((uint16_t)teams.size());
    for (const RoomTeamNet& t : teams) {
        b.str(t.name);
        b.f32(t.r);
        b.f32(t.g);
        b.f32(t.b);
        b.u8(t.spectator ? 1 : 0);
    }
    b.u16((uint16_t)players.size());
    for (const RoomPlayerNet& p : players) {
        b.u32(p.id);
        b.str(p.name);
        b.i32(p.team);
        b.u8(p.host ? 1 : 0);
    }
    std::error_code ec;
    std::filesystem::create_directories(saves::utf8Path(utf8Path).parent_path(), ec);
    if (!writeFile(utf8Path, b.data())) {
        err = "无法写入房间交接";
        return false;
    }
    return true;
}

bool readRoomHandoff(const std::string& utf8Path, std::vector<RoomTeamNet>& teams,
                     std::vector<RoomPlayerNet>& players) {
    teams.clear();
    players.clear();
    std::vector<uint8_t> bytes;
    if (!readFile(utf8Path, bytes) || bytes.size() < 8) return false;
    const uint8_t* p = bytes.data();
    const uint8_t* end = p + bytes.size();
    uint32_t magic = 0;
    uint16_t proto = 0;
    if (!Buf::u32(p, end, magic) || magic != 0x534C5652u) return false;
    if (!Buf::u16(p, end, proto) || proto != kRoomProto) return false;
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

bool spawnRoomServer(uint16_t port, const std::string& handoffUtf8, ServerProcess& proc, std::string& err) {
    proc.kill();
    wchar_t exe[MAX_PATH] = {};
    if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) {
        err = "无法定位游戏程序";
        return false;
    }
    std::filesystem::path hop = saves::utf8Path(handoffUtf8);
    std::wstring hopW = hop.wstring();
    std::wstring cmd = L"\"";
    cmd += exe;
    cmd += L"\" --room-server --port ";
    cmd += std::to_wstring(port);
    cmd += L" --handoff \"";
    cmd += hopW;
    cmd += L"\"";
    std::vector<wchar_t> mutableCmd(cmd.begin(), cmd.end());
    mutableCmd.push_back(0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe, mutableCmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        err = "无法启动服务器进程";
        return false;
    }
    proc.process = pi.hProcess;
    proc.thread = pi.hThread;
    return true;
}
