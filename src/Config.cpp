#include "Config.h"
#include <ll/api/Config.h>
#include <ll/api/io/Logger.h>
#include <string>

namespace parallel_tick {

static Config configInstance;
static const std::string CONFIG_PATH = "config/parallel_tick.json";

Config& getConfig() {
    return configInstance;
}

bool loadConfig() {
    auto& logger = ll::mod::NativeMod::current()->getLogger();
    try {
        if (!ll::config::loadConfig(configInstance, CONFIG_PATH)) {
            // 文件不存在则创建默认配置
            ll::config::saveConfig(configInstance, CONFIG_PATH);
            logger.info("Created default config at {}", CONFIG_PATH);
        } else {
            logger.info("Loaded config from {}", CONFIG_PATH);
        }
        // 版本检查（可选）
        if (configInstance.version != 2) {
            logger.warn("Config version mismatch, some settings may be reset.");
        }
        return true;
    } catch (const std::exception& e) {
        logger.error("Failed to load config: {}", e.what());
        return false;
    }
}

bool saveConfig() {
    return ll::config::saveConfig(configInstance, CONFIG_PATH);
}

} // namespace parallel_tick
