#pragma once
#include "world/tree_grow.hpp"

class TreeSim : public TreeGrow {
public:
    struct Frame {
        std::vector<IVec3> woods;
        std::vector<IVec3> leaves;
        std::vector<TreeShoot> tips;
    };
    std::vector<Frame> history;

    void bakeHistory();
    void rebuildLeaves();

private:
    Frame snapshot() const;
};
