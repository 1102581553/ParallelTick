#pragma once
#include <ll/api/Config.h>

namespace parallel_tick {

struct Config {
    int   version   = 1;
    bool  enabled   = true;   // 模组总开关
    bool  debug     = false;  // 详细调试日志
    bool  stats     = false;  // 每5秒统计输出
    int   threadCount = 0;    // 线程数，0自动
    int   maxEntitiesPerTask = 256;   // 每个任务块最大实体数
    float gridSizeBase = 16.0f;       // 基础网格大小（方块）
};

} // namespace parallel_tick
