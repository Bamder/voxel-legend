# Combat regression tests

Run from PowerShell with a MinGW C++20 compiler:

```powershell
.\tests\run-combat.ps1 -Compiler C:\msys64\ucrt64\bin\g++.exe
```

Executables are written to a unique temporary directory, not over the game or editor binaries. The body test links production World/Player code and constructs a deterministic arena in memory, with saving disabled. No test loot or map generation is added to gameplay.

Build a standalone interactive client/server test executable without overwriting `voxel-legend.exe`:

```powershell
.\tests\build-combat-test.ps1
```

This produces `voxel-legend-combat-test.exe` in the repository. The script passes relative paths to MinGW so repositories under Chinese directory names link correctly.

## Current combat + guide/clue checkpoint

- `combat`: localized damage using existing vitals, limb restrictions, healing arithmetic, weapon timing, replay protection, hit volumes and future Boss multipliers.
- `room_body`: official-room movement intent is stepped by Dedicated Server at 20 Hz using six existing 120 Hz physics steps. Client position/flying/grounded claims cannot move combat players. Input expires after 10 ticks.
- The server owns vitals/fatigue and death; the recipient receives a fixed-size body snapshot in PlayDelta. The client predicts movement and accepts corrections; it does not independently tick room health. Local play/editor use the existing default physics behavior.
- Protocol version is `2609290103`; rebuild all room participants and the server together.

Phase 1 now includes authoritative room drops, pickup, count-preserving inventory rearrangement, hand-slot sealing, pick/axe attack intents, server windup/cooldown, server LOS/limb hit testing, vitals damage/death, remote body tint/reaction and attacker hit markers. Client target hints never name a victim on the wire and client held-item IDs do not authorize combat. Friendly fire and self hits are rejected. Pick deals 14% to the hit limb; axe deals 10%. Their Boss multipliers remain data-only adapters because no Boss entity exists.

Phase 2 adds the stackable/consumable `ITEM_ARCANE_FIREBALL`, cast replay protection and cooldown, server-owned projectiles, world/player collision, radial whole-body vitals damage, per-source Burning refresh/ticks, bounded projectile/event decoding, status sync and shared flight/trail/explosion/burning visuals. Right click casts from the selected right hand, falling back to the selected left hand. The client still sends no target, hit, damage, status or death declaration.

Phase 3 adds stackable/consumable Freeze and Heal items. Freeze uses a server-owned single-target projectile, applies 8% whole-body damage and one refreshable five-second Frozen state, and blocks movement/jump/sprint in authoritative physics while preserving view rotation. Heal lets the server ray-select an injured same-team player within eight world units and line of sight, falling back to the caster, then adds 25% to every existing vitals limb without resurrection or overheal. Their cooldowns, inventory consumption, status/event vectors and blue-white/green-gold visuals are all authoritative/synchronized.

Guide Book is now a server-granted, reusable room Item with eight static pages. Bound Clue items reveal a server-owned, team-shared building coordinate through PlayDelta. The clue director rejects wrong-team, out-of-order and pre-Boss progression, and exposes an exactly-once future Boss relic drop adapter. No Boss Entity or Boss AI was added.

Still requiring interactive validation: two-client latency/prediction feel, Arcane visual tuning and final building/Boss placement integration. Current room inventory covers the 33 ordinary inventory slots; carried blocks and worn equipment still use their pre-existing paths and are not combat authority. Free exploration intentionally does not activate the room combat Arcane path.

Before enabling PvP, verify two clients walking/jumping/colliding, deployment/death/redeployment, blocked-hand item use, melee versus mining arbitration and high-latency correction. The automated checks do not replace that interactive validation.

## Building loot integration contract

Weapon and arcane loot comes from building-supplied points, not starting inventories or world-wide scatter. `src/world/building_loot.hpp` exposes `building_loot::Spawner`, with `spawnItemAt(position, ItemSlot)` and `spawnRandomLootAt(position, span<Entry>)`. Buildings provide world-unit positions and weighted tables; the match server owns the spawner/RNG and calls it once after placing each building. Chunk reloads must not trigger another spawn. Counts must fit the existing item stack limit; invalid points/tables return false without spawning anything. A table has at most 64 entries.

The point must be in a loaded column and a non-solid cell. Building code is responsible for placing it inside the finished building, above its floor. Random selection changes item/count, never the supplied position. Fireball, Freeze and Heal have registered `ITEM_ARCANE_*` IDs and may be added to a building-owned table; final loot probabilities remain owned by building/game design. Guide Book is granted at room entry and never belongs in a loot table.

Clues must use `spawnClueAt(position, director, link)`, not generic/random spawning. The link binds a drop to one combat team, a strictly increasing stage (1..32), the next building's world position, a destination kind (Clue/Arcane/Boss), and an optional reward item. Boss links require a ritual relic item. One Boss completion unlocks the next stage and can produce its configured relic only once.

The spawn API uses existing World drops; room drops now have server IDs and are synchronized, range/LOS checked and transferred into the server inventory without duplication. Invoke the spawner only on Dedicated Server, once per placed building. Building generation itself is not modified because that module is still being developed.
