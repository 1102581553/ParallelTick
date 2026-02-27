#pragma once

struct Config {
    int   version                = 1;

    bool  enabled                = true;
    bool  debug                  = false;
    bool  stats                  = true;
    int   threadCount            = 0;

    float gridSizeBase           = 64.0f;

    int   maxEntitiesPerTask     = 256;
    int   minEntitiesPerTask     = 8;
    int   maxEntitiesPerTaskLimit= 1024;

    bool  autoAdjust             = true;
    int   targetTickMs           = 40;
    int   adjustStep             = 10;

    // ── 崩溃保护 ──
    // 单个实体 tick 超时（毫秒），超时则标记崩溃
    int   actorTickTimeoutMs     = 5000;
    // 单个实体连续 SEH/异常次数超过此值则永久禁用
    int   maxCrashCountPerActor  = 1;
    // 是否尝试 kill 崩溃实体
    bool  killCrashedActors      = true;
};
