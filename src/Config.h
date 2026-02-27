#pragma once

struct Config {
    int   version                = 1;

    bool  enabled                = true;
    bool  debug                  = false;
    bool  stats                  = true;
    int   threadCount            = 0;

    bool  autoAdjust             = true;
    int   targetTickMs           = 40;
    int   adjustStep             = 10;

    int   actorTickTimeoutMs     = 30000;
};
