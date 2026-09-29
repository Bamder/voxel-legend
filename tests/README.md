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

## Current Phase 2 checkpoint

- `combat`: localized damage using existing vitals, limb restrictions, healing arithmetic, weapon timing, replay protection, hit volumes and future Boss multipliers.
- `room_body`: official-room movement intent is stepped by Dedicated Server at 20 Hz using six existing 120 Hz physics steps. Client position/flying/grounded claims cannot move combat players. Input expires after 10 ticks.
- The server owns vitals/fatigue and death; the recipient receives a fixed-size body snapshot in PlayDelta. The client predicts movement and accepts corrections; it does not independently tick room health. Local play/editor use the existing default physics behavior.
- Protocol version is `2609290101`; rebuild all room participants and the server together.

Phase 1 now includes authoritative room drops, pickup, count-preserving inventory rearrangement, hand-slot sealing, pick/axe attack intents, server windup/cooldown, server LOS/limb hit testing, vitals damage/death, remote body tint/reaction and attacker hit markers. Client target hints never name a victim on the wire and client held-item IDs do not authorize combat. Friendly fire and self hits are rejected. Pick deals 14% to the hit limb; axe deals 10%. Their Boss multipliers remain data-only adapters because no Boss entity exists.

Phase 2 adds the stackable/consumable `ITEM_ARCANE_FIREBALL`, cast replay protection and cooldown, server-owned projectiles, world/player collision, radial whole-body vitals damage, per-source Burning refresh/ticks, bounded projectile/event decoding, status sync and shared flight/trail/explosion/burning visuals. Right click casts from the selected right hand, falling back to the selected left hand. The client still sends no target, hit, damage, status or death declaration.

Still requiring interactive validation: two-client latency/prediction feel, Fireball visual tuning and final building placement integration. Freeze, Heal and Guide Book remain later phases. Current room inventory covers the 33 ordinary inventory slots; carried blocks and worn equipment still use their pre-existing paths and are not combat authority.

Before enabling PvP, verify two clients walking/jumping/colliding, deployment/death/redeployment, blocked-hand item use, melee versus mining arbitration and high-latency correction. The automated checks do not replace that interactive validation.

## Building loot integration contract

Weapon and arcane loot comes from building-supplied points, not starting inventories or world-wide scatter. `src/world/building_loot.hpp` exposes `building_loot::Spawner`, with `spawnItemAt(position, ItemSlot)` and `spawnRandomLootAt(position, span<Entry>)`. Buildings provide world-unit positions and weighted tables; the match server owns the spawner/RNG and calls it once after placing each building. Chunk reloads must not trigger another spawn. Counts must fit the existing item stack limit; invalid points/tables return false without spawning anything. A table has at most 64 entries.

The point must be in a loaded column and a non-solid cell. Building code is responsible for placing it inside the finished building, above its floor. Random selection changes item/count, never the supplied position. Fireball now has the registered `ITEM_ARCANE_FIREBALL` ID and may be added to a building-owned table; final loot probabilities remain owned by building/game design. Guide Book retains its separately specified first-match-spawn rule.

The spawn API uses existing World drops; room drops now have server IDs and are synchronized, range/LOS checked and transferred into the server inventory without duplication. Invoke the spawner only on Dedicated Server, once per placed building. Building generation itself is not modified because that module is still being developed.
