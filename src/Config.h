#pragma once
#include <ll/api/Config.h>

namespace parallel_tick {

struct Config {
    int   version   = 1;
    bool  enabled   = true;
    bool  debug     = false;
    int   batchSize = 64;
    float gridSize  = 16.0f;
};

} // namespace parallel_tick
