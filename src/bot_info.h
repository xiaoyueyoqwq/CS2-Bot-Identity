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
};

// BotInfo: loads bot identities from JSON config
class BotInfo {
public:
    BotInfo() = default;

    bool Load(const char* path);
    int Count() const { return static_cast<int>(m_Bots.size()); }
    const BotIdentity* GetByIndex(int idx) const;
    const BotIdentity* GetByName(const char* name) const;
    BotIdentity* GetFree();  // returns first slot=-1 entry, or nullptr
    BotIdentity* At(int idx);  // mutable access (for IdentityManager)

private:
    std::vector<BotIdentity> m_Bots;
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
