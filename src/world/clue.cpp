#include "clue.hpp"
#include "matchmap.hpp"
#include "world.hpp"
#include <cmath>

namespace clue {
namespace {

bool finitePosition(Vec3 position) {
    return std::isfinite(position.x) && std::isfinite(position.y) && std::isfinite(position.z) &&
        std::fabs(position.x) <= 100000.0f && std::fabs(position.y) <= 100000.0f &&
        std::fabs(position.z) <= 100000.0f;
}

bool combatTeam(int team) { return team >= 1 && team <= matchmap::kCombatTeams; }
bool relic(uint8_t item) { return item >= ITEM_ELEM_CORE && item <= ITEM_EYELESS; }

} // namespace

bool validLink(const Link& link) {
    if (!combatTeam(link.team) || link.stage < 1 || link.stage > kMaxStage ||
        !finitePosition(link.target) || link.destination < Destination::Clue ||
        link.destination > Destination::Boss) return false;
    if (link.destination == Destination::Boss) return relic(link.rewardItem);
    return link.rewardItem == AIR || (validBlock(link.rewardItem) && !relic(link.rewardItem));
}

const char* destinationName(Destination destination) {
    switch (destination) {
        case Destination::Clue: return "线索建筑";
        case Destination::Arcane: return "奥术藏匿点";
        case Destination::Boss: return "Boss 巢穴";
        default: return "未知目标";
    }
}

bool Director::bindDrop(uint32_t dropId, const Link& link) {
    if (!dropId || !validLink(link) || drops_.find(dropId) != drops_.end()) return false;
    drops_.emplace(dropId, link);
    return true;
}

bool Director::knowsDrop(uint32_t dropId) const {
    return drops_.find(dropId) != drops_.end();
}

bool Director::canPickup(uint32_t dropId, int team) const {
    auto found = drops_.find(dropId);
    if (found == drops_.end() || !combatTeam(team) || found->second.team != team) return false;
    const TeamState& state = teams_[(size_t)team];
    if (state.target.active && state.target.destination == Destination::Boss &&
        !state.target.bossRewardClaimed) return false;
    uint8_t expected = state.target.active ? (uint8_t)(state.target.stage + 1) : 1;
    return expected <= kMaxStage && found->second.stage == expected;
}

bool Director::claimPickup(uint32_t dropId, int team) {
    if (!canPickup(dropId, team)) return false;
    auto found = drops_.find(dropId);
    const Link link = found->second;
    drops_.erase(found);
    TeamState& state = teams_[(size_t)team];
    state.target.active = true;
    state.target.stage = link.stage;
    state.target.position = link.target;
    state.target.destination = link.destination;
    state.target.rewardItem = link.rewardItem;
    state.target.bossRewardClaimed = false;
    state.completedEncounter = 0;
    return true;
}

Target Director::targetFor(int team) const {
    return combatTeam(team) ? teams_[(size_t)team].target : Target{};
}

uint8_t Director::currentStage(int team) const {
    Target target = targetFor(team);
    return target.active ? target.stage : 0;
}

uint32_t Director::completeBoss(World& world, int team, uint32_t encounterId, Vec3 dropPosition) {
    if (!combatTeam(team) || !encounterId || !finitePosition(dropPosition)) return 0;
    TeamState& state = teams_[(size_t)team];
    Target& target = state.target;
    if (!target.active || target.destination != Destination::Boss ||
        target.bossRewardClaimed || state.completedEncounter == encounterId ||
        !relic(target.rewardItem)) return 0;
    Vec3 delta = dropPosition - target.position;
    if (delta.lengthSq() > kBossRewardRadius * kBossRewardRadius) return 0;
    uint32_t dropId = world.spawnDrop(dropPosition, target.rewardItem, 1, true);
    if (!dropId) return 0;
    target.bossRewardClaimed = true;
    state.completedEncounter = encounterId;
    return dropId;
}

} // namespace clue
