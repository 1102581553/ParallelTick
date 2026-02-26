#pragma once
#include <ll/api/Config.h>

namespace parallel_tick {

struct Config {
    int  version           = 1;
    bool enabled           = true;
    bool debug             = false; // 新增：Debug 开关
    int  batchSize         = 64;   
    float gridSize         = 16.0f; 
    bool parallelItemsOnly = false; 
};

} // namespace parallel_tick
