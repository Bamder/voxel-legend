#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// World snapshots live at GameSaves/<worldName>/<slot>/
//   default                          — the active save (enter-world target)
//   <backup_created_time>            — a manual or auto backup folder
namespace saves {

constexpr const char* kRoot = "GameSaves";
constexpr const char* kActive = "default";

std::filesystem::path utf8Path(const std::string& utf8);
std::string pathToUtf8(const std::filesystem::path& p);

std::string worldDir(const std::string& worldName);
std::string slotDir(const std::string& worldName, const std::string& slot);

bool isValidWorldName(const std::string& name);
std::string timestampNow();

std::vector<std::string> listWorlds();
std::vector<std::string> listBackups(const std::string& worldName); // newest first; excludes "default"

bool writeMeta(const std::string& dir, uint32_t seed);
uint32_t readSeed(const std::string& dir, uint32_t fallback);

bool createWorld(const std::string& name, uint32_t seed);
bool deleteWorld(const std::string& name);

// Copy GameSaves/<world>/default -> GameSaves/<world>/<timestamp>.
bool backupDefault(const std::string& worldName, std::string* outSlot = nullptr);

// Snapshot current default, then replace default with a copy of `backupSlot`.
bool restoreBackup(const std::string& worldName, const std::string& backupSlot);

// One-time: world_save/ -> GameSaves/world/default if no worlds exist yet.
void migrateLegacy(uint32_t fallbackSeed);

} // namespace saves
