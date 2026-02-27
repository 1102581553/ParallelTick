#pragma once
#include <cstddef>

namespace parallel_tick {

struct Config {
    int  version = 2;                // 配置文件版本
    bool enabled = true;             // 全局开关
    bool debug   = false;            // 调试输出

    // 线程池配置
    int  threadCount = 0;             // 0 = 自动（硬件并发-1）
    size_t maxEntitiesPerTask = 64;   // 每个任务最多处理多少实体
    int  actorTickTimeoutMs = 5000;   // 并行阶段超时（毫秒）

    // 崩溃黑名单清理
    int  cleanupIntervalTicks = 100;  // 每多少 tick 清理一次过期的黑名单
    int  maxExpiredAge = 600;         // 实体加入黑名单后最多保留多少 tick（约30秒）

    // 统计输出
    bool statsEnabled = true;         // 是否启用统计信息输出（每5秒）
};

// 全局配置访问
Config& getConfig();
bool loadConfig();   // 从文件加载
bool saveConfig();   // 保存配置（可选）

} // namespace parallel_tick
