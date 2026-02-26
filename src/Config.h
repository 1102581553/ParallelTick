#pragma once
#include <ll/api/Config.h>

namespace parallel_tick {

struct Config {
    int  version           = 1;
    bool enabled           = true;
    int  batchSize         = 64;   // 每批次执行的实体数
    float gridSize         = 16.0f; // 网格大小（推荐16.0或32.0）
    bool parallelItemsOnly = false; // 是否仅并行化掉落物和经验球
};

} // namespace parallel_tick
