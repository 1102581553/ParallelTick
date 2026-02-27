#include "Config.h"
#include <ll/api/Config.h>
#include <ll/api/io/Logger.h>
#include <ll/api/mod/NativeMod.h>
#include <filesystem>

namespace parallel_tick {

static Config configInstance;

Config& getConfig() {
    return configInstance;
}

bool loadConfig() {
    auto& logger = ll::mod::NativeMod::current()->getLogger();
    auto configDir = ll::mod::NativeMod::current()->getConfigDir();
    auto configPath = configDir / "parallel_tick.json";

    try {
        if (!ll::config::loadConfig(configInstance, configPath)) {
            // 文件不存在，创建默认配置
            if (!std::filesystem::exists(configDir)) {
                std::filesystem::create_directories(configDir);
            }
            if (!ll::config::saveConfig(configInstance, configPath)) {
                logger.error("Failed to save default config to {}", configPath.string());
                return false;
            }
            logger.info("Created default config at {}", configPath.string());
        } else {
            logger.info("Loaded config from {}", configPath.string());
        }
        // 版本检查
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
    auto configPath = ll::mod::NativeMod::current()->getConfigDir() / "parallel_tick.json";
    return ll::config::saveConfig(configInstance, configPath);
}

} // namespace parallel_tick
