#pragma once
#include "room_proto.hpp"
#include "../world/player.hpp"

// Dedicated-server state. No client-supplied health, velocity, grounded or fly state.
namespace room_body {
struct State {
    Player player;
    InputState input;
    uint32_t lastInputTick = 0;
    bool hasInput = false;
};
InputState movement(uint8_t bits);
MovementLimits limits(const Player& player, const InputState& input, bool frozen = false);
bool accept(State& state, const PlayInputNet& input, uint32_t serverTick);
void spawn(State& state, Vec3 position);
void tick(State& state, const World& world, uint32_t serverTick, bool active, bool mining);
BodyStateNet snapshot(const State& state, bool landed);
}
