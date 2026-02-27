#pragma once

struct Config {
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
};
