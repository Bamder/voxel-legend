#pragma once
#include "room_proto.hpp"
#include <cstdint>
#include <string>
#include <vector>

// TCP lobby on the host game process, then a dedicated --room-server process
// once the match starts. The host's own game is a client of that process.

class NetConn {
public:
    NetConn();
    explicit NetConn(uintptr_t sock);
    ~NetConn();
    NetConn(const NetConn&) = delete;
    NetConn& operator=(const NetConn&) = delete;
    NetConn(NetConn&& o) noexcept;
    NetConn& operator=(NetConn&& o) noexcept;

    bool valid() const;
    bool dead() const;
    void close();
    // Blocking linger so a final packet (match start) is delivered before FIN.
    void closeGraceful();
    void pump();
    bool pop(uint16_t& type, std::vector<uint8_t>& payload);
    void send(uint16_t type, const std::vector<uint8_t>& payload);
    bool flushOut(int timeoutMs);
    bool busy() const { return !out.empty(); }

private:
    uintptr_t sock;
    bool deadFlag = false;
    std::vector<uint8_t> in, out;
};

class LobbyHost {
public:
    bool open(uint16_t port, std::string& err);
    void close();
    bool listening() const { return listenSock != kInvalid; }
    uint16_t port() const { return boundPort; }
    int remoteCount() const { return (int)remotes.size(); }

    void poll();
    void broadcast(const std::vector<RoomTeamNet>& teams, const std::vector<RoomPlayerNet>& players);
    void sendMatchStart();
    bool flushOut(int timeoutMs);

    struct Join {
        uint32_t id = 0;
        std::string name;
    };
    struct TeamCmd {
        uint32_t id = 0;
        int team = -1;
    };
    std::vector<Join> takeJoins();
    std::vector<uint32_t> takeLeaves();
    std::vector<TeamCmd> takeTeamCmds();

private:
    static constexpr uintptr_t kInvalid = ~(uintptr_t)0;
    struct Remote {
        NetConn conn;
        uint32_t id = 0;
        bool welcomed = false;
    };
    uintptr_t listenSock = kInvalid;
    uint16_t boundPort = 0;
    uint32_t nextId = 2;
    std::vector<Remote> remotes;
    std::vector<Join> joins;
    std::vector<uint32_t> leaves;
    std::vector<TeamCmd> teams;
};

class LobbyGuest {
public:
    bool connect(const std::string& host, uint16_t port, const std::string& name, std::string& err);
    void close();
    void poll();
    void sendJoinTeam(int team);
    bool alive() const { return up; }
    bool matchStarting() const { return starting; }
    uint32_t localId() const { return id; }
    const std::string& host() const { return hostAddr; }
    uint16_t port() const { return hostPort; }
    bool takeLobby(uint16_t& port, std::vector<RoomTeamNet>& teams, std::vector<RoomPlayerNet>& players);

private:
    NetConn conn;
    bool up = false;
    bool starting = false;
    bool lobbyFresh = false;
    uint32_t id = 0;
    uint16_t lobbyPort = 0;
    std::string hostAddr;
    uint16_t hostPort = 0;
    std::vector<RoomTeamNet> teams;
    std::vector<RoomPlayerNet> players;
};

class GameClient {
public:
    // Retries until the dedicated server is listening (world create can take a moment).
    bool startConnect(const std::string& host, uint16_t port, const std::string& name, std::string& err);
    void close();
    void poll();
    bool handshaking() const { return phase == Phase::WaitWelcome; }
    bool connecting() const;
    bool welcomed() const { return haveWelcome; }
    bool failed() const { return dead; }
    const std::string& failReason() const { return why; }
    uint32_t selfId() const { return id; }
    uint32_t seed() const { return worldSeed; }
    float spawnX() const { return sx; }
    float spawnY() const { return sy; }
    float spawnZ() const { return sz; }
    bool spectator() const { return spec; }
    int team() const { return teamId; }
    void sendInput(const PlayInputNet& in);
    void sendDeploy(uint8_t action, int bx, int bz);
    void sendClueAnswer(uint32_t challengeId, uint8_t option);
    std::vector<PlayDeltaNet> takeDeltas();
    std::vector<ClueQuizNet> takeClueQuizzes();
    bool takeDeploy(std::vector<DeployPinNet>& out);

private:
    enum class Phase { Idle, Connecting, WaitWelcome, Play, Dead };
    bool dial(std::string& err);
    void fail(const std::string& reason);
    void pumpWelcome();

    Phase phase = Phase::Idle;
    NetConn conn;
    uintptr_t dialSock = ~(uintptr_t)0;
    std::string hostAddr = "127.0.0.1";
    uint16_t hostPort = kRoomPortDefault;
    std::string playerName;
    bool haveWelcome = false;
    bool dead = false;
    std::string why;
    uint32_t id = 0;
    uint32_t worldSeed = 1;
    float sx = 0, sy = 0, sz = 0;
    bool spec = false;
    int teamId = -1;
    std::vector<PlayDeltaNet> deltas;
    std::vector<ClueQuizNet> clueQuizzes;
    std::vector<DeployPinNet> deploySnap;
    bool deployFresh = false;
    long long deadlineMs = 0;
    long long retryAtMs = 0;
};

struct ServerProcess {
    void* process = nullptr;
    void* thread = nullptr;
    ServerProcess() = default;
    ~ServerProcess();
    ServerProcess(const ServerProcess&) = delete;
    ServerProcess& operator=(const ServerProcess&) = delete;
    ServerProcess(ServerProcess&& o) noexcept;
    ServerProcess& operator=(ServerProcess&& o) noexcept;
    void kill();
};

bool writeRoomHandoff(const std::string& utf8Path, const std::vector<RoomTeamNet>& teams,
                      const std::vector<RoomPlayerNet>& players, std::string& err);
bool readRoomHandoff(const std::string& utf8Path, std::vector<RoomTeamNet>& teams,
                     std::vector<RoomPlayerNet>& players);
bool spawnRoomServer(uint16_t port, const std::string& handoffUtf8, ServerProcess& proc,
                     std::string& err, bool clueQa = false);

int runRoomServer(uint16_t port, const std::string& handoffUtf8, bool clueQa = false);
