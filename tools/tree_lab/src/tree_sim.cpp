#include "tree_sim.hpp"

TreeSim::Frame TreeSim::snapshot() const {
    return { woods, leaves, tips };
}

void TreeSim::bakeHistory() {
    startTrunk();
    history.clear();
    history.push_back(snapshot());
    while (!finished) {
        bool more = step();
        history.push_back(snapshot());
        if (!more) break;
    }
}

void TreeSim::rebuildLeaves() {
    TreeGrow::rebuildLeaves();
    if (!history.empty())
        history.back().leaves = leaves;
}
