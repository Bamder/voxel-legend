#include "saves.hpp"
#include <algorithm>
#include <ctime>
#include <fstream>
#include <system_error>

namespace saves {
namespace fs = std::filesystem;

static bool isReservedSlot(const std::string& name) {
    return name == kActive || name == "." || name == "..";
}

fs::path utf8Path(const std::string& utf8) {
    return fs::path(std::u8string(utf8.begin(), utf8.end()));
}

std::string pathToUtf8(const fs::path& p) {
    auto u8 = p.u8string();
    return std::string(u8.begin(), u8.end());
}

static bool copyDir(const fs::path& src, const fs::path& dst) {
    std::error_code ec;
    if (!fs::exists(src, ec) || !fs::is_directory(src, ec)) return false;
    if (fs::exists(dst, ec)) {
        fs::create_directories(dst, ec);
        if (ec) return false;
        for (fs::directory_iterator it(src, ec); it != fs::directory_iterator() && !ec; ++it) {
            fs::copy(it->path(), dst / it->path().filename(),
                     fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
            if (ec) return false;
        }
        return true;
    }
    fs::copy(src, dst, fs::copy_options::recursive, ec);
    return !ec;
}

static bool dirHasFiles(const fs::path& dir) {
    std::error_code ec;
    if (!fs::exists(dir, ec) || !fs::is_directory(dir, ec)) return false;
    for (fs::directory_iterator it(dir, ec); it != fs::directory_iterator() && !ec; ++it)
        return true;
    return false;
}

std::string worldDir(const std::string& worldName) {
    return std::string(kRoot) + "/" + worldName;
}

std::string slotDir(const std::string& worldName, const std::string& slot) {
    return worldDir(worldName) + "/" + slot;
}

bool isValidWorldName(const std::string& name) {
    if (name.empty() || name.size() > 64) return false;
    if (name == "." || name == "..") return false;
    if (static_cast<unsigned char>(name.front()) <= 32) return false;
    if (static_cast<unsigned char>(name.back()) <= 32 || name.back() == '.') return false;
    for (unsigned char c : name) {
        if (c < 32) return false;
        if (c == '<' || c == '>' || c == ':' || c == '"' || c == '/' ||
            c == '\\' || c == '|' || c == '?' || c == '*')
            return false;
    }
    return true;
}

std::string timestampNow() {
    std::time_t t = std::time(nullptr);
    std::tm* tm = std::localtime(&t);
    char buf[32];
    if (!tm) return "backup";
    std::strftime(buf, sizeof(buf), "%Y-%m-%d_%H-%M-%S", tm);
    return std::string(buf);
}

std::vector<std::string> listWorlds() {
    std::vector<std::string> out;
    std::error_code ec;
    fs::path root = utf8Path(kRoot);
    if (!fs::exists(root, ec) || !fs::is_directory(root, ec)) return out;
    for (fs::directory_iterator it(root, ec); it != fs::directory_iterator() && !ec; ++it) {
        if (!it->is_directory(ec)) continue;
        std::string name = pathToUtf8(it->path().filename());
        if (name.empty() || name == "." || name == "..") continue;
        out.push_back(name);
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<std::string> listBackups(const std::string& worldName) {
    std::vector<std::string> out;
    std::error_code ec;
    fs::path root = utf8Path(worldDir(worldName));
    if (!fs::exists(root, ec) || !fs::is_directory(root, ec)) return out;
    for (fs::directory_iterator it(root, ec); it != fs::directory_iterator() && !ec; ++it) {
        if (!it->is_directory(ec)) continue;
        std::string name = pathToUtf8(it->path().filename());
        if (isReservedSlot(name)) continue;
        out.push_back(name);
    }
    std::sort(out.begin(), out.end());
    std::reverse(out.begin(), out.end());
    return out;
}

bool writeMeta(const std::string& dir, uint32_t seed) {
    std::error_code ec;
    fs::create_directories(utf8Path(dir), ec);
    fs::path p = utf8Path(dir) / "world.txt";
    std::ofstream f(p, std::ios::trunc);
    if (!f) return false;
    f << "seed " << seed << "\n";
    return (bool)f;
}

uint32_t readSeed(const std::string& dir, uint32_t fallback) {
    fs::path p = utf8Path(dir) / "world.txt";
    std::ifstream f(p);
    if (!f) return fallback;
    std::string key;
    uint32_t seed = fallback;
    if (f >> key >> seed && key == "seed") return seed;
    return fallback;
}

bool createWorld(const std::string& name, uint32_t seed) {
    if (!isValidWorldName(name)) return false;
    std::error_code ec;
    fs::path def = utf8Path(slotDir(name, kActive));
    if (fs::exists(utf8Path(worldDir(name)), ec)) return false;
    fs::create_directories(def, ec);
    if (ec) return false;
    return writeMeta(slotDir(name, kActive), seed);
}

bool deleteWorld(const std::string& name) {
    if (!isValidWorldName(name)) return false;
    std::error_code ec;
    fs::path dir = utf8Path(worldDir(name));
    if (!fs::exists(dir, ec) || !fs::is_directory(dir, ec)) return false;
    fs::remove_all(dir, ec);
    if (ec) return false;
    return !fs::exists(dir, ec);
}

bool backupDefault(const std::string& worldName, std::string* outSlot) {
    fs::path src = utf8Path(slotDir(worldName, kActive));
    std::error_code ec;
    if (!fs::exists(src, ec) || !fs::is_directory(src, ec)) return false;

    std::string stamp = timestampNow();
    fs::path dst = utf8Path(slotDir(worldName, stamp));
    int n = 2;
    while (fs::exists(dst, ec)) {
        stamp = timestampNow() + "_" + std::to_string(n++);
        dst = utf8Path(slotDir(worldName, stamp));
        if (n > 99) return false;
    }
    if (!copyDir(src, dst)) return false;
    if (outSlot) *outSlot = stamp;
    return true;
}

bool restoreBackup(const std::string& worldName, const std::string& backupSlot) {
    if (isReservedSlot(backupSlot) || backupSlot.find('/') != std::string::npos ||
        backupSlot.find('\\') != std::string::npos)
        return false;
    fs::path src = utf8Path(slotDir(worldName, backupSlot));
    fs::path dst = utf8Path(slotDir(worldName, kActive));
    std::error_code ec;
    if (!fs::exists(src, ec) || !fs::is_directory(src, ec)) return false;

    // Always snapshot the live default before replacing it.
    if (fs::exists(dst, ec) && fs::is_directory(dst, ec)) {
        if (!backupDefault(worldName, nullptr)) return false;
    }

    fs::path tmp = utf8Path(slotDir(worldName, "__restore_tmp"));
    fs::remove_all(tmp, ec);
    if (!copyDir(src, tmp)) {
        fs::remove_all(tmp, ec);
        return false;
    }
    fs::remove_all(dst, ec);
    fs::rename(tmp, dst, ec);
    if (ec) {
        // Fall back to a second copy if rename fails (e.g. cross-device).
        bool ok = copyDir(tmp, dst);
        fs::remove_all(tmp, ec);
        return ok;
    }
    return true;
}

static uint32_t seedFromLegacyChunks(uint32_t fallback) {
    std::error_code ec;
    fs::path dir = utf8Path("world_save");
    if (!fs::exists(dir, ec) || !fs::is_directory(dir, ec)) return fallback;
    for (fs::directory_iterator it(dir, ec); it != fs::directory_iterator() && !ec; ++it) {
        if (!it->is_regular_file(ec)) continue;
        auto ext = it->path().extension().string();
        if (ext != ".bin") continue;
        std::ifstream f(it->path(), std::ios::binary);
        char magic[4] = {};
        uint32_t seed = 0;
        f.read(magic, 4);
        f.read((char*)&seed, 4);
        if (f && magic[0] == 'V' && magic[1] == 'L' && magic[2] == 'V') return seed;
    }
    return fallback;
}

void migrateLegacy(uint32_t fallbackSeed) {
    std::error_code ec;
    if (!dirHasFiles(utf8Path("world_save"))) return;
    if (!listWorlds().empty()) return;
    uint32_t seed = seedFromLegacyChunks(fallbackSeed);
    fs::path dst = utf8Path(slotDir("world", kActive));
    fs::create_directories(dst, ec);
    copyDir(utf8Path("world_save"), dst);
    writeMeta(slotDir("world", kActive), seed);
}

} // namespace saves
