#pragma once

#include <string>
#include <map>
#include <random>
#include <cstdint>
#include <fstream>
#include <sstream>

// Simple JSON-like config parser (minimal implementation)
// In production, use a proper JSON library like nlohmann/json

struct BotConfig {
    std::string name;
    uint64_t steamId = 0;
    std::string crosshairCode;
    uint32_t scoreboardFlair = 0;
    std::string avatarPath;
};

struct PluginConfig {
    std::map<std::string, BotConfig> bots;
    bool enableFakePing = true;
    int fakePingMin = 10;
    int fakePingMax = 60;
    bool enableScoreboardFlair = true;
    double scoreboardFlairProbability = 0.3;
};

class ConfigManager {
public:
    bool LoadConfig(const std::string& path) {
        // TODO: Implement proper JSON parsing
        // For now, use a simple format
        std::ifstream file(path);
        if (!file.is_open()) {
            return false;
        }

        // Simple parsing - in production, use a proper JSON library
        std::string line;
        while (std::getline(file, line)) {
            // Parse config lines
            // This is a placeholder - real implementation needed
        }

        return true;
    }

    const BotConfig* GetBotConfig(const std::string& botName) const {
        auto it = config_.bots.find(botName);
        if (it != config_.bots.end()) {
            return &it->second;
        }
        return nullptr;
    }

    const PluginConfig& GetConfig() const {
        return config_;
    }

private:
    PluginConfig config_;
};
