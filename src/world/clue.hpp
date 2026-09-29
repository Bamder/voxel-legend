#pragma once
#include "blocks.hpp"
#include "../core/math.hpp"
#include <array>
#include <cstdint>
#include <unordered_map>

class World;

namespace clue {

enum class Destination : uint8_t {
    None = 0,
    Clue = 1,
    Arcane = 2,
    Boss = 3,
};

inline constexpr uint8_t kMaxStage = 32;
inline constexpr float kBossRewardRadius = 48.0f;

// Building code creates this on the Dedicated Server when it places a clue.
// Stages are strictly sequential per team. Boss rewardItem must be a relic.
struct Link {
    uint8_t team = 0;
    uint8_t stage = 0;
    Vec3 target{};
    Destination destination = Destination::None;
    uint8_t rewardItem = AIR;
};

struct Target {
    bool active = false;
    uint8_t stage = 0;
    Vec3 position{};
    Destination destination = Destination::None;
    uint8_t rewardItem = AIR;
    bool bossRewardClaimed = false;
};

bool validLink(const Link& link);
const char* destinationName(Destination destination);

// Match-owned authoritative clue state. Drop bindings never enter ItemSlot or
// client messages: the server resolves a picked-up drop id, then shares only the
// resulting target with that player's team.
class Director {
public:
    bool bindDrop(uint32_t dropId, const Link& link);
    bool knowsDrop(uint32_t dropId) const;
    bool canPickup(uint32_t dropId, int team) const;
    bool claimPickup(uint32_t dropId, int team);
    Target targetFor(int team) const;
    uint8_t currentStage(int team) const;

    // Future Boss code calls this only after the Dedicated Server confirms the
    // authoritative encounter death. Returns the spawned relic drop id, or 0.
    uint32_t completeBoss(World& world, int team, uint32_t encounterId, Vec3 dropPosition);

private:
    struct TeamState {
        Target target{};
        uint32_t completedEncounter = 0;
    };
    std::array<TeamState, 5> teams_{};
    std::unordered_map<uint32_t, Link> drops_;
};

} // namespace clue
