# Combat regression tests

Run from PowerShell with a MinGW C++20 compiler:

```powershell
.\tests\run-combat.ps1 -Compiler C:\msys64\ucrt64\bin\g++.exe
```

Executables are written to a unique temporary directory, not over the game or editor binaries. The body test links production World/Player code and constructs a deterministic arena in memory, with saving disabled. No test loot or map generation is added to gameplay.

## Current Phase 1 checkpoint

- `combat`: localized damage using existing vitals, limb restrictions, healing arithmetic, weapon timing, replay protection, hit volumes and future Boss multipliers.
- `room_body`: official-room movement intent is stepped by Dedicated Server at 20 Hz using six existing 120 Hz physics steps. Client position/flying/grounded claims cannot move combat players. Input expires after 10 ticks.
- The server owns vitals/fatigue and death; the recipient receives a fixed-size body snapshot in PlayDelta. The client predicts movement and accepts corrections; it does not independently tick room health. Local play/editor use the existing default physics behavior.
- Protocol version is `2609281701`; rebuild all room participants and the server together.

Not yet complete: authoritative item acquisition/inventory operations, melee requests and hit feedback, remote limb/status rendering, latency-aware prediction reconciliation, and two-client interactive validation. Client held-item fields remain visual metadata and must **not** authorize combat. PvP damage is not enabled at this checkpoint. Later phases (spells, loot API, Guide Book) are unchanged.

Before enabling PvP, verify two clients walking/jumping/colliding, deployment/death/redeployment, blocked-hand item use, melee versus mining arbitration and high-latency correction. The automated checks do not replace that interactive validation.
