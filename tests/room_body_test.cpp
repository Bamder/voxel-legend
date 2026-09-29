#include "../src/net/room_body.hpp"
#include "../src/net/room_inventory.hpp"
#include "../src/plugin/plugin.hpp"
#include "../src/world/building_loot.hpp"
#include "../src/world/guide.hpp"
#include "../src/world/ritual.hpp"
#include "../src/world/structure.hpp"
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
    input.selectedLeft = 2; input.selectedRight = 1;
    input.attackSequence = 9; input.attackHand = 1;
    input.castSequence = 10; input.castHand = 0;
    input.pickupSequence = 4; input.pickupDrop = 77;
    input.layoutSequence = 5; input.layoutBaseRevision = 3;
    input.layout[0] = {HAND_PICK,1}; input.combatAck = 12; input.arcaneAck = 14;
    input.guardianSequence = 6; input.guardianRelic = 4;
    auto bytes = encodePlayInput(input);
    PlayInputNet decoded;
    check(decodePlayInput(bytes.data(), bytes.data()+bytes.size(), decoded) && decoded.movement == input.movement &&
          decoded.attackSequence == 9 && decoded.pickupDrop == 77 &&
          decoded.castSequence == 10 && decoded.castHand == 0 &&
          decoded.layout[0].block == HAND_PICK && decoded.combatAck == 12 && decoded.arcaneAck == 14 &&
          decoded.guardianSequence == 6 && decoded.guardianRelic == 4,
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
    delta.inventoryRevision = 7; delta.inventoryLayoutAck = 5;
    delta.inventory[4] = {HAND_AXE,1};
    DropNet wireDrop;
    wireDrop.id = 22; wireDrop.x = 8; wireDrop.y = 1; wireDrop.z = 8;
    wireDrop.vx = .25f; wireDrop.axx = 0; wireDrop.axz = 1;
    wireDrop.azx = -1; wireDrop.azz = 0; wireDrop.avy = .5f; wireDrop.age = 2;
    wireDrop.item = HAND_PICK; wireDrop.count = 1; wireDrop.grounded = true;
    delta.drops.push_back(wireDrop);
    delta.combat.push_back({13,1,2,CombatEventKind::Hit,vitals::HandL,1400});
    PlayerPoseNet burningPose; burningPose.id = 2;
    burningPose.status = kStatusBurning | kStatusFrozen | kStatusHealing;
    delta.players.push_back(burningPose);
    delta.projectiles.push_back({31,1,ArcaneProjectileKind::Freeze,2,3,4,5,6,7});
    delta.arcane.push_back({17,ArcaneEventKind::HealPulse,8,9,10});
    delta.clue = {true, 3, (uint8_t)clue::Destination::Boss, ITEM_ELEM_CORE, false, 24, 2, -16};
    delta.guardians.push_back({3, 120, 320, 4.0f, 5.0f, 6.0f, 0.25f, 2});
    bytes = encodePlayDelta(delta);
    PlayDeltaNet output;
    check(decodePlayDelta(bytes.data(), bytes.data()+bytes.size(), output) &&
          output.body.x == 4 && output.body.vitals.limb[vitals::HandL].health == .125f &&
          output.body.vitals.thirst == .5f && output.body.flags == 1 && output.serverTick == 88,
          "authority body roundtrip");
    check(output.body.fatigue.emptied[vitals::FootL] && output.body.fatigue.recoverDelay[vitals::FootL] == 2.f,
          "prediction fatigue state roundtrip");
    check(output.inventoryRevision == 7 && output.inventoryLayoutAck == 5 &&
          output.inventory[4].block == HAND_AXE && output.drops.size() == 1 &&
          output.drops[0].id == 22 && output.drops[0].vx == .25f && output.drops[0].axz == 1 &&
          output.drops[0].avy == .5f && output.drops[0].age == 2 &&
          output.combat.size() == 1 && output.combat[0].limb == vitals::HandL &&
          output.players.size() == 1 &&
          output.players[0].status == (kStatusBurning | kStatusFrozen | kStatusHealing) &&
          output.projectiles.size() == 1 && output.projectiles[0].id == 31 &&
          output.projectiles[0].kind == ArcaneProjectileKind::Freeze && output.projectiles[0].vz == 7 &&
          output.arcane.size() == 1 && output.arcane[0].serial == 17 &&
          output.arcane[0].kind == ArcaneEventKind::HealPulse && output.arcane[0].z == 10 &&
          output.clue.active && output.clue.stage == 3 &&
          output.clue.destination == (uint8_t)clue::Destination::Boss && output.clue.x == 24 &&
          output.guardians.size() == 1 && output.guardians[0].relic == 3 &&
          output.guardians[0].hp == 120 && output.guardians[0].maxHp == 320 &&
          output.guardians[0].x == 4.0f && output.guardians[0].z == 6.0f &&
          output.guardians[0].yaw == 0.25f && output.guardians[0].swing == 2,
          "inventory combat status arcane guardian and clue snapshot roundtrip");
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
    delta.players.clear(); delta.projectiles.clear(); delta.arcane.clear(); delta.combat.clear(); delta.drops.clear();
    delta.clue = {};
    delta.guardians.clear();
    bytes = encodePlayDelta(delta);
    bytes[bytes.size()-4] = 65;
    check(!decodePlayDelta(bytes.data(), bytes.data()+bytes.size(), output), "oversize projectile vector rejected");
    bytes = encodePlayDelta(delta);
    bytes[bytes.size()-3] = 33;
    check(!decodePlayDelta(bytes.data(), bytes.data()+bytes.size(), output), "oversize arcane event vector rejected");
    bytes = encodePlayDelta(delta);
    bytes[bytes.size()-2] = 17;
    check(!decodePlayDelta(bytes.data(), bytes.data()+bytes.size(), output), "oversize guardian vector rejected");
    delta.clue = {true, 1, 4, AIR, false, 1, 2, 3};
    bytes = encodePlayDelta(delta);
    check(!decodePlayDelta(bytes.data(), bytes.data()+bytes.size(), output), "unknown clue destination rejected");
    delta.clue = {true, 1, (uint8_t)clue::Destination::Clue, AIR, false, nan, 2, 3};
    bytes = encodePlayDelta(delta);
    check(!decodePlayDelta(bytes.data(), bytes.data()+bytes.size(), output), "NaN clue target rejected");
    delta.clue = {};
    delta.projectiles.push_back({1,1,ArcaneProjectileKind::Fireball,nan,0,0,0,0,0});
    bytes = encodePlayDelta(delta);
    check(!decodePlayDelta(bytes.data(), bytes.data()+bytes.size(), output), "NaN projectile rejected");
    delta.projectiles.clear();
    delta.projectiles.push_back({1,1,(ArcaneProjectileKind)3,0,0,0,0,0,0});
    bytes = encodePlayDelta(delta);
    check(!decodePlayDelta(bytes.data(), bytes.data()+bytes.size(), output), "unknown projectile kind rejected");
    delta.projectiles.clear();
    delta.arcane.push_back({1,(ArcaneEventKind)4,0,0,0});
    bytes = encodePlayDelta(delta);
    check(!decodePlayDelta(bytes.data(), bytes.data()+bytes.size(), output), "unknown arcane event kind rejected");
    delta.arcane.clear();
    burningPose.status = 8; delta.players.push_back(burningPose);
    bytes = encodePlayDelta(delta);
    check(!decodePlayDelta(bytes.data(), bytes.data()+bytes.size(), output), "unknown status bits rejected");
    delta.players.clear();
    delta.guardians.push_back({1, 10, 20, nan, 0, 0, 0, 0});
    bytes = encodePlayDelta(delta);
    check(!decodePlayDelta(bytes.data(), bytes.data()+bytes.size(), output), "NaN guardian pose rejected");
    delta.guardians.clear();

    // A deterministic flat test arena; never spawned in the actual game world.
    plugin::init();
    ritual::roll(7);
    check(!ritual::relicSpawned(0) && !ritual::relicSpawned(ritual::RelicCount - 1),
          "legacy map stamping no longer gives away Boss relics");
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
    building_loot::Spawner lootSpawner(world, 17);
    check(world.drops().empty(), "creating building spawner never scatters items");
    check(guide::kPageCount == 8 && guide::pageLineCount(0) > 0 && guide::pageLineCount(7) > 0,
          "guide book exposes all eight static pages");
    check(lootSpawner.spawnItemAt({8,1,8}, {HAND_PICK, 1}), "building point accepts registered tool");
    check(world.drops().size() == 1 && world.drops()[0].item == HAND_PICK,
          "explicit building spawn uses existing world drops");
    check(!lootSpawner.spawnItemAt({8,1,8}, {HAND_AXE, 2}), "tools cannot stack");
    check(lootSpawner.spawnItemAt({8,1,8}, {ITEM_ARCANE_FREEZE, 2}) &&
          lootSpawner.spawnItemAt({8,1,8}, {ITEM_ARCANE_HEAL, 2}),
          "building point accepts both new stackable arcane items");
    check(structure::guardianArcaneHurt(ITEM_ARCANE_FIREBALL, 0, 0.0f) == 38 &&
          structure::guardianArcaneHurt(ITEM_ARCANE_FIREBALL, 0, 3.0f) == 15,
          "fireball converts radial percentage damage to guardian HP");
    check(structure::guardianArcaneHurt(ITEM_ARCANE_FREEZE, 0) == 26 &&
          structure::guardianArcaneHurt(AIR, 0) == 0,
          "freeze direct hit damages guardians while invalid spells do not");
    check(!lootSpawner.spawnItemAt({8,1,8}, {AIR, 1}), "air is not loot");
    check(!lootSpawner.spawnItemAt({nan,1,8}, {HAND_PICK, 1}), "invalid building point rejected");
    check(!lootSpawner.spawnItemAt({8,.1f,8}, {HAND_PICK, 1}), "loot cannot spawn inside solid floor");
    check(!lootSpawner.spawnItemAt({1000,1,1000}, {HAND_PICK, 1}), "unloaded building point rejected");
    const building_loot::Entry tools[] = {{HAND_PICK,1,1,3}, {HAND_AXE,1,1,1}};
    for (int roll = 0; roll < 20; ++roll) {
        check(lootSpawner.spawnRandomLootAt({8,1,8}, tools), "weighted building loot spawn");
        const auto& drop = world.drops().back();
        check((drop.item == HAND_PICK || drop.item == HAND_AXE) && drop.count == 1,
              "random spawn stays within building table");
        check(drop.pos.x == 8 && drop.pos.y == 1 && drop.pos.z == 8, "randomness changes item, not position");
    }
    const size_t beforeInvalid = world.drops().size();
    const building_loot::Entry bad[] = {{HAND_PICK,1,1,1}, {HAND_AXE,1,2,1}};
    check(!lootSpawner.spawnRandomLootAt({8,1,8}, bad), "malformed table rejected atomically");
    check(!lootSpawner.spawnRandomLootAt({8,1,8}, {}), "empty table rejected");
    const building_loot::Entry unboundClue[] = {{ITEM_CLUE,1,1,1}};
    check(!lootSpawner.spawnItemAt({8,1,8}, {ITEM_CLUE,1}) &&
          !lootSpawner.spawnRandomLootAt({8,1,8}, unboundClue),
          "generic building loot cannot create an unbound clue");
    std::vector<building_loot::Entry> tooMany(65, {HAND_PICK,1,1,1});
    check(!lootSpawner.spawnRandomLootAt({8,1,8}, tooMany), "loot table size bounded");
    check(world.drops().size() == beforeInvalid, "invalid tables create no partial loot");

    clue::Director clues;
    clue::Link stageTwo{1, 2, {9,1,9}, clue::Destination::Boss, ITEM_ELEM_CORE};
    uint32_t stageTwoDrop = lootSpawner.spawnClueAt({8,1,8}, clues, stageTwo);
    check(stageTwoDrop && !clues.canPickup(stageTwoDrop, 1), "later clue stage cannot skip the route");
    clue::Link stageThree{1, 3, {14,1,14}, clue::Destination::Clue, AIR};
    uint32_t stageThreeDrop = lootSpawner.spawnClueAt({8,1,8}, clues, stageThree);
    clue::Link stageOne{1, 1, {12,1,12}, clue::Destination::Arcane, ITEM_ARCANE_FIREBALL};
    uint32_t stageOneDrop = lootSpawner.spawnClueAt({8,1,8}, clues, stageOne);
    check(stageOneDrop && clues.knowsDrop(stageOneDrop) && clues.canPickup(stageOneDrop, 1) &&
          !clues.canPickup(stageOneDrop, 2), "bound clue is restricted to its team and expected stage");
    room_inventory::State clueInventory;
    check(room_inventory::pickup(clueInventory, 1, world, stageOneDrop, {8,1.5f,8}, {8,.5f,8}) &&
          clues.claimPickup(stageOneDrop, 1), "server pickup reveals a bound clue target");
    clue::Target target = clues.targetFor(1);
    check(target.active && target.stage == 1 && target.destination == clue::Destination::Arcane &&
          target.position.x == 12 && target.rewardItem == ITEM_ARCANE_FIREBALL,
          "team receives the authoritative Arcane building coordinate");
    check(clues.canPickup(stageTwoDrop, 1) &&
          room_inventory::pickup(clueInventory, 2, world, stageTwoDrop, {8,1.5f,8}, {8,.5f,8}) &&
          clues.claimPickup(stageTwoDrop, 1), "next bound clue advances the team route");
    target = clues.targetFor(1);
    check(target.stage == 2 && target.destination == clue::Destination::Boss &&
          target.rewardItem == ITEM_ELEM_CORE, "final clue carries a future Boss relic contract");
    check(stageThreeDrop && !clues.canPickup(stageThreeDrop, 1),
          "an undefeated Boss blocks the next clue stage");
    check(!clues.completeBoss(world, 1, 91, {100,1,100}), "Boss reward must be near its bound encounter");
    uint32_t relicDrop = clues.completeBoss(world, 1, 91, {9,1,9});
    check(relicDrop && world.dropById(relicDrop) && world.dropById(relicDrop)->item == ITEM_ELEM_CORE,
          "authoritative Boss completion spawns the configured relic");
    check(!clues.completeBoss(world, 1, 91, {9,1,9}) && clues.targetFor(1).bossRewardClaimed,
          "Boss relic completion is replay-safe and exactly once");
    check(clues.canPickup(stageThreeDrop, 1), "Boss reward completion unlocks the next clue chain");

    room_inventory::State guideInventory;
    check(room_inventory::add(guideInventory, ITEM_GUIDE_BOOK, 1) == 0 &&
          guideInventory.slots[0].block == ITEM_GUIDE_BOOK,
          "server can grant one guide book at match entry");
    room_inventory::State inventory;
    check(room_inventory::add(inventory, HAND_PICK, 1) == 0 && inventory.slots[0].block == HAND_PICK,
          "server inventory adds a tool");
    check(room_inventory::add(inventory, HAND_PICK, 2) == 0 &&
          inventory.slots[1].block == HAND_PICK && inventory.slots[2].block == HAND_PICK,
          "nonstacking tools occupy separate slots");
    auto rearranged = inventory.slots;
    std::swap(rearranged[0], rearranged[5]);
    uint32_t baseRevision = inventory.revision;
    check(room_inventory::acceptLayout(inventory, 1, baseRevision, rearranged),
          "server accepts count-preserving rearrangement");
    check(inventory.slots[5].block == HAND_PICK, "layout is applied");
    auto forged = inventory.slots;
    forged[4] = {HAND_AXE,1};
    check(!room_inventory::acceptLayout(inventory, 2, inventory.revision, forged),
          "layout cannot create an item");
    check(!room_inventory::acceptLayout(inventory, 2, inventory.revision, inventory.slots),
          "rejected layout sequence cannot replay");
    check(!room_inventory::acceptLayout(inventory, 3, baseRevision, inventory.slots),
          "stale inventory revision rejected");
    check(!room_inventory::validSlot({HAND_AXE,2}), "tool stack validation");
    vitals::Vitals inventoryBody;
    check(room_inventory::held(inventory, inventoryBody, 5) == HAND_PICK, "server resolves held item by slot");
    inventoryBody.limb[vitals::HandR].health = 0;
    check(room_inventory::held(inventory, inventoryBody, 5) == AIR, "destroyed hand seals server slot use");
    room_inventory::State arcaneInventory;
    check(room_inventory::add(arcaneInventory, ITEM_ARCANE_FIREBALL, 8) == 0 &&
          arcaneInventory.slots[0].count == 8, "fireball items stack to their configured cap");
    uint32_t arcaneRevision = arcaneInventory.revision;
    check(room_inventory::consume(arcaneInventory, 0, ITEM_ARCANE_FIREBALL) &&
          arcaneInventory.slots[0].count == 7 && arcaneInventory.revision == arcaneRevision + 1,
          "server consumes exactly one accepted fireball");
    check(!room_inventory::consume(arcaneInventory, 0, ITEM_PRIM_FIRE),
          "server cannot consume a different item as a fireball");
    check(room_inventory::add(arcaneInventory, ITEM_ARCANE_FREEZE, 8) == 0 &&
          room_inventory::add(arcaneInventory, ITEM_ARCANE_HEAL, 8) == 0,
          "freeze and heal items stack to their configured cap");
    int freezeSlot = -1, healSlot = -1;
    for (int i = 0; i < cfg::INVENTORY_SLOTS; ++i) {
        if (arcaneInventory.slots[i].block == ITEM_ARCANE_FREEZE) freezeSlot = i;
        if (arcaneInventory.slots[i].block == ITEM_ARCANE_HEAL) healSlot = i;
    }
    check(freezeSlot >= 0 && healSlot >= 0 &&
          room_inventory::consume(arcaneInventory, freezeSlot, ITEM_ARCANE_FREEZE) &&
          room_inventory::consume(arcaneInventory, healSlot, ITEM_ARCANE_HEAL),
          "server consumes one accepted freeze and heal item");

    room_inventory::State pickupInventory;
    uint32_t pickupId = world.spawnDrop({8,1,8}, HAND_AXE, 1, true);
    check(pickupId && room_inventory::pickup(pickupInventory, 1, world, pickupId,
                                             {8,1.5f,8}, {8,.5f,8}),
          "server validates and applies nearby pickup");
    check(pickupInventory.slots[0].block == HAND_AXE && !world.dropById(pickupId),
          "pickup transfers instead of duplicating");
    check(!room_inventory::pickup(pickupInventory, 1, world, pickupId,
                                  {8,1.5f,8}, {8,.5f,8}), "pickup sequence cannot replay");
    uint32_t farId = world.spawnDrop({15,1,15}, HAND_PICK, 1, true);
    check(!room_inventory::pickup(pickupInventory, 2, world, farId,
                                  {8,1.5f,8}, {8,.5f,8}) && world.dropById(farId),
          "server rejects distant pickup without deleting it");
    uint32_t wallId = world.spawnDrop({8,1,11}, HAND_PICK, 1, true);
    check(!room_inventory::pickup(pickupInventory, 3, world, wallId,
                                  {8,1.5f,9}, {8,.5f,9}), "solid wall blocks pickup");
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
    room_body::spawn(state, {8,.5001f,8});
    input = {}; input.movement = kMoveForward | kMoveJump | kMoveSprint;
    room_body::accept(state, input, 14);
    room_body::tick(state, world, 14, true, false, true);
    check(state.player.pos.x == 8 && state.player.pos.z == 8 &&
          state.player.vel.x == 0 && state.player.vel.z == 0 && !state.player.jumpedThisUpdate,
          "frozen server tick blocks movement sprint and jump");
    state.player.vitals.limb[vitals::FootL].health = 0;
    state.player.vitals.limb[vitals::FootR].health = 0;
    state.player.vel = {5,0,5};
    input.movement |= kMoveJump;
    room_body::accept(state, input, 15);
    room_body::tick(state, world, 15, true, false);
    check(state.player.pos.x == 8 && state.player.pos.z == 8 && state.player.vel.y <= 0,
          "both feet loss stops momentum and jump in real physics");
    state.player.vitals.limb[vitals::Head].health = 0;
    room_body::tick(state, world, 16, true, false);
    check(state.player.dead && state.player.vel.lengthSq() == 0, "server death stops simulation");
    std::cout << "room body: " << checks << " checks passed\n";
}
