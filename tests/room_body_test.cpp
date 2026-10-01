#include "../src/net/room_body.hpp"
#include "../src/net/room_inventory.hpp"
#include "../src/plugin/plugin.hpp"
#include "../src/world/building_loot.hpp"
#include "../src/world/clue_quiz.hpp"
#include "../src/world/guide.hpp"
#include "../src/world/match_content.hpp"
#include "../src/world/ritual.hpp"
#include "../src/world/structure.hpp"
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <unordered_set>

namespace {
int checks = 0;
void check(bool value, const char* message) {
    ++checks;
    if (!value) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
}

int main() {
    check(matchmap::playableSpan(4, 64) == 2048, "full combat roster keeps the 2048 field");
    check(matchmap::playableSpan(2, 2) == 181, "two solo teams use the shrunk field");
    check(matchmap::playableSpan(1, 0) == matchmap::playableSpan(1, 2), "fewer than 2 players counts as 2");
    check(matchmap::playableSpan(6, 90) == 2048, "teams above 4 and players above 64 stay at the full field");
    check(matchmap::span() == matchmap::kFullSpan, "tests start from the full field");
    check(matchmap::zoneColumns() == 819, "full field deploy side is 40 percent");
    matchmap::setSpan(181);
    check(matchmap::zoneColumns() == 72, "shrunk field deploy side stays 40 percent");
    auto zonesOverlap = [](matchmap::Zone a, matchmap::Zone b) {
        return a.cx0 < b.cx0 + b.columns && b.cx0 < a.cx0 + a.columns &&
               a.cz0 < b.cz0 + b.columns && b.cz0 < a.cz0 + a.columns;
    };
    for (int i = 0; i < 4; ++i) {
        matchmap::Zone zone = matchmap::combatZone(i);
        check(zone.columns == 72, "each corner deploy square uses the 40 percent side");
        check(matchmap::columnPlayable(zone.cx0, zone.cz0) &&
              matchmap::columnPlayable(zone.cx0 + zone.columns - 1, zone.cz0 + zone.columns - 1),
              "corner deploy squares stay inside the playable field");
        for (int j = i + 1; j < 6; ++j)
            check(!zonesOverlap(zone, matchmap::combatZone(j)), "deploy squares do not overlap");
    }
    check(!zonesOverlap(matchmap::combatZone(4), matchmap::combatZone(5)),
          "the two center deploy squares do not overlap");
    matchmap::setSpan(matchmap::kFullSpan);

    std::vector<RoomTeamNet> teams(4);
    teams[0].spectator = true;
    std::vector<RoomPlayerNet> players(5);
    players[0].team = 0;
    players[1].team = 0;
    players[2].team = 1;
    players[3].team = 2;
    players[4].team = -1;
    int combatTeams = 0, combatPlayers = 0;
    countCombatRoster(teams, players, combatTeams, combatPlayers);
    check(combatTeams == 2 && combatPlayers == 2,
          "spectators, unassigned players, and empty combat teams stay out of the field roster");
    int buildingCounts[3] = {};
    int buildingTotal = match_content::resourceBuildingTotal(2, 2, 0);
    check(buildingTotal >= 15 && buildingTotal <= 64, "resource building total stays in range");
    int crowded = match_content::resourceBuildingTotal(4, 64, 0);
    check(crowded >= 40 && crowded <= 64, "a full roster raises the building minimum to 40");
    match_content::splitResourceBuildings(17, 1, buildingCounts);
    check(buildingCounts[0] + buildingCounts[1] + buildingCounts[2] == 17, "building types sum to the total");
    int lo = buildingCounts[0], hi = buildingCounts[0];
    for (int count : buildingCounts) {
        if (count < lo) lo = count;
        if (count > hi) hi = count;
    }
    check(hi - lo <= 1, "the three building types differ by at most one");

    matchmap::setMatchRoster(0x1, 1, 1);
    matchmap::setSpan(matchmap::kFullSpan);
    World rosterWorld(91);
    rosterWorld.setSaveEnabled(false);
    rosterWorld.setMatchBounds(true);
    int liveRitual = ritual::assignedRitual(1);
    int liveX = 0, liveZ = 0;
    check(structure::ritualAnchor(liveRitual, liveX, liveZ), "the entered team keeps its altar");
    {
        matchmap::Zone home = matchmap::combatZone(0);
        int colX = matchmap::blockToCol(liveX, cfg::CHUNK_X);
        int colZ = matchmap::blockToCol(liveZ, cfg::CHUNK_Z);
        check(colX >= home.cx0 && colX < home.cx0 + home.columns &&
              colZ >= home.cz0 && colZ < home.cz0 + home.columns,
              "that altar stays inside the entered team's deploy square");
    }
    for (int team = 2; team <= matchmap::kCombatTeams; ++team) {
        int absentX = 0, absentZ = 0;
        check(!structure::ritualAnchor(ritual::assignedRitual(team), absentX, absentZ),
              "a team that did not enter gets no altar");
    }
    int liveRecipe[3] = {};
    ritual::recipeRelics(liveRitual, liveRecipe);
    bool recipeRelic[ritual::RelicCount] = {};
    for (int piece = 0; piece < 3; ++piece) recipeRelic[liveRecipe[piece]] = true;
    int ruinCount = 0;
    for (int relic = 0; relic < ritual::RelicCount; ++relic) {
        int rx = 0, rz = 0;
        bool present = structure::relicAnchor(relic, rx, rz);
        if (recipeRelic[relic]) {
            check(present, "each relic the entered ritual needs has a ruin");
            ++ruinCount;
        } else {
            check(!present, "relics outside the entered rituals are not generated");
        }
    }
    check(ruinCount == 3, "one team places exactly its three relic ruins");
    matchmap::setMatchRoster(0x3F, 6, 0);

    check(clue_quiz::count() == 50, "clue question bank contains exactly 50 questions");
    std::unordered_set<std::string> subjects;
    std::unordered_set<std::string> prompts;
    for (size_t i = 0; i < clue_quiz::count(); ++i) {
        const auto& question = clue_quiz::at(i);
        check(question.correct < 4 && std::strlen(question.prompt) <= 96 &&
              std::strlen(question.subject) <= 96, "question fits bounded wire strings");
        std::unordered_set<std::string> options;
        for (const char* option : question.options) {
            check(option && *option && std::strlen(option) <= 96,
                  "every clue option fits bounded wire string");
            options.insert(option);
        }
        check(options.size() == 4, "clue question has four distinct answers");
        check(prompts.insert(question.prompt).second, "clue question prompt is unique");
        subjects.insert(question.subject);
    }
    check(subjects.size() == 8, "clue questions cover all eight requested subjects");
    ClueQuizNet quiz;
    quiz.status = 1; quiz.challengeId = 91; quiz.dropId = 42;
    quiz.subject = "拓扑学"; quiz.prompt = "测试题";
    quiz.options = {"甲", "乙", "丙", "丁"};
    auto quizBytes = encodeClueQuiz(quiz);
    ClueQuizNet quizDecoded;
    check(decodeClueQuiz(quizBytes.data(), quizBytes.data() + quizBytes.size(), quizDecoded) &&
          quizDecoded.challengeId == 91 && quizDecoded.options[3] == "丁",
          "four-choice clue challenge roundtrip");
    check(!decodeClueQuiz(quizBytes.data(), quizBytes.data() + quizBytes.size() - 1, quizDecoded),
          "truncated clue challenge rejected");
    ClueQuizNet denied;
    denied.status = 4; denied.dropId = 42;
    denied.prompt = "该线索不属于当前阵营或阶段";
    auto deniedBytes = encodeClueQuiz(denied);
    check(decodeClueQuiz(deniedBytes.data(), deniedBytes.data() + deniedBytes.size(), quizDecoded) &&
          quizDecoded.status == 4 && quizDecoded.prompt == denied.prompt,
          "server clue pickup rejection explains the reason to the client");
    auto answerBytes = encodeClueAnswer(91, 3);
    uint32_t challengeId = 0; uint8_t answerOption = 0;
    check(decodeClueAnswer(answerBytes.data(), answerBytes.data() + answerBytes.size(),
                           challengeId, answerOption) && challengeId == 91 && answerOption == 3,
          "clue answer roundtrip");
    answerBytes = encodeClueAnswer(91, 4);
    check(!decodeClueAnswer(answerBytes.data(), answerBytes.data() + answerBytes.size(),
                            challengeId, answerOption), "fifth clue option rejected");

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
    bool assignedRituals[ritual::kRitualCount]{};
    for (int team = 1; team <= matchmap::kCombatTeams; ++team) {
        int assigned = ritual::assignedRitual(team);
        check(assigned >= 0 && assigned < ritual::kRitualCount &&
              !assignedRituals[assigned], "all six teams have distinct ritual assignments");
        assignedRituals[assigned] = true;
    }
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
    // Place an actual wall at world z=10 (voxel z=20 at half-block scale).
    world.setBlock(16, 2, 20, STONE, true, false);
    world.setBlock(16, 3, 20, STONE, true, false);
    uint32_t wallId = world.spawnDrop({8,1,11}, HAND_PICK, 1, true);
    check(!room_inventory::pickup(pickupInventory, 3, world, wallId,
                                  {8,1.5f,9}, {8,.5f,9}), "solid wall blocks pickup");
    // A real player looks down at a floor drop from eye height. The supporting
    // floor is beyond the item and must not be mistaken for an obstruction.
    uint32_t floorId = world.spawnDrop({8,.75f,8}, ITEM_ARCANE_FIREBALL, 1, true);
    check(floorId && room_inventory::pickup(pickupInventory, 4, world, floorId,
                                            {8,2.12f,8}, {8,.5f,8}),
          "server accepts a visible floor drop from normal player eye height");
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

    // Exercise the same team content builder the Dedicated Server calls.
    World matchWorld(247);
    matchWorld.setSaveEnabled(false);
    matchWorld.setMatchBounds(true);
    building_loot::Spawner matchLoot(matchWorld, 247);
    clue::Director matchClues;
    check(!match_content::populateTeam(matchWorld, matchLoot, matchClues, 247, 0),
          "spectators receive no team content");
    check(match_content::populateTeam(matchWorld, matchLoot, matchClues, 247, 1),
          "one active team receives its buildings, altar, weapons and clue route");
    int altarX = 0, altarZ = 0;
    int assignedAltar = ritual::assignedRitual(1);
    int altarY = 0;
    check(structure::ritualAnchor(assignedAltar, altarX, altarZ) &&
          structure::isOfferingCell(matchWorld, assignedAltar, altarX,
                                    matchWorld.surfaceHeight(altarX, altarZ) + 1, altarZ),
          "painted ritual altar remains the authoritative offering site");
    {
        matchmap::Zone home = matchmap::combatZone(0);
        int altarColX = matchmap::blockToCol(altarX, cfg::CHUNK_X);
        int altarColZ = matchmap::blockToCol(altarZ, cfg::CHUNK_Z);
        check(altarColX >= home.cx0 && altarColX < home.cx0 + home.columns &&
              altarColZ >= home.cz0 && altarColZ < home.cz0 + home.columns,
              "the team's ritual altar is inside its deploy square");
        int homeX = home.cx0 * cfg::CHUNK_X + home.columns * cfg::CHUNK_X / 2;
        int homeZ = home.cz0 * cfg::CHUNK_Z + home.columns * cfg::CHUNK_Z / 2;
        int dx = altarX - homeX;
        int dz = altarZ - homeZ;
        check(dx * dx + dz * dz >= 160 * 160, "the altar stays clear of the spawn buildings");
    }
    altarY = matchWorld.surfaceHeight(altarX, altarZ);
    check(matchWorld.getBlock(altarX, altarY, altarZ) == BRICK &&
          matchWorld.getBlock(altarX, altarY + 1, altarZ) == AIR,
          "painted altar leaves its offering triangle accessible");
    int neededRelics[3];
    ritual::recipeRelics(assignedAltar, neededRelics);
    for (int i = 0; i < 3; ++i)
        matchWorld.setBlock(altarX + i - 1, altarY + 1, altarZ,
                            (uint8_t)ritual::blockId(neededRelics[i]), true, false);
    check(structure::offeringReady(matchWorld, assignedAltar),
          "three Boss relics complete the painted ritual site");
    int picks = 0, axes = 0, arcaneItems = 0, clueDrops = 0, relicDrops = 0;
    for (const auto& drop : matchWorld.drops()) {
        picks += drop.item == HAND_PICK;
        axes += drop.item == HAND_AXE;
        arcaneItems += drop.item == ITEM_ARCANE_FIREBALL ||
                       drop.item == ITEM_ARCANE_FREEZE || drop.item == ITEM_ARCANE_HEAL;
        clueDrops += drop.item == ITEM_CLUE;
        relicDrops += drop.item >= ITEM_ELEM_CORE && drop.item <= ITEM_EYELESS;
    }
    check(picks >= 1 && axes >= 1 && arcaneItems >= 3 &&
          picks + axes + arcaneItems <= 6 && clueDrops == 6 && !relicDrops,
          "weapon room has both tools and all spells plus at most one random bonus; no relics");
    for (const auto& drop : matchWorld.drops()) {
        if (drop.item != HAND_PICK) continue;
        room_inventory::State roomPickup;
        Vec3 feet{drop.pos.x, drop.pos.y - cfg::BLOCK_SCALE * .5f, drop.pos.z};
        Vec3 eye{feet.x, feet.y + cfg::EYE_HEIGHT, feet.z};
        int nearby = room_inventory::nearbyDrop(matchWorld, eye, feet);
        check(nearby >= 0 && matchWorld.drops()[(size_t)nearby].netId == drop.netId,
              "F can target the spawned weapon-room tool through decorative grass");
        check(room_inventory::pickup(roomPickup, 1, matchWorld, drop.netId, eye, feet) &&
              roomPickup.slots[0].block == HAND_PICK,
              "a spawned weapon-room tool can be picked up from player eye height");
        break;
    }
    check(!matchClues.targetFor(2).active && !matchClues.targetFor(6).active,
          "a one-team match does not create routes for absent teams");
    matchWorld.updateDrops(cfg::DROP_LIFETIME + 1.0f);
    int survivingClues = 0;
    for (const auto& drop : matchWorld.drops()) survivingClues += drop.item == ITEM_CLUE;
    check(survivingClues == 6, "progression clues survive ordinary drop expiry");
    for (const auto& drop : matchWorld.drops()) {
        if (drop.item != ITEM_CLUE || !matchClues.canPickup(drop.netId, 1)) continue;
        room_inventory::State cluePickup;
        Vec3 feet{drop.pos.x, drop.pos.y - cfg::BLOCK_SCALE * .5f, drop.pos.z};
        Vec3 eye{feet.x, feet.y + cfg::EYE_HEIGHT, feet.z};
        check(room_inventory::canPickup(cluePickup, matchWorld, drop.netId, eye, feet),
              "first spawned clue is reachable and fits in an empty inventory");
        int nearby = room_inventory::nearbyDrop(matchWorld, eye, feet);
        check(nearby >= 0 && matchWorld.drops()[(size_t)nearby].netId == drop.netId,
              "F can target the first spawned clue from player eye height");
        break;
    }
    room_inventory::State routeInventory;
    int bossSteps = 0;
    for (int stage = 1; stage <= 6; ++stage) {
        uint32_t next = 0;
        Vec3 cluePosition{};
        for (const auto& drop : matchWorld.drops()) {
            if (!matchClues.canPickup(drop.netId, 1)) continue;
            next = drop.netId;
            cluePosition = drop.pos;
            break;
        }
        Vec3 feet{cluePosition.x, cluePosition.y - cfg::BLOCK_SCALE * .5f, cluePosition.z};
        Vec3 eye{feet.x, feet.y + cfg::EYE_HEIGHT, feet.z};
        check(next && room_inventory::pickup(routeInventory, (uint32_t)stage,
                                             matchWorld, next, eye, feet) &&
              matchClues.claimPickup(next, 1),
              "each ordered clue can actually enter inventory from its spawned position");
        clue::Target target = matchClues.targetFor(1);
        check(target.stage == stage && target.active, "clue advances exactly one stage");
        check((target.position - cluePosition).lengthSq() > 1.0f,
              "each clue points away from its own pickup location");
        if (target.destination == clue::Destination::Boss) {
            ++bossSteps;
            int relic = (int)target.rewardItem - (int)ITEM_ELEM_CORE;
            Vec3 home{};
            check(structure::guardianHome(matchWorld, relic, home) &&
                  (home - target.position).lengthSq() < 1.0f,
                  "Boss clue points to a spawned guardian core");
            check(matchClues.completeBoss(matchWorld, 1, (uint32_t)relic + 1, home),
                  "server Boss completion awards its bound ritual relic");
        }
    }
    check(bossSteps == 3, "route supplies all three Boss relics needed for the ritual");
    check(match_content::populateTeam(matchWorld, matchLoot, matchClues, 247, 6),
          "late sixth team can receive its own independent content");
    std::cout << "room body: " << checks << " checks passed\n";
}
