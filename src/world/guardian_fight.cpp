#include "guardian_fight.hpp"
#include "../core/config.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace guardian_fight {
namespace {

struct Hit {
    uint32_t id = 0;
    float time = 0.0f;
    float amount = 0.0f;
};

constexpr int kRouteMax = 48;

struct Waypoint {
    float x = 0.0f;
    float z = 0.0f;
};

struct Mind {
    guardian_ai::Brain brain{};
    Vec3 feet{};
    Vec3 home{};
    float yaw = 0.0f;
    float combat = 0.0f;
    float clock = 0.0f;
    float iframe = 0.0f;
    uint8_t swing = 0;
    bool placed = false;
    bool live = false;
    Hit hits[48]{};
    int hitCount = 0;
    Waypoint way[kRouteMax]{};
    int wayCount = 0;
    int wayAt = 0;
    int wayGoalX = 0x7fffffff;
    int wayGoalZ = 0x7fffffff;
    float wayAge = 0.0f;
};

Mind g_mind[kSlots];

void forgetOld(Mind& mind) {
    int keep = 0;
    for (int i = 0; i < mind.hitCount; ++i) {
        if (mind.clock - mind.hits[i].time <= guardian_ai::kThreatWindow)
            mind.hits[keep++] = mind.hits[i];
    }
    mind.hitCount = keep;
}

void sample(const Mind& mind, uint32_t id, float& damage, float& burst) {
    damage = 0.0f;
    burst = 0.0f;
    for (int i = 0; i < mind.hitCount; ++i) {
        if (mind.hits[i].id != id) continue;
        damage += mind.hits[i].amount;
        if (mind.hits[i].amount > burst) burst = mind.hits[i].amount;
    }
}

int pressure(const Mind& mind) {
    uint32_t ids[8]{};
    int count = 0;
    for (int i = 0; i < mind.hitCount; ++i) {
        if (mind.hits[i].amount <= 0.0f || !mind.hits[i].id) continue;
        bool seen = false;
        for (int j = 0; j < count; ++j) if (ids[j] == mind.hits[i].id) seen = true;
        if (seen) continue;
        if (count < 8) ids[count++] = mind.hits[i].id;
    }
    return count;
}

bool pushBlow(Blow* blows, int maxBlows, int& count, const Blow& blow) {
    if (!blows || count >= maxBlows || blow.amount <= 0.0f || !blow.target) return false;
    blows[count++] = blow;
    return true;
}

void strike(Mind& mind, int relic, const guardian_ai::Order& order, const Rival* rivals, int rivalCount,
            Blow* blows, int maxBlows, int& count) {
    if (order.skill != guardian_ai::SkillSlot::Melee &&
        order.skill != guardian_ai::SkillSlot::SmallAoe) return;
    mind.swing = (uint8_t)(mind.swing + 1u);
    if (!mind.swing) mind.swing = 1;
    float scale = (1.0f + order.attackBonus) * (1.0f + order.fragile);
    bool aoe = order.skill == guardian_ai::SkillSlot::SmallAoe;
    float reach = aoe ? kAoeReach : kHitReach;
    float hurt = (aoe ? kAoeHurt : kMeleeHurt) * scale;
    if (!std::isfinite(hurt) || hurt <= 0.0f) return;
    for (int i = 0; i < rivalCount; ++i) {
        const Rival& rival = rivals[i];
        if (!rival.alive || !rival.active || !rival.id) continue;
        if (!aoe && rival.id != order.target) continue;
        if ((rival.feet - mind.feet).length() > reach) continue;
        Blow blow;
        blow.target = rival.id;
        blow.relic = relic;
        blow.amount = hurt;
        blow.wholeBody = aoe;
        blow.limb = aoe ? -1 : vitals::Chest;
        pushBlow(blows, maxBlows, count, blow);
    }
}

// Chase routes around columns the body cannot climb. The search is the block
// grid inside the leash: eight neighbours, no corner cuts, and the same
// two-block step the stepper uses. The route is kept between replans.
constexpr int kRouteRadius = 44;
constexpr int kRouteSpan = kRouteRadius * 2 + 1;
constexpr int kRouteExpand = 700;

struct RouteOpen {
    int f = 0;
    int g = 0;
    int at = 0;
};

int gRouteScore[kRouteSpan * kRouteSpan];
int gRouteParent[kRouteSpan * kRouteSpan];
float gRouteStand[kRouteSpan * kRouteSpan];
uint8_t gRouteStamp[kRouteSpan * kRouteSpan];
uint8_t gRouteGen = 0;
RouteOpen gRouteHeap[8192];

int routeOctile(int dx, int dz) {
    dx = std::abs(dx);
    dz = std::abs(dz);
    int smaller = dx < dz ? dx : dz;
    return 10 * (dx + dz) - 6 * smaller;
}

bool routeWorse(const RouteOpen& a, const RouteOpen& b) {
    if (a.f != b.f) return a.f > b.f;
    return a.g < b.g;
}

void routePush(int& count, RouteOpen node) {
    if (count >= (int)(sizeof(gRouteHeap) / sizeof(gRouteHeap[0]))) return;
    gRouteHeap[count] = node;
    int i = count++;
    while (i > 0) {
        int parent = (i - 1) / 2;
        if (!routeWorse(gRouteHeap[parent], gRouteHeap[i])) break;
        std::swap(gRouteHeap[parent], gRouteHeap[i]);
        i = parent;
    }
}

RouteOpen routePop(int& count) {
    RouteOpen top = gRouteHeap[0];
    gRouteHeap[0] = gRouteHeap[--count];
    int i = 0;
    for (;;) {
        int left = i * 2 + 1;
        int right = left + 1;
        int best = i;
        if (left < count && routeWorse(gRouteHeap[best], gRouteHeap[left])) best = left;
        if (right < count && routeWorse(gRouteHeap[best], gRouteHeap[right])) best = right;
        if (best == i) break;
        std::swap(gRouteHeap[i], gRouteHeap[best]);
        i = best;
    }
    return top;
}

bool routeStand(Ground ground, float x, float z, float fromY, float& outY) {
    const float tile = cfg::BLOCK_SCALE;
    auto fits = [&](float y) {
        if (ground.blocked && ground.blocked(ground.ctx, x, y, z)) return false;
        if (!ground.feetY) return true;
        float stand = ground.feetY(ground.ctx, x, y, z);
        return std::isfinite(stand) && std::fabs(stand - y) <= 1e-3f;
    };
    if (fits(fromY)) { outY = fromY; return true; }
    for (int n = 1; n <= 2; ++n) {
        if (!fits(fromY + n * tile)) continue;
        outY = fromY + n * tile;
        return true;
    }
    for (int n = 1; n <= 2; ++n) {
        if (!fits(fromY - n * tile)) continue;
        outY = fromY - n * tile;
        return true;
    }
    return false;
}

void planChase(Mind& mind, Vec3 goal, Ground ground) {
    mind.wayCount = 0;
    mind.wayAt = 0;
    const float scale = cfg::BLOCK_SCALE;
    if (!(scale > 0.0f) || !std::isfinite(goal.x) || !std::isfinite(goal.z)) return;
    int originX = (int)std::floor(mind.home.x / scale);
    int originZ = (int)std::floor(mind.home.z / scale);
    int x0 = originX - kRouteRadius;
    int z0 = originZ - kRouteRadius;
    auto inside = [&](int x, int z) {
        return x >= x0 && z >= z0 && x < x0 + kRouteSpan && z < z0 + kRouteSpan;
    };
    auto indexOf = [&](int x, int z) { return (z - z0) * kRouteSpan + (x - x0); };
    int sx = (int)std::floor(mind.feet.x / scale);
    int sz = (int)std::floor(mind.feet.z / scale);
    int gx = (int)std::floor(goal.x / scale);
    int gz = (int)std::floor(goal.z / scale);
    float gwx = (gx + 0.5f) * scale;
    float gwz = (gz + 0.5f) * scale;
    Vec3 pull{ gwx - mind.home.x, 0.0f, gwz - mind.home.z };
    float pullLen = pull.length();
    if (pullLen > guardian_ai::kLeash && pullLen > 1e-4f) {
        pull = pull * (guardian_ai::kLeash / pullLen);
        gx = (int)std::floor((mind.home.x + pull.x) / scale);
        gz = (int)std::floor((mind.home.z + pull.z) / scale);
    }
    if (!inside(sx, sz) || !inside(gx, gz)) return;

    if (++gRouteGen == 0) {
        std::memset(gRouteStamp, 0, sizeof(gRouteStamp));
        gRouteGen = 1;
    }
    const uint8_t stamp = gRouteGen;
    const float leashSq = guardian_ai::kLeash * guardian_ai::kLeash;
    auto known = [&](int index) { return gRouteStamp[index] == stamp; };
    auto touch = [&](int index) {
        if (gRouteStamp[index] == stamp) return;
        gRouteStamp[index] = stamp;
        gRouteScore[index] = 0x7fffffff;
        gRouteParent[index] = -1;
        gRouteStand[index] = mind.feet.y;
    };

    int start = indexOf(sx, sz);
    touch(start);
    gRouteScore[start] = 0;
    gRouteStand[start] = mind.feet.y;
    int heapCount = 0;
    routePush(heapCount, RouteOpen{ routeOctile(sx - gx, sz - gz), 0, start });

    const int stepX[8] = { 1, -1, 0, 0, 1, 1, -1, -1 };
    const int stepZ[8] = { 0, 0, 1, -1, 1, -1, 1, -1 };
    const int stepCost[8] = { 10, 10, 10, 10, 14, 14, 14, 14 };
    int best = start;
    int bestH = routeOctile(sx - gx, sz - gz);
    int expanded = 0;
    while (heapCount > 0 && expanded < kRouteExpand) {
        RouteOpen open = routePop(heapCount);
        if (!known(open.at) || open.g != gRouteScore[open.at]) continue;
        int cx = x0 + open.at % kRouteSpan;
        int cz = z0 + open.at / kRouteSpan;
        if (cx == gx && cz == gz) { best = open.at; break; }
        ++expanded;
        int hHere = routeOctile(cx - gx, cz - gz);
        if (hHere < bestH) { bestH = hHere; best = open.at; }
        for (int n = 0; n < 8; ++n) {
            int nx = cx + stepX[n];
            int nz = cz + stepZ[n];
            if (!inside(nx, nz)) continue;
            float wx = (nx + 0.5f) * scale;
            float wz = (nz + 0.5f) * scale;
            float hx = wx - mind.home.x;
            float hz = wz - mind.home.z;
            if (hx * hx + hz * hz > leashSq) continue;
            if (n >= 4) {
                float ignore = 0.0f;
                float ax = (cx + stepX[n] + 0.5f) * scale;
                float az = (cz + 0.5f) * scale;
                float bx = (cx + 0.5f) * scale;
                float bz = (cz + stepZ[n] + 0.5f) * scale;
                if (!routeStand(ground, ax, az, gRouteStand[open.at], ignore)) continue;
                if (!routeStand(ground, bx, bz, gRouteStand[open.at], ignore)) continue;
            }
            float stand = 0.0f;
            if (!routeStand(ground, wx, wz, gRouteStand[open.at], stand)) continue;
            int next = indexOf(nx, nz);
            touch(next);
            int g = open.g + stepCost[n];
            if (g >= gRouteScore[next]) continue;
            gRouteScore[next] = g;
            gRouteParent[next] = open.at;
            gRouteStand[next] = stand;
            routePush(heapCount, RouteOpen{ g + routeOctile(nx - gx, nz - gz), g, next });
        }
    }

    int chain[704];
    int chainCount = 0;
    bool reached = false;
    for (int at = best; at >= 0 && chainCount < 704; at = gRouteParent[at]) {
        chain[chainCount++] = at;
        if (at == start) { reached = true; break; }
    }
    // chain[0] is the chosen cell and chain[last] is the boss. Keep the first
    // steps; a later replan extends the rest.
    if (!reached) return;
    for (int i = chainCount - 2; i >= 0 && mind.wayCount < kRouteMax; --i) {
        int at = chain[i];
        int cx = x0 + at % kRouteSpan;
        int cz = z0 + at / kRouteSpan;
        mind.way[mind.wayCount++] = Waypoint{ (cx + 0.5f) * scale, (cz + 0.5f) * scale };
    }
}

Vec3 chaseDirection(Mind& mind, const Rival& target, float dt, Ground ground, Vec3 direct) {
    const float scale = cfg::BLOCK_SCALE;
    mind.wayAge += dt;
    int gx = (int)std::floor(target.feet.x / scale);
    int gz = (int)std::floor(target.feet.z / scale);
    bool consumed = mind.wayCount > 0 && mind.wayAt >= mind.wayCount;
    bool due = gx != mind.wayGoalX || gz != mind.wayGoalZ || mind.wayAge >= 0.25f || consumed;
    if (due) {
        planChase(mind, target.feet, ground);
        mind.wayGoalX = gx;
        mind.wayGoalZ = gz;
        mind.wayAge = 0.0f;
    }
    const float arrive = scale * 0.65f;
    while (mind.wayAt < mind.wayCount) {
        float dx = mind.way[mind.wayAt].x - mind.feet.x;
        float dz = mind.way[mind.wayAt].z - mind.feet.z;
        if (dx * dx + dz * dz > arrive * arrive) break;
        ++mind.wayAt;
    }
    if (mind.wayAt >= mind.wayCount) return direct;
    Vec3 to{ mind.way[mind.wayAt].x - mind.feet.x, 0.0f, mind.way[mind.wayAt].z - mind.feet.z };
    float len = to.length();
    return len > 1e-4f ? to / len : direct;
}

void moveMind(Mind& mind, const guardian_ai::Order& order, const Rival* target, float dt, Ground ground) {
    float bonus = 1.0f + order.speedBonus;
    float speed = cfg::WALK_SPEED * bonus;
    if (order.state == guardian_ai::State::Charge) speed = cfg::SPRINT_SPEED * 1.7f * bonus;
    else if (order.state == guardian_ai::State::Evade) speed = cfg::SPRINT_SPEED * 1.25f * bonus;

    Vec3 face = target ? (target->feet - mind.feet) : (mind.home - mind.feet);
    face.y = 0.0f;
    float dist = face.length();
    Vec3 dir = dist > 1e-4f ? face / dist : Vec3{};
    Vec3 side{ -dir.z, 0.0f, dir.x };
    if (order.strafe < 0.0f) side = side * -1.0f;
    Vec3 wish{};
    const bool chasing = target && order.move == guardian_ai::Move::Approach
        && dist > order.preferDistance + 0.15f;
    if (!chasing) {
        mind.wayCount = 0;
        mind.wayAt = 0;
        mind.wayAge = 0.25f;
    }
    if (chasing) {
        wish = chaseDirection(mind, *target, dt, ground, dir);
    } else if (target && order.move == guardian_ai::Move::Approach) {
        wish = side;
    } else if (target && order.move == guardian_ai::Move::Retreat) {
        wish = dir * -1.0f;
    } else if (target && order.move == guardian_ai::Move::Orbit) {
        Vec3 side{ -dir.z, 0.0f, dir.x };
        if (order.strafe < 0.0f) side = side * -1.0f;
        float radial = dist > 12.0f ? 0.65f : (dist < 8.0f ? -0.65f : 0.0f);
        if (order.state == guardian_ai::State::Wander) {
            // No standoff. Strafe, and let a fraction of the step point backward.
            radial = -0.22f;
        }
        wish = side + dir * radial;
        float len = wish.length();
        if (len > 1e-4f) wish = wish / len;
    } else {
        Vec3 home = mind.home - mind.feet;
        home.y = 0.0f;
        float back = home.length();
        if (back > 0.35f) wish = home / back;
        face = home;
    }
    if (face.lengthSq() > 1e-6f) mind.yaw = std::atan2(face.x, -face.z);

    // One or two blocks can be stepped. Taller columns stay walls.
    const float tile = cfg::BLOCK_SCALE;
    const int kClimb = 2;
    auto slide = [&](Vec3 step) {
        float x = mind.feet.x + step.x;
        float z = mind.feet.z + step.z;
        Vec3 from{ x - mind.home.x, 0.0f, z - mind.home.z };
        float reach = from.length();
        if (reach > guardian_ai::kLeash && reach > 1e-4f) {
            from = from * (guardian_ai::kLeash / reach);
            x = mind.home.x + from.x;
            z = mind.home.z + from.z;
        }
        auto fits = [&](float y) {
            if (ground.blocked && ground.blocked(ground.ctx, x, y, z)) return false;
            if (!ground.feetY) return true;
            float stand = ground.feetY(ground.ctx, x, y, z);
            return std::isfinite(stand) && std::fabs(stand - y) <= 1e-3f;
        };
        if (fits(mind.feet.y)) {
            mind.feet.x = x;
            mind.feet.z = z;
            return true;
        }
        for (int n = 1; n <= kClimb; ++n) {
            if (!fits(mind.feet.y + n * tile)) continue;
            mind.feet.x = x;
            mind.feet.y += n * tile;
            mind.feet.z = z;
            return true;
        }
        for (int n = 1; n <= kClimb; ++n) {
            if (!fits(mind.feet.y - n * tile)) continue;
            mind.feet.x = x;
            mind.feet.y -= n * tile;
            mind.feet.z = z;
            return true;
        }
        return false;
    };
    Vec3 step = wish * (speed * dt);
    if (step.lengthSq() > 0.0f && !slide(step)) {
        if (!slide({ step.x, 0.0f, 0.0f })) slide({ 0.0f, 0.0f, step.z });
    }
}

} // namespace

void reset() {
    for (Mind& mind : g_mind) mind = {};
}

void noteDamage(int relic, uint32_t attacker, int hp) {
    if (relic < 0 || relic >= kSlots || !attacker || hp <= 0) return;
    Mind& mind = g_mind[relic];
    if (mind.iframe > 0.0f) return;
    if (mind.hitCount >= (int)(sizeof(mind.hits) / sizeof(mind.hits[0]))) {
        std::memmove(mind.hits, mind.hits + 1, sizeof(Hit) * (mind.hitCount - 1));
        --mind.hitCount;
    }
    mind.hits[mind.hitCount++] = Hit{ attacker, mind.clock, (float)hp * kThreatPerHp };
}

bool vulnerable(int relic) {
    if (relic < 0 || relic >= kSlots) return true;
    return g_mind[relic].iframe <= 0.0f;
}

int tick(const Home* homes, int homeCount, const Rival* rivals, int rivalCount,
         float dt, Ground ground, Blow* blows, int maxBlows) {
    if (!std::isfinite(dt) || dt < 0.0f) dt = 0.0f;
    if (dt > 1.0f) dt = 1.0f;
    if (homeCount < 0) homeCount = 0;
    if (homeCount > kSlots) homeCount = kSlots;
    if (!rivals || rivalCount < 0) rivalCount = 0;
    if (rivalCount > guardian_ai::kMaxTargets) rivalCount = guardian_ai::kMaxTargets;
    int blowsOut = 0;
    bool seen[kSlots]{};
    for (int h = 0; h < homeCount; ++h) {
        const Home& home = homes[h];
        if (home.relic < 0 || home.relic >= kSlots) continue;
        seen[home.relic] = true;
        Mind& mind = g_mind[home.relic];
        mind.home = home.feet;
        if (!(home.maxHp > 0.0f) || home.hp <= 0.0f) {
            uint32_t rng = mind.brain.rng;
            mind = {};
            mind.brain.rng = rng ? rng : 1u;
            continue;
        }
        if (!mind.placed) {
            mind.feet = home.feet;
            mind.placed = true;
        }
        mind.live = true;
        mind.clock += dt;
        if (mind.iframe > 0.0f) mind.iframe = std::max(0.0f, mind.iframe - dt);
        forgetOld(mind);

        guardian_ai::Sense senses[guardian_ai::kMaxTargets];
        const Rival* nearRival[guardian_ai::kMaxTargets]{};
        int near = 0;
        for (int i = 0; i < rivalCount; ++i) {
            const Rival& rival = rivals[i];
            if (!rival.id || !rival.alive || !rival.active) continue;
            float distance = (rival.feet - mind.feet).length();
            if (!std::isfinite(distance) || distance > guardian_ai::kLeash) continue;
            guardian_ai::Sense& sense = senses[near];
            sense = {};
            sense.id = rival.id;
            sense.alive = true;
            sense.distance = distance;
            sample(mind, rival.id, sense.damage10s, sense.burst10s);
            sense.windup = std::isfinite(rival.windup) && rival.windup > 0.0f ? rival.windup : 0.0f;
            nearRival[near] = &rival;
            if (++near >= guardian_ai::kMaxTargets) break;
        }

        guardian_ai::Order order;
        const Rival* locked = nullptr;
        if (near > 0) {
            mind.combat += dt;
            guardian_ai::Body body;
            body.hp = home.maxHp > 0.0f ? home.hp / home.maxHp : 1.0f;
            body.combatSeconds = mind.combat;
            body.focused = pressure(mind) >= 2;
            order = guardian_ai::think(mind.brain, body, senses, near, dt);
            if (order.invulnerable > mind.iframe) mind.iframe = order.invulnerable;
            for (int i = 0; i < near; ++i)
                if (nearRival[i] && nearRival[i]->id == order.target) locked = nearRival[i];
            strike(mind, home.relic, order, rivals, rivalCount, blows, maxBlows, blowsOut);
        } else {
            uint32_t rng = mind.brain.rng ? mind.brain.rng : 1u;
            float iframe = mind.iframe;
            uint8_t swing = mind.swing;
            Vec3 feet = mind.feet;
            float clock = mind.clock;
            Hit kept[48];
            int keptCount = mind.hitCount;
            std::memcpy(kept, mind.hits, sizeof(Hit) * keptCount);
            mind.brain = {};
            mind.brain.rng = rng;
            mind.combat = 0.0f;
            mind.iframe = iframe;
            mind.swing = swing;
            mind.feet = feet;
            mind.clock = clock;
            mind.hitCount = keptCount;
            std::memcpy(mind.hits, kept, sizeof(Hit) * keptCount);
            mind.placed = true;
            mind.live = true;
            mind.home = home.feet;
        }
        moveMind(mind, order, locked, dt, ground);
    }
    for (int i = 0; i < kSlots; ++i) {
        if (!seen[i]) g_mind[i].live = false;
    }
    return blowsOut;
}

int poses(Pose* out, int max) {
    if (!out || max <= 0) return 0;
    int count = 0;
    for (int i = 0; i < kSlots && count < max; ++i) {
        const Mind& mind = g_mind[i];
        if (!mind.live || !mind.placed) continue;
        Pose& pose = out[count++];
        pose.relic = i;
        pose.feet = mind.feet;
        pose.yaw = mind.yaw;
        pose.swing = mind.swing;
    }
    return count;
}

} // namespace guardian_fight
