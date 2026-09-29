#include "plugin.hpp"
#include "modules/BasicConstruction/basic_construction.hpp"
#include <unordered_map>

BlockInfo g_blockInfo[256] {};
int g_blockCount = 0;

namespace plugin {
namespace {

BlockStrategy g_defaultBlock;
EntityStrategy g_defaultEntity;
std::string g_blockIds[256];
std::string g_blockNames[256];
BlockStrategy* g_blockStrat[256] {};
std::vector<uint8_t> g_palette;
std::vector<EntityModule> g_entities;
std::unordered_map<std::string, int> g_entityIndex;
bool g_inited = false;

} // namespace

void init() {
    if (g_inited) return;
    g_inited = true;
    BasicConstruction::registerModule();
}

int blockCount() {
    init();
    return liveBlockCount();
}

const char* blockId(uint8_t id) {
    init();
    if (id < (uint8_t)liveBlockCount() && !g_blockIds[id].empty())
        return g_blockIds[id].c_str();
    return "";
}

BlockStrategy* blockStrategy(uint8_t id) {
    init();
    if (id < (uint8_t)liveBlockCount() && g_blockStrat[id]) return g_blockStrat[id];
    return &g_defaultBlock;
}

const std::vector<uint8_t>& creativePalette() {
    init();
    return g_palette;
}

int registerBlock(BlockModule mod) {
    if (mod.id.empty()) return -1;
    init();
    for (int i = 0; i < g_blockCount; i++)
        if (g_blockIds[i] == mod.id) return -1;
    if (g_blockCount >= 256) return -1;
    int n = g_blockCount++;
    g_blockIds[n] = mod.id;
    g_blockNames[n] = (mod.info.name && mod.info.name[0]) ? mod.info.name : mod.id;
    g_blockInfo[n] = mod.info;
    g_blockInfo[n].name = g_blockNames[n].c_str();
    g_blockStrat[n] = mod.strategy ? mod.strategy : &g_defaultBlock;
    if (mod.creative) g_palette.push_back((uint8_t)n);
    return n;
}

int registerBlock(const char* id, const BlockInfo& info, BlockStrategy* strategy, bool creative) {
    if (!id || !id[0]) return -1;
    return registerBlock(BlockModule{ id, info, strategy, creative });
}

const EntityModule* findEntity(const char* id) {
    if (!id) return nullptr;
    init();
    auto it = g_entityIndex.find(id);
    if (it == g_entityIndex.end()) return nullptr;
    return &g_entities[it->second];
}

const std::vector<EntityModule>& entities() {
    init();
    return g_entities;
}

bool registerEntity(EntityModule mod) {
    if (mod.id.empty()) return false;
    init();
    if (g_entityIndex.count(mod.id)) return false;
    if (!mod.strategy) mod.strategy = &g_defaultEntity;
    if (mod.appearance.empty()) mod.appearance = mod.id;
    g_entityIndex[mod.id] = (int)g_entities.size();
    g_entities.push_back(std::move(mod));
    return true;
}

} // namespace plugin
