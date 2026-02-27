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
    int   actorTickTimeoutMs     = 30000;
    int   maxCrashCountPerActor  = 1;
    // 不再有 killCrashedActors — 崩溃实体只做永久跳过，不调用 remove()
};
