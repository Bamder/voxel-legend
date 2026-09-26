#include "data_pack.hpp"
#include "asset_pack.hpp"
#include "../plugin/plugin.hpp"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace data {
namespace fs = std::filesystem;

namespace {

std::vector<ObjectDef> g_objects;
std::vector<EntityDef> g_entities;
std::vector<PackInfo> g_packs;
bool g_ready = false;

const TagInfo kTags[] = {
    { "axe",         TAG_AXE,         L"Axe" },
    { "woodworking", TAG_WOODWORKING, L"Wood" },
    { "one_hand",    TAG_ONE_HAND,    L"1H" },
    { "pick",        TAG_PICK,        L"Pick" },
    { "shovel",      TAG_SHOVEL,      L"Shovel" },
    { "shears",      TAG_SHEARS,      L"Shears" },
};

std::string trim(std::string s) {
    while (!s.empty() && (unsigned char)s.front() <= ' ') s.erase(s.begin());
    while (!s.empty() && (unsigned char)s.back() <= ' ') s.pop_back();
    return s;
}

std::string lower(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
    }
    return s;
}

bool parseLine(const std::string& raw, std::string& key, std::string& val) {
    std::string s = raw;
    auto hash = s.find('#');
    if (hash != std::string::npos) s = s.substr(0, hash);
    s = trim(s);
    if (s.empty()) return false;
    size_t sp = s.find_first_of(" \t=");
    if (sp == std::string::npos) {
        key = lower(s);
        val.clear();
        return true;
    }
    key = lower(s.substr(0, sp));
    val = trim(s.substr(sp + 1));
    if (!val.empty() && val[0] == '=') val = trim(val.substr(1));
    return true;
}

bool ensureDir(const fs::path& p) {
    std::error_code ec;
    fs::create_directories(p, ec);
    return fs::is_directory(p);
}

std::string readAll(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool writeAll(const fs::path& p, const std::string& text) {
    if (!ensureDir(p.parent_path())) return false;
    std::ofstream out(p, std::ios::binary);
    if (!out) return false;
    out << text;
    return (bool)out;
}

void addDrop(ObjectDef& o, const std::string& item, int count) {
    if (item.empty() || o.nDrops >= 4) return;
    if (count < 1) count = 1;
    if (count > 255) count = 255;
    o.drops[o.nDrops].itemId = item;
    o.drops[o.nDrops].count = (uint8_t)count;
    o.nDrops++;
    o.harvest = true;
}

void applyKey(ObjectDef& o, const std::string& key, const std::string& val) {
    if (key == "id" && !val.empty()) o.id = val;
    else if (key == "display") o.display = val;
    else if (key == "kind") o.kind = parseKind(val);
    else if (key == "max_stack" || key == "maxstack") {
        int n = std::atoi(val.c_str());
        if (n < 1) n = 1;
        if (n > 255) n = 255;
        o.maxStack = (uint8_t)n;
    } else if (key == "hardness") o.hardness = (float)std::atof(val.c_str());
    else if (key == "durability") o.durability = (float)std::atof(val.c_str());
    else if (key == "efficiency") o.efficiency = (float)std::atof(val.c_str());
    else if (key == "charge_sec") o.chargeSec = (float)std::atof(val.c_str());
    else if (key == "cooldown_sec") o.cooldownSec = (float)std::atof(val.c_str());
    else if (key == "weight") o.weight = (float)std::atof(val.c_str());
    else if (key == "wrong_tool_resist") o.wrongResist = (float)std::atof(val.c_str());
    else if (key == "tags") o.tags = parseTags(val);
    else if (key == "wear") o.wear = wear::fromId(lower(val));
    else if (key == "harvest_need" || key == "harvestneed") {
        std::string v = lower(val);
        if (v.empty() || v == "none" || v == "any" || v == "hand") o.harvestNeed = 0;
        else o.harvestNeed = parseTags(val);
        o.harvest = true;
    } else if (key == "harvest_drop" || key == "harvestdrop") {
        std::istringstream ss(val);
        std::string item;
        int count = 1;
        ss >> item >> count;
        addDrop(o, item, count);
    } else if (key == "crack_folds" || key == "crackfolds") {
        o.crackFolds = loot::clampCrackFolds(std::atoi(val.c_str()));
    } else if (key == "crack_color" || key == "crackcolor") {
        int r = loot::kDefaultCrackR, g = loot::kDefaultCrackG, b = loot::kDefaultCrackB;
        std::istringstream ss(val);
        ss >> r >> g >> b;
        auto cl8 = [](int v) -> uint8_t {
            if (v < 0) v = 0;
            if (v > 255) v = 255;
            return (uint8_t)v;
        };
        o.crackR = cl8(r);
        o.crackG = cl8(g);
        o.crackB = cl8(b);
    }
}

void applyKey(EntityDef& e, const std::string& key, const std::string& val) {
    float v = (float)std::atof(val.c_str());
    if (key == "id" && !val.empty()) e.id = val;
    else if (key == "display") e.display = val;
    else if (key == "mine_stamina_per_sec") e.rates.mineStaminaPerSec = v;
    else if (key == "sprint_stamina_per_sec") e.rates.sprintStaminaPerSec = v;
    else if (key == "jump_stamina_burst") e.rates.jumpStaminaBurst = v;
    else if (key == "swim_stamina_per_sec") e.rates.swimStaminaPerSec = v;
    else if (key == "stam_regen_fast") e.rates.stamRegenFast = v;
    else if (key == "stam_regen_slow") e.rates.stamRegenSlow = v;
    else if (key == "empty_recover_delay") e.rates.emptyRecoverDelay = v;
    else if (key == "run_cardio_per_sec") e.rates.runCardioPerSec = v;
    else if (key == "jump_chain_window") e.rates.jumpChainWindow = v;
    else if (key == "jump_cardio_base") e.rates.jumpCardioBase = v;
    else if (key == "jump_cardio_growth") e.rates.jumpCardioGrowth = v;
    else if (key == "jump_cardio_cap") e.rates.jumpCardioCap = v;
    else if (key == "empty_speed_mul") e.rates.emptySpeedMul = v;
    else if (key == "empty_jump_mul") e.rates.emptyJumpMul = v;
    else if (key == "mine_charge_sec") e.rates.mineChargeSec = v;
    else if (key == "mine_cooldown_sec") e.rates.mineCooldownSec = v;
}

void applyKey(PackInfo& p, const std::string& key, const std::string& val) {
    if (key == "name" && !val.empty()) p.name = val;
    else if (key == "title") p.title = val;
    else if (key == "description") p.description = val;
    else if (key == "priority") p.priority = std::atoi(val.c_str());
}

template <typename T>
void parseText(const std::string& text, T& dst) {
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string key, val;
        if (!parseLine(line, key, val)) continue;
        applyKey(dst, key, val);
    }
}

ObjectDef* upsertObject(const ObjectDef& src) {
    if (src.id.empty()) return nullptr;
    for (ObjectDef& o : g_objects) {
        if (o.id == src.id) {
            o = src;
            return &o;
        }
    }
    g_objects.push_back(src);
    return &g_objects.back();
}

EntityDef* upsertEntity(const EntityDef& src) {
    if (src.id.empty()) return nullptr;
    for (EntityDef& e : g_entities) {
        if (e.id == src.id) {
            e = src;
            return &e;
        }
    }
    g_entities.push_back(src);
    return &g_entities.back();
}

std::string objectFileText(const ObjectDef& o) {
    std::ostringstream ss;
    ss << "# VOXEL LEGEND data pack entry\n";
    ss << "id " << o.id << "\n";
    if (!o.display.empty()) ss << "display " << o.display << "\n";
    ss << "kind " << kindId(o.kind) << "\n";
    ss << "max_stack " << (int)o.maxStack << "\n";
    ss << "hardness " << o.hardness << "\n";
    ss << "durability " << o.durability << "\n";
    ss << "efficiency " << o.efficiency << "\n";
    if (o.kind == loot::Kind::Tool) {
        ss << "charge_sec " << o.chargeSec << "\n";
        ss << "cooldown_sec " << o.cooldownSec << "\n";
    }
    ss << "weight " << o.weight << "\n";
    if (o.kind != loot::Kind::Tool)
        ss << "wrong_tool_resist " << o.wrongResist << "\n";
    ss << "tags " << formatTags(o.tags) << "\n";
    if (o.wear >= 0 && o.wear < wear::Count)
        ss << "wear " << wear::id(o.wear) << "\n";
    if (o.harvest) {
        ss << "harvest_need " << (o.harvestNeed ? formatTags(o.harvestNeed) : "none") << "\n";
        for (int i = 0; i < o.nDrops; i++) {
            if (o.drops[i].itemId.empty()) continue;
            ss << "harvest_drop " << o.drops[i].itemId << " " << (int)o.drops[i].count << "\n";
        }
    }
    if (o.kind == loot::Kind::Block) {
        ss << "crack_folds " << loot::clampCrackFolds(o.crackFolds) << "\n";
        ss << "crack_color " << (int)o.crackR << " " << (int)o.crackG << " " << (int)o.crackB << "\n";
    }
    return ss.str();
}

std::string entityFileText(const EntityDef& e) {
    const vitals::Rates& r = e.rates;
    std::ostringstream ss;
    ss << "# VOXEL LEGEND entity rates\n";
    ss << "id " << e.id << "\n";
    if (!e.display.empty()) ss << "display " << e.display << "\n";
    ss << "mine_stamina_per_sec " << r.mineStaminaPerSec << "\n";
    ss << "sprint_stamina_per_sec " << r.sprintStaminaPerSec << "\n";
    ss << "jump_stamina_burst " << r.jumpStaminaBurst << "\n";
    ss << "swim_stamina_per_sec " << r.swimStaminaPerSec << "\n";
    ss << "stam_regen_fast " << r.stamRegenFast << "\n";
    ss << "stam_regen_slow " << r.stamRegenSlow << "\n";
    ss << "empty_recover_delay " << r.emptyRecoverDelay << "\n";
    ss << "run_cardio_per_sec " << r.runCardioPerSec << "\n";
    ss << "jump_chain_window " << r.jumpChainWindow << "\n";
    ss << "jump_cardio_base " << r.jumpCardioBase << "\n";
    ss << "jump_cardio_growth " << r.jumpCardioGrowth << "\n";
    ss << "jump_cardio_cap " << r.jumpCardioCap << "\n";
    ss << "empty_speed_mul " << r.emptySpeedMul << "\n";
    ss << "empty_jump_mul " << r.emptyJumpMul << "\n";
    return ss.str();
}

std::string packFileText(const PackInfo& p) {
    std::ostringstream ss;
    ss << "# VOXEL LEGEND data pack\n";
    ss << "name " << p.name << "\n";
    ss << "title " << p.title << "\n";
    ss << "description " << p.description << "\n";
    ss << "priority " << p.priority << "\n";
    return ss.str();
}

fs::path objectPath(const ObjectDef& o) {
    return fs::path(pack::dataPackDir(o.pack.empty() ? kVanilla : o.pack))
        / folderFor(o) / (o.id + ".def");
}

fs::path entityPath(const EntityDef& e) {
    return fs::path(pack::dataPackDir(e.pack.empty() ? kVanilla : e.pack))
        / "entity" / (e.id + ".def");
}

ObjectDef snapshotObject(uint8_t id) {
    ObjectDef o;
    o.id = plugin::blockId(id);
    if (o.id.empty()) return o;
    o.display = blockOf(id).name ? blockOf(id).name : o.id;
    o.pack = kVanilla;
    o.kind = loot::itemDef(id).kind;
    o.maxStack = loot::maxStack(id);
    loot::BreakStats b = loot::blockBreak(id);
    loot::BreakStats t = loot::isTool(id) ? loot::toolBreak(id) : loot::BreakStats{};
    if (o.kind == loot::Kind::Tool) {
        o.hardness = t.hardness;
        o.durability = 0.0f;
        o.efficiency = t.efficiency;
        o.chargeSec = t.chargeSec;
        o.cooldownSec = t.cooldownSec;
    } else {
        o.hardness = b.hardness;
        o.durability = b.durability;
        o.efficiency = 0.0f;
    }
    o.weight = blockOf(id).weight;
    o.tags = itemTags(id);
    o.wear = loot::itemWear(id);
    loot::HarvestRule hr = loot::harvestRule(id);
    if (hr.used) {
        o.harvest = true;
        o.harvestNeed = hr.needTags;
        o.wrongResist = hr.wrongResist;
        o.nDrops = 0;
        for (int i = 0; i < hr.nDrops && o.nDrops < 4; i++) {
            const char* nid = plugin::blockId(hr.drops[i].item);
            if (!nid || !nid[0]) continue;
            o.drops[o.nDrops].itemId = nid;
            o.drops[o.nDrops].count = hr.drops[i].count;
            o.nDrops++;
        }
    }
    loot::CrackStyle ck = loot::blockCrack(id);
    o.crackFolds = ck.folds;
    o.crackR = ck.r;
    o.crackG = ck.g;
    o.crackB = ck.b;
    return o;
}

ObjectDef snapshotHand() {
    ObjectDef o;
    o.id = kHandId;
    o.display = "Bare Hand";
    o.pack = kVanilla;
    o.kind = loot::Kind::Tool;
    o.maxStack = 1;
    loot::BreakStats h = loot::handBreak();
    o.hardness = h.hardness;
    o.efficiency = h.efficiency;
    o.chargeSec = h.chargeSec;
    o.cooldownSec = h.cooldownSec;
    o.weight = 0.0f;
    return o;
}

ObjectDef snapshotSod() {
    ObjectDef o;
    o.id = kSodId;
    o.display = "Grass Sod";
    o.pack = kVanilla;
    o.kind = loot::Kind::Item;
    o.maxStack = 1;
    loot::BreakStats s = loot::sodBreak();
    o.hardness = s.hardness;
    o.durability = s.durability;
    o.harvest = true;
    o.harvestNeed = TAG_SHOVEL;
    o.wrongResist = loot::kDefaultWrongResist;
    o.weight = 0.04f;
    return o;
}

EntityDef snapshotPlayer() {
    EntityDef e;
    e.id = "player";
    e.display = "Player";
    e.pack = kVanilla;
    e.rates = vitals::Rates{};
    return e;
}

void seedVanillaFiles() {
    PackInfo vanilla;
    vanilla.name = kVanilla;
    vanilla.title = "Vanilla";
    vanilla.description = "Built-in VOXEL LEGEND gameplay data";
    vanilla.priority = 0;
    fs::path packDef = fs::path(pack::dataPackDir(kVanilla)) / "pack.def";
    if (!pack::exists(packDef.string())) writeAll(packDef, packFileText(vanilla));

    loot::resetBuiltin();
    vitals::resetRates();
    plugin::init();

    for (int i = 0; i < liveBlockCount(); i++) {
        ObjectDef o = snapshotObject((uint8_t)i);
        if (o.id.empty()) continue;
        fs::path p = objectPath(o);
        if (!pack::exists(p.string())) writeAll(p, objectFileText(o));
    }
    ObjectDef hand = snapshotHand();
    fs::path hp = objectPath(hand);
    if (!pack::exists(hp.string())) writeAll(hp, objectFileText(hand));
    ObjectDef sod = snapshotSod();
    fs::path sp = objectPath(sod);
    if (!pack::exists(sp.string())) writeAll(sp, objectFileText(sod));

    EntityDef player = snapshotPlayer();
    fs::path ep = entityPath(player);
    if (!pack::exists(ep.string())) writeAll(ep, entityFileText(player));
}

void loadObjectFile(const fs::path& file, const std::string& packName) {
    ObjectDef o;
    o.pack = packName;
    o.id = file.stem().string();
    parseText(readAll(file), o);
    if (o.id.empty()) o.id = file.stem().string();
    upsertObject(o);
}

void loadEntityFile(const fs::path& file, const std::string& packName) {
    EntityDef e;
    e.pack = packName;
    e.id = file.stem().string();
    parseText(readAll(file), e);
    if (e.id.empty()) e.id = file.stem().string();
    upsertEntity(e);
}

void loadPackDir(const fs::path& dir) {
    PackInfo info;
    info.name = dir.filename().string();
    info.title = info.name;
    fs::path packDef = dir / "pack.def";
    if (fs::exists(packDef)) parseText(readAll(packDef), info);
    if (info.name.empty()) info.name = dir.filename().string();

    auto loadFolder = [&](const char* folder, bool entity) {
        fs::path sub = dir / folder;
        std::error_code ec;
        if (!fs::is_directory(sub, ec)) return;
        for (const auto& ent : fs::directory_iterator(sub, ec)) {
            if (ec) break;
            if (!ent.is_regular_file()) continue;
            if (ent.path().extension() != ".def") continue;
            if (entity) loadEntityFile(ent.path(), info.name);
            else loadObjectFile(ent.path(), info.name);
        }
    };
    loadFolder("block", false);
    loadFolder("item", false);
    loadFolder("entity", true);

    g_packs.push_back(info);
}

void loadFromBuiltinMemory() {
    g_objects.clear();
    g_entities.clear();
    loot::resetBuiltin();
    vitals::resetRates();
    plugin::init();
    for (int i = 0; i < liveBlockCount(); i++) {
        ObjectDef o = snapshotObject((uint8_t)i);
        if (!o.id.empty()) g_objects.push_back(o);
    }
    g_objects.push_back(snapshotHand());
    g_objects.push_back(snapshotSod());
    g_entities.push_back(snapshotPlayer());
}

void loadAllPacks() {
    g_packs.clear();
    loadFromBuiltinMemory();

    fs::path root(pack::dataDir());
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return;

    std::vector<fs::path> dirs;
    for (const auto& ent : fs::directory_iterator(root, ec)) {
        if (ec) break;
        if (ent.is_directory()) dirs.push_back(ent.path());
    }
    std::sort(dirs.begin(), dirs.end(), [](const fs::path& a, const fs::path& b) {
        return a.filename().string() < b.filename().string();
    });

    struct Ranked {
        int priority;
        fs::path dir;
    };
    std::vector<Ranked> ranked;
    for (const fs::path& d : dirs) {
        PackInfo peek;
        peek.name = d.filename().string();
        fs::path pd = d / "pack.def";
        if (fs::exists(pd)) parseText(readAll(pd), peek);
        ranked.push_back({ peek.priority, d });
    }
    std::stable_sort(ranked.begin(), ranked.end(), [](const Ranked& a, const Ranked& b) {
        return a.priority < b.priority;
    });
    g_packs.clear();
    for (const Ranked& r : ranked) loadPackDir(r.dir);
}

void applyOne(const ObjectDef& o) {
    if (o.id == kHandId) {
        loot::setHand({ o.hardness, 0.0f, o.efficiency, o.chargeSec, o.cooldownSec });
        return;
    }
    if (o.id == kSodId) {
        loot::setSod({ o.hardness, o.durability, 0.0f }, o.harvestNeed, o.wrongResist);
        return;
    }
    int id = resolveId(o.id);
    if (id < 0) return;
    uint8_t b = (uint8_t)id;
    int8_t wearSlot = (o.wear >= 0 && o.wear < wear::Count) ? (int8_t)o.wear : (int8_t)-1;
    loot::setItemDef(b, { o.kind, o.maxStack, wearSlot });
    loot::setTags(b, o.tags);
    if (o.kind == loot::Kind::Tool)
        loot::setToolBreak(b, { o.hardness, 0.0f, o.efficiency, o.chargeSec, o.cooldownSec });
    else
        loot::setBlockBreak(b, { o.hardness, o.durability, 0.0f });
    if (o.kind != loot::Kind::Tool) {
        loot::CrackStyle ck;
        ck.folds = o.crackFolds;
        ck.r = o.crackR;
        ck.g = o.crackG;
        ck.b = o.crackB;
        loot::setBlockCrack(b, ck);
    }
    if (b < (uint8_t)liveBlockCount())
        g_blockInfo[b].weight = o.weight;
    if (o.harvest) {
        loot::HarvestRule hr;
        hr.used = true;
        hr.needTags = o.harvestNeed;
        hr.wrongResist = o.wrongResist;
        hr.nDrops = 0;
        for (int i = 0; i < o.nDrops && hr.nDrops < 4; i++) {
            int item = resolveId(o.drops[i].itemId);
            if (item < 0) continue;
            hr.drops[hr.nDrops] = { (uint8_t)item, o.drops[i].count };
            hr.nDrops++;
        }
        loot::setHarvest(b, hr);
    } else {
        loot::clearHarvest(b);
    }
}

} // namespace

void apply() {
    loot::resetBuiltin();
    vitals::resetRates();
    for (const ObjectDef& o : g_objects) applyOne(o);
    for (const EntityDef& e : g_entities) {
        if (e.id == "player") vitals::rates() = e.rates;
    }
}

void init() {
    if (g_ready) return;
    plugin::init();
    loot::resetBuiltin();
    seedVanillaFiles();
    loadAllPacks();
    apply();
    g_ready = true;
}

void reload() {
    g_ready = false;
    g_objects.clear();
    g_entities.clear();
    g_packs.clear();
    init();
}

bool saveObject(const ObjectDef& o) {
    if (o.id.empty()) return false;
    return writeAll(objectPath(o), objectFileText(o));
}

bool saveEntity(const EntityDef& e) {
    if (e.id.empty()) return false;
    return writeAll(entityPath(e), entityFileText(e));
}

bool saveAll() {
    bool ok = true;
    for (const ObjectDef& o : g_objects) ok = saveObject(o) && ok;
    for (const EntityDef& e : g_entities) ok = saveEntity(e) && ok;
    apply();
    return ok;
}

std::vector<ObjectDef>& objects() { return g_objects; }
std::vector<EntityDef>& entities() { return g_entities; }
const std::vector<PackInfo>& packs() { return g_packs; }

ObjectDef* findObject(const std::string& id) {
    for (ObjectDef& o : g_objects) if (o.id == id) return &o;
    return nullptr;
}

EntityDef* findEntity(const std::string& id) {
    for (EntityDef& e : g_entities) if (e.id == id) return &e;
    return nullptr;
}

int resolveId(const std::string& id) {
    if (id.empty() || id == kHandId || id == kSodId) return -1;
    plugin::init();
    for (int i = 0; i < liveBlockCount(); i++) {
        if (id == plugin::blockId((uint8_t)i)) return i;
    }
    return -1;
}

const char* folderFor(const ObjectDef& o) {
    if (o.id == kHandId || o.id == kSodId || o.kind == loot::Kind::Tool || o.kind == loot::Kind::Item)
        return "item";
    return "block";
}

uint32_t parseTags(const std::string& s) {
    uint32_t bits = 0;
    std::string cur;
    auto flush = [&]() {
        std::string t = lower(trim(cur));
        cur.clear();
        if (t.empty() || t == "none") return;
        for (const TagInfo& info : kTags) {
            if (t == info.id) { bits |= info.bit; return; }
        }
    };
    for (char c : s) {
        if (c == ',' || c == ' ' || c == ';' || c == '|') flush();
        else cur.push_back(c);
    }
    flush();
    return bits;
}

std::string formatTags(uint32_t bits) {
    std::string out;
    for (const TagInfo& info : kTags) {
        if ((bits & info.bit) != info.bit) continue;
        if (!out.empty()) out += ',';
        out += info.id;
    }
    return out;
}

const char* kindId(loot::Kind k) {
    if (k == loot::Kind::Tool) return "tool";
    if (k == loot::Kind::Item) return "item";
    return "block";
}

loot::Kind parseKind(const std::string& s) {
    std::string v = lower(trim(s));
    if (v == "tool") return loot::Kind::Tool;
    if (v == "item") return loot::Kind::Item;
    return loot::Kind::Block;
}

const TagInfo* tagList(int* count) {
    if (count) *count = (int)(sizeof(kTags) / sizeof(kTags[0]));
    return kTags;
}

} // namespace data
