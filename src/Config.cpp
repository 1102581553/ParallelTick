#include "Config.h"

namespace parallel_tick {

static Config configInstance;

Config& getConfig() {
    return configInstance;
}

} // namespace parallel_tick
