#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace botid {

constexpr int kMaxSlots = 64;
constexpr int kMaxBotIdentities = 32;

// SteamID base for synthetic bot identities
constexpr uint64_t kSteamIdBase = 0x7400000000000001ULL;

struct BotIdentity {
    int slot = -1;
    uint64_t steamId = 0;
    std::string name;
    int ping = 0;        // 0 = don't override
    std::string crosshair;  // empty = no override
    uint32_t scoreboardFlair = 0;  // 0 = no override
    bool applied = false;
    uint16_t reused = 0;  // increments each time this entry is recycled
};

// Global plugin features, loaded from the top-level "features" key in
// config.json. Defaults match a "light disguise" profile: a moderate
// ping range and a low flair probability.
struct PluginFeatures {
    bool enableFakePing = true;
    int fakePingMin = 20;
    int fakePingMax = 90;

    bool enableScoreboardFlair = true;
    double scoreboardFlairProbability = 0.3;
    uint32_t defaultScoreboardFlair = 0;  // 0 = use per-bot value if set

    bool enableCrosshair = true;
    int pingJitterPercent = 30;  // ±N% per bot per 30s tick

    bool resetShmOnStart = true;  // unlink shm on plugin load
};

// BotInfo: loads bot identities from JSON config
class BotInfo {
public:
    BotInfo() = default;

    bool Load(const char* path);
    int Count() const { return static_cast<int>(m_Bots.size()); }
    const BotIdentity* GetByIndex(int idx) const;
    const BotIdentity* GetByName(const char* name) const;
    BotIdentity* GetFree();  // returns first unused entry, recycles if needed
    bool IsSlotActive(int slot);  // check if engine slot has a connected player
    BotIdentity* At(int idx);  // mutable access (for IdentityManager)

    const PluginFeatures& Features() const { return m_Features; }

private:
    std::vector<BotIdentity> m_Bots;
    PluginFeatures m_Features;
};

// IdentityManager: tracks which slots are currently managed
class IdentityManager {
public:
    IdentityManager() { m_Slots.fill(-1); }

    bool IsManaged(int slot) const;
    void Mark(int slot, int botIndex);
    void Unmark(int slot);
    int Lookup(int slot) const;  // returns botIndex, or -1
    int ActiveCount() const { return m_ActiveCount; }

    BotIdentity* GetIdentity(int slot);
    BotIdentity* GetIdentityByBotIndex(int botIndex);

private:
    std::array<int, kMaxSlots> m_Slots{};
    std::array<int, kMaxBotIdentities> m_BotToSlot{};
    int m_ActiveCount = 0;
};

// Global singletons
BotInfo& BotInfos();
IdentityManager& IdentityMgr();

}  // namespace botid
