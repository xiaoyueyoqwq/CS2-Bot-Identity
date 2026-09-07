#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace botid {

constexpr int kMaxSlots = 64;
constexpr int kMaxBotIdentities = 64;
// Must match shm_pub.h kShmNameLen - 1 (31 bytes UTF-8 + NUL).
constexpr size_t kMaxPersonaNameBytes = 31;

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
    uint16_t reused = 0;  // diagnostic recycle count; SteamID64 is never rewritten
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

    // GameFrame_Post ticks to hold native bot markers after callvote
    // dispatch returns. Valve builds the voter pool on the first vote
    // Think, which is after DispatchConCommand returns.
    int voteTransactionHoldFrames = 3;
};

// BotInfo: loads bot identities from JSON config
class BotInfo {
public:
    BotInfo() = default;

    bool Load(const char* path);
    bool LoadFeatures(const char* path);
    bool LoadBots(const char* path);
    int Count() const { return static_cast<int>(m_Bots.size()); }
    const BotIdentity* GetByIndex(int idx) const;
    const BotIdentity* GetByName(const char* name) const;
    BotIdentity* GetFree();  // returns first unused entry, recycles if needed
    bool IsSlotActive(int slot);  // check if engine slot has a connected player
    BotIdentity* At(int idx);  // mutable access (for IdentityManager)

    const PluginFeatures& Features() const { return m_Features; }
    int DroppedOnLoad() const { return m_DroppedOnLoad; }
    int TruncatedOnLoad() const { return m_TruncatedOnLoad; }

private:
    std::vector<BotIdentity> m_Bots;
    PluginFeatures m_Features;
    int m_DroppedOnLoad = 0;
    int m_TruncatedOnLoad = 0;
};

// CSS ServerLanguage is RFC 4646 (e.g. "zh-Hans-CN"). Underscores are
// treated as hyphens; comparison is case-insensitive.
inline std::string NormalizeLanguageTag(std::string tag) {
    size_t start = 0;
    while (start < tag.size() &&
           (tag[start] == ' ' || tag[start] == '\t' || tag[start] == '\n' || tag[start] == '\r')) {
        ++start;
    }
    size_t end = tag.size();
    while (end > start &&
           (tag[end - 1] == ' ' || tag[end - 1] == '\t' || tag[end - 1] == '\n' || tag[end - 1] == '\r')) {
        --end;
    }
    tag = tag.substr(start, end - start);
    for (char& c : tag) {
        if (c == '_') c = '-';
        else if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return tag;
}

inline bool IsSimplifiedChinese(std::string tag) {
    tag = NormalizeLanguageTag(std::move(tag));
    if (tag.empty()) return false;

    const size_t dash = tag.find('-');
    const std::string primary = (dash == std::string::npos) ? tag : tag.substr(0, dash);
    if (primary != "zh") return false;

    size_t i = (dash == std::string::npos) ? tag.size() : dash + 1;
    while (i < tag.size()) {
        size_t next = tag.find('-', i);
        if (next == std::string::npos) next = tag.size();
        const std::string sub = tag.substr(i, next - i);
        if (sub == "tw" || sub == "hk" || sub == "mo" || sub == "hant") return false;
        if (next == tag.size()) break;
        i = next + 1;
    }
    return true;
}

bool ReadCssServerLanguage(const char* path, std::string& out);

struct BotListSelection {
    std::string cssLanguage;
    bool cssLanguageFound = false;
    std::string matched;       // "zh-CN" or "default"
    std::string relativeFile;  // "lang/zh-CN.json" or "bots.json"
    std::string absolutePath;
};

BotListSelection SelectBotList(const std::string& baseDir);

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
