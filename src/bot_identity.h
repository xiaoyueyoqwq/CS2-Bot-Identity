#pragma once

#include "memory_ops.h"
#include "config.h"
#include <map>
#include <random>
#include <cstdint>

struct ManagedBot {
    int slot = -1;
    void* client = nullptr;
    uint64_t steamId = 0;
    std::string name;
    int fakePing = 0;
    uint32_t scoreboardFlair = 0;
    bool isApplied = false;
};

class BotIdentityManager {
public:
    BotIdentityManager() : rng_(std::random_device{}()) {}

    void SetConfigManager(ConfigManager* config) {
        config_ = config;
    }

    // Called when a bot connects
    void OnBotConnected(int slot, void* client, const char* botName) {
        if (!client || !botName) return;

        // Check if this is a fake player (bot)
        // Note: For native bots, we need to convert them to fake players first
        // This is the key difference from our Schema API approach

        // Get bot config
        const BotConfig* botConfig = config_->GetBotConfig(botName);

        // Create managed bot entry
        ManagedBot bot;
        bot.slot = slot;
        bot.client = client;

        // Assign SteamID
        if (botConfig && botConfig->steamId != 0) {
            bot.steamId = botConfig->steamId;
        } else {
            bot.steamId = GenerateSteamId(slot);
        }

        // Assign name
        if (botConfig && !botConfig->name.empty()) {
            bot.name = botConfig->name;
        } else {
            bot.name = botName;
        }

        // Assign fake ping
        const auto& cfg = config_->GetConfig();
        if (cfg.enableFakePing) {
            std::uniform_int_distribution<> dist(cfg.fakePingMin, cfg.fakePingMax);
            bot.fakePing = dist(rng_);
        }

        // Assign scoreboard flair (with probability)
        if (cfg.enableScoreboardFlair) {
            std::uniform_real_distribution<> probDist(0.0, 1.0);
            if (probDist(rng_) < cfg.scoreboardFlairProbability) {
                if (botConfig) {
                    bot.scoreboardFlair = botConfig->scoreboardFlair;
                }
            }
        }

        // Apply identity
        ApplyIdentity(bot);

        // Store managed bot
        managedBots_[slot] = bot;
    }

    // Called when a bot disconnects
    void OnBotDisconnected(int slot) {
        managedBots_.erase(slot);
    }

    // Reapply identity (for periodic updates)
    void ReapplyIdentity(int slot) {
        auto it = managedBots_.find(slot);
        if (it == managedBots_.end()) return;

        ApplyIdentity(it->second);
    }

private:
    void ApplyIdentity(ManagedBot& bot) {
        if (!bot.client) return;

        // Step 1: Convert to fake player if not already
        // This is the KEY step that makes our modifications work
        if (!memory_ops::IsFakePlayerSet(bot.client)) {
            memory_ops::SetFakePlayer(bot.client);
        }

        // Step 2: Write SteamID
        if (bot.steamId != 0) {
            memory_ops::WriteSteamId(bot.client, bot.steamId);
        }

        // Step 3: Set name (if implemented)
        // memory_ops::SetPlayerName(bot.client, bot.name.c_str());

        bot.isApplied = true;
    }

    uint64_t GenerateSteamId(int slot) {
        // Generate a unique SteamID for this bot
        // Base: 76561197960265729 (a common base for synthetic IDs)
        uint64_t base = 76561197960265729ULL;
        std::uniform_int_distribution<uint64_t> dist(0, 1000000);
        return base + slot + dist(rng_);
    }

    ConfigManager* config_ = nullptr;
    std::map<int, ManagedBot> managedBots_;
    std::mt19937_64 rng_;
};
