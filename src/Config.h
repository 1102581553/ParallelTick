#pragma once

struct Config {
    // LeviLamina 1.9.5 要求: ll::config::IsConfig 检查此字段
    int   version                = 1;

    bool  enabled                = true;
    bool  debug                  = false;
    bool  stats                  = true;
    int   threadCount            = 0;

    // gridSizeBase=64 → 同相实体最小间距 128 格，远超任何实体交互范围
    float gridSizeBase           = 64.0f;

    int   maxEntitiesPerTask     = 256;
    int   minEntitiesPerTask     = 8;
    int   maxEntitiesPerTaskLimit= 1024;

    bool  autoAdjust             = true;
    int   targetTickMs           = 40;
    int   adjustStep             = 10;
};
