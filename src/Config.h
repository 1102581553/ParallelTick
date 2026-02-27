#pragma once
#include <ll/api/data/JsonSerializable.h>

struct Config : ll::data::JsonSerializable<Config> {
    bool  enabled                = true;
    bool  debug                  = false;
    bool  stats                  = true;
    int   threadCount            = 0;
    int   maxEntitiesPerTask     = 256;
    int   minEntitiesPerTask     = 8;
    int   maxEntitiesPerTaskLimit= 1024;
    float gridSizeBase           = 16.0f;
    bool  autoAdjust             = true;
    int   targetTickMs           = 40;
    int   adjustStep             = 10;
};
