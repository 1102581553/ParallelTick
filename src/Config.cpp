#include "Config.h"
#include <ll/api/Config.h>
#include <ll/api/io/Logger.h>
#include <ll/api/mod/NativeMod.h>

namespace parallel_tick {

static Config configInstance;

Config& getConfig() {
    return configInstance;
}

bool loadConfig() {
    auto configPath = ll::mod::NativeMod::current()->getConfigDir() / "parallel_tick.json";
    return ll::config::loadConfig(configInstance, configPath);
}

bool saveConfig() {
    auto configPath = ll::mod::NativeMod::current()->getConfigDir() / "parallel_tick.json";
    return ll::config::saveConfig(configInstance, configPath);
}

} // namespace parallel_tick
