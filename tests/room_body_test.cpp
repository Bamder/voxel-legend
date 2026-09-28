#include "../src/net/room_body.hpp"
#include "../src/plugin/plugin.hpp"
#include <cstdlib>
#include <iostream>
#include <limits>

namespace {
int checks = 0;
void check(bool value, const char* message) {
    ++checks;
    if (!value) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
}

int main() {
    room_body::State state;
    room_body::spawn(state, {1,2,3});
    PlayInputNet input;
    input.x = 9999; input.y = 9999; input.vx = 9999;
    input.flags = kPfFly | kPfGround; input.spectator = true;
    input.movement = kMoveForward | kMoveJump | kMoveSprint;
    check(room_body::accept(state, input, 42), "valid movement intent accepted");
    check(state.player.pos.x == 1 && state.player.pos.y == 2 && state.player.vel.x == 0,
          "client cannot write server position or velocity");
    check(!state.player.flying && !state.player.onGround && !state.player.privilegeMode,
          "client flags cannot grant flight or grounded state");
    check(state.input.forward && state.input.jump && state.input.sprint && state.lastInputTick == 42,
          "input intent and server receipt tick retained");
    const float nan = std::numeric_limits<float>::quiet_NaN();
    input.yaw = nan;
    check(!room_body::accept(state, input, 43) && state.lastInputTick == 42, "NaN aim rejected atomically");
    input.yaw = 0; input.pitch = 2;
    check(!room_body::accept(state, input, 43), "invalid pitch rejected");
    input.pitch = 0; input.movement = 128;
    check(!room_body::accept(state, input, 43), "reserved movement bits rejected");
    state.player.vitals.limb[vitals::FootL].health = 0;
    state.player.onGround = true;
    InputState move; move.forward = true;
    auto constraints = room_body::limits(state.player, move);
    check(!constraints.horizontal && !constraints.sprint, "one foot requires hop");
    move.jump = true;
    constraints = room_body::limits(state.player, move);
    check(constraints.horizontal && constraints.jump && constraints.speed < 1, "hop is slower");
    state.player.vitals.limb[vitals::FootR].health = 0;
    constraints = room_body::limits(state.player, move);
    check(!constraints.horizontal && !constraints.jump, "both feet disabled");
    constraints = room_body::limits(state.player, move, true);
    check(!constraints.horizontal && !constraints.jump, "frozen constraints");
    state.player.vitals.limb[vitals::Head].health = 0;
    room_body::spawn(state, {4,5,6});
    check(!state.hasInput && state.player.vitals.limb[vitals::Head].health == 1, "server spawn resets body and old input");

    input = {};
    input.movement = kMoveLeft | kMoveSneak;
    auto bytes = encodePlayInput(input);
    PlayInputNet decoded;
    check(decodePlayInput(bytes.data(), bytes.data()+bytes.size(), decoded) && decoded.movement == input.movement,
          "movement protocol roundtrip");
    for (size_t n = 0; n < bytes.size(); ++n)
        check(!decodePlayInput(bytes.data(), bytes.data()+n, decoded), "all truncated input packets rejected");
    bytes[0] = 128;
    check(!decodePlayInput(bytes.data(), bytes.data()+bytes.size(), decoded), "wire movement bits validated");
    input.edits.resize(40); input.resync.resize(10); input.bark.resize(12); input.mines.resize(15);
    bytes = encodePlayInput(input);
    check(decodePlayInput(bytes.data(), bytes.data()+bytes.size(), decoded) &&
          decoded.edits.size() == 32 && decoded.resync.size() == 8 && decoded.bark.size() == 8 && decoded.mines.size() == 8,
          "existing input vector bounds preserved");

    PlayDeltaNet delta;
    state.player.vitals.limb[vitals::HandL].health = .125f;
    state.player.vitals.thirst = .5f;
    state.player.fatigue.emptied[vitals::FootL] = true;
    state.player.fatigue.recoverDelay[vitals::FootL] = 2.f;
    delta.body = room_body::snapshot(state, true);
    delta.serverTick = 88;
    bytes = encodePlayDelta(delta);
    PlayDeltaNet output;
    check(decodePlayDelta(bytes.data(), bytes.data()+bytes.size(), output) &&
          output.body.x == 4 && output.body.vitals.limb[vitals::HandL].health == .125f &&
          output.body.vitals.thirst == .5f && output.body.flags == 1 && output.serverTick == 88,
          "authority body roundtrip");
    check(output.body.fatigue.emptied[vitals::FootL] && output.body.fatigue.recoverDelay[vitals::FootL] == 2.f,
          "prediction fatigue state roundtrip");
    for (size_t n = 0; n < bytes.size(); ++n)
        check(!decodePlayDelta(bytes.data(), bytes.data()+n, output), "all truncated authority snapshots rejected");
    delta.body.vitals.limb[0].health = nan;
    bytes = encodePlayDelta(delta);
    check(!decodePlayDelta(bytes.data(), bytes.data()+bytes.size(), output), "NaN health rejected");
    delta.body.vitals.limb[0].health = 2;
    bytes = encodePlayDelta(delta);
    check(!decodePlayDelta(bytes.data(), bytes.data()+bytes.size(), output), "out of range health rejected");
    delta.body.vitals.limb[0].health = 1;
    delta.players.resize(65);
    bytes = encodePlayDelta(delta);
    check(!decodePlayDelta(bytes.data(), bytes.data()+bytes.size(), output), "oversize player vector rejected");

    // A deterministic flat test arena; never spawned in the actual game world.
    plugin::init();
    World world(7);
    world.setSaveEnabled(false);
    std::vector<uint8_t> blocks(cfg::CHUNK_VOLUME, AIR), zeros(cfg::CHUNK_VOLUME, 0);
    for (int z = 0; z < cfg::CHUNK_Z; ++z)
        for (int x = 0; x < cfg::CHUNK_X; ++x) {
            blocks[z * cfg::CHUNK_X + x] = STONE;
            for (int y = 1; y < 8; ++y)
                blocks[(y * cfg::CHUNK_Z + 10) * cfg::CHUNK_X + x] = STONE;
        }
    world.writeAuthChunk(0, 0, 0, blocks.data(), zeros.data(), zeros.data(), {}, {});
    room_body::spawn(state, {8,.5001f,8});
    input = {}; input.movement = kMoveForward;
    for (uint32_t tick = 1; tick <= 40; ++tick) {
        room_body::accept(state, input, tick);
        room_body::tick(state, world, tick, true, false);
    }
    check(state.player.pos.z >= 5.79f && state.player.pos.z < 6, "server physics stops at solid wall");
    room_body::spawn(state, {8,.5001f,8});
    auto once = state;
    for (int packet = 0; packet < 100; ++packet) room_body::accept(state, input, 1);
    room_body::accept(once, input, 1);
    room_body::tick(state, world, 1, true, false);
    room_body::tick(once, world, 1, true, false);
    check(state.player.pos.z == once.player.pos.z, "packet flood cannot accelerate simulation");
    room_body::spawn(state, {8,.5001f,8});
    room_body::accept(state, input, 1);
    room_body::tick(state, world, 12, true, false);
    check(state.player.pos.z == 8, "stale input stops movement");
    room_body::accept(state, input, 13);
    room_body::tick(state, world, 13, false, false);
    check(state.player.pos.z == 8, "undeployed player cannot move");
    state.player.vitals.limb[vitals::FootL].health = 0;
    state.player.vitals.limb[vitals::FootR].health = 0;
    state.player.vel = {5,0,5};
    input.movement |= kMoveJump;
    room_body::accept(state, input, 14);
    room_body::tick(state, world, 14, true, false);
    check(state.player.pos.x == 8 && state.player.pos.z == 8 && state.player.vel.y <= 0,
          "both feet loss stops momentum and jump in real physics");
    state.player.vitals.limb[vitals::Head].health = 0;
    room_body::tick(state, world, 15, true, false);
    check(state.player.dead && state.player.vel.lengthSq() == 0, "server death stops simulation");
    std::cout << "room body: " << checks << " checks passed\n";
}
