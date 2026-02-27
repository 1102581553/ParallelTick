#pragma once
#include <ll/api/Config.h>

namespace parallel_tick {

struct Config {
    int   version   = 1;
    bool  enabled   = true;   // 模组总开关
    bool  debug     = false;  // 详细调试日志（每个实体tick、Hook进入等）
    bool  stats     = false;  // 每5秒输出性能统计（总实体数、分组计数）
    int   threadCount = 0;    // 线程池线程数，0表示自动（CPU核心数-1）
    int   batchSize = 64;     // 每批处理的实体数
    float gridSize  = 16.0f;  // 分组网格大小
};

} // namespace parallel_tick
