#pragma once

struct Config {
    int   version = 1;
    bool  enabled = true;
    bool  debug   = false;
    bool  stats   = true;
    int   threadCount = 0;

    // 4-色网格：gridSize=128 → 同相最小间距 256 格
    float gridSizeBase         = 128.0f;
    int   maxEntitiesPerTask   = 512;

    int   actorTickTimeoutMs   = 30000;
};
