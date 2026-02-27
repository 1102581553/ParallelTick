#pragma once

struct Config {
    int   version = 1;
    bool  enabled = true;
    bool  debug   = false;
    bool  stats   = true;
    int   threadCount = 0;

    // 用于 HookLevelTick 的 Actor::tick 4色网格分组
    float gridSizeBase       = 128.0f;
    int   maxEntitiesPerTask = 512;
    int   actorTickTimeoutMs = 30000;
};
