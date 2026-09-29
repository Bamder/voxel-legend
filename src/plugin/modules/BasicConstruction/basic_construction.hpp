#pragma once

// Built-in block + player entity pack. Strategies, type tables, creative
// palette, and registration live here. The host only calls registerModule().
// Appearance files stay under assets/; they do not register types.
// Numeric ids 0..BLOCK_COUNT-1 match enum Block (world gen / saves).
namespace plugin {
namespace BasicConstruction {

void registerModule();

} // namespace BasicConstruction
} // namespace plugin
