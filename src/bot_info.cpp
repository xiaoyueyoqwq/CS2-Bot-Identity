#include "bot_info.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <eiface.h>
#include <fstream>
#include <functional>
#include <interfaces/interfaces.h>
#include <iserver.h>
#include <sstream>

class INetworkServerService;
extern INetworkServerService* g_pNetworkServerService;

namespace botid {

// ─── Tiny JSON reader (config only, no external deps) ─────────────────────

static bool SkipWhitespace(const std::string& s, size_t& i) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
    return i < s.size();
}

static bool MatchChar(const std::string& s, size_t& i, char c) {
    SkipWhitespace(s, i);
    if (i < s.size() && s[i] == c) { ++i; return true; }
    return false;
}

static std::string ReadString(const std::string& s, size_t& i) {
    if (!MatchChar(s, i, '"')) return {};
    size_t start = i;
    while (i < s.size() && s[i] != '"') ++i;
    std::string r = s.substr(start, i - start);
    if (i < s.size()) ++i;
    return r;
}

static uint64_t ReadUInt64(const std::string& s, size_t& i) {
    SkipWhitespace(s, i);
    uint64_t v = 0;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
        v = v * 10 + (s[i] - '0');
        ++i;
    }
    return v;
}

static double ReadDouble(const std::string& s, size_t& i) {
    SkipWhitespace(s, i);
    // Read integer part
    double v = 0.0;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
        v = v * 10.0 + double(s[i] - '0');
        ++i;
    }
    if (i < s.size() && s[i] == '.') {
        ++i;
        double frac = 0.1;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
            v += double(s[i] - '0') * frac;
            frac *= 0.1;
            ++i;
        }
    }
    return v;
}

static std::string ReadKey(const std::string& s, size_t& i) {
    return ReadString(s, i);
}

static void LogWarning(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    std::fprintf(stderr, "[BotIdentity] %s", buf);
}

static std::string TruncateUtf8(const std::string& s, size_t maxBytes) {
    if (s.size() <= maxBytes) return s;
    size_t i = maxBytes;
    while (i > 0 && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) --i;
    return s.substr(0, i);
}

static void SkipJsonValue(const std::string& json, size_t& i) {
    SkipWhitespace(json, i);
    if (i >= json.size()) return;
    if (json[i] == '"') {
        ReadString(json, i);
        return;
    }
    if (json[i] == '{' || json[i] == '[') {
        int depth = 0;
        do {
            if (i >= json.size()) return;
            if (json[i] == '"') {
                ReadString(json, i);
                continue;
            }
            if (json[i] == '{' || json[i] == '[') ++depth;
            else if (json[i] == '}' || json[i] == ']') --depth;
            ++i;
        } while (i < json.size() && depth > 0);
        return;
    }
    while (i < json.size() && json[i] != ',' && json[i] != '}' && json[i] != ']') ++i;
}

// Read a top-level block: { k1:v1, k2:v2, ... }
static void ReadBlock(const std::string& json, size_t& i,
                       std::function<void(const std::string& k)> on_key) {
    if (!MatchChar(json, i, '{')) return;
    while (SkipWhitespace(json, i) && json[i] != '}') {
        std::string k = ReadKey(json, i);
        if (k.empty()) break;
        if (!MatchChar(json, i, ':')) break;
        if (on_key) on_key(k);
        if (json[i] == '{') {
            // skip nested object
            int depth = 1;
            while (i < json.size() && depth > 0) {
                if (json[i] == '{') ++depth;
                else if (json[i] == '}') --depth;
                ++i;
            }
        } else if (json[i] == '[') {
            // skip nested array
            int depth = 1;
            while (i < json.size() && depth > 0) {
                if (json[i] == '[') ++depth;
                else if (json[i] == ']') --depth;
                ++i;
            }
        } else {
            // skip scalar
            while (i < json.size() && json[i] != ',' && json[i] != '}') ++i;
        }
        if (!MatchChar(json, i, ',')) break;
    }
    MatchChar(json, i, '}');
}

bool BotInfo::Load(const char* path) {
    std::ifstream f(path);
    if (!f) return false;
    std::stringstream ss; ss << f.rdbuf();
    std::string json = ss.str();
    size_t i = 0;

    if (!MatchChar(json, i, '{')) return false;

    while (SkipWhitespace(json, i) && json[i] != '}') {
        std::string key = ReadKey(json, i);
        if (key.empty()) break;
        if (!MatchChar(json, i, ':')) break;

        if (key == "features") {
            ReadBlock(json, i, [&](const std::string& fk) {
                if (fk == "enableFakePing") {
                    SkipWhitespace(json, i);
                    m_Features.enableFakePing = (json[i] == 't' || json[i] == '1');
                    while (i < json.size() && json[i] != ',' && json[i] != '}') ++i;
                } else if (fk == "fakePingMin") {
                    m_Features.fakePingMin = (int)ReadUInt64(json, i);
                } else if (fk == "fakePingMax") {
                    m_Features.fakePingMax = (int)ReadUInt64(json, i);
                } else if (fk == "enableScoreboardFlair") {
                    SkipWhitespace(json, i);
                    m_Features.enableScoreboardFlair = (json[i] == 't' || json[i] == '1');
                    while (i < json.size() && json[i] != ',' && json[i] != '}') ++i;
                } else if (fk == "scoreboardFlairProbability") {
                    m_Features.scoreboardFlairProbability = ReadDouble(json, i);
                } else if (fk == "enableCrosshair") {
                    SkipWhitespace(json, i);
                    m_Features.enableCrosshair = (json[i] == 't' || json[i] == '1');
                    while (i < json.size() && json[i] != ',' && json[i] != '}') ++i;
                } else if (fk == "defaultScoreboardFlair") {
                    m_Features.defaultScoreboardFlair = (uint32_t)ReadUInt64(json, i);
                } else if (fk == "pingJitterPercent") {
                    m_Features.pingJitterPercent = (int)ReadUInt64(json, i);
                } else if (fk == "voteTransactionHoldFrames") {
                    m_Features.voteTransactionHoldFrames = (int)ReadUInt64(json, i);
                }
            });
        } else {
            // Unknown top-level key: skip it
            if (json[i] == '{') {
                int depth = 1; while (++i < json.size() && depth > 0) {
                    if (json[i] == '{') ++depth; else if (json[i] == '}') --depth;
                } ++i;
            } else if (json[i] == '[') {
                int depth = 1; while (++i < json.size() && depth > 0) {
                    if (json[i] == '[') ++depth; else if (json[i] == ']') --depth;
                } ++i;
            } else {
                while (i < json.size() && json[i] != ',' && json[i] != '}') ++i;
            }
        }
        if (!MatchChar(json, i, ',')) break;
    }
    MatchChar(json, i, '}');
    if (m_Features.voteTransactionHoldFrames < 1)
        m_Features.voteTransactionHoldFrames = 1;
    if (m_Features.voteTransactionHoldFrames > 32)
        m_Features.voteTransactionHoldFrames = 32;
    return true;
}

bool BotInfo::LoadFeatures(const char* path) {
    return Load(path);  // config.json 只含 features，Load 已处理
}

bool BotInfo::LoadBots(const char* path) {
    std::ifstream f(path);
    if (!f) return false;
    std::stringstream ss; ss << f.rdbuf();
    std::string json = ss.str();
    size_t i = 0;

    if (!MatchChar(json, i, '{')) return false;

    m_Bots.clear();
    m_DroppedOnLoad = 0;
    m_TruncatedOnLoad = 0;

    while (SkipWhitespace(json, i) && json[i] != '}') {
        std::string key = ReadKey(json, i);
        if (key.empty()) break;
        if (!MatchChar(json, i, ':')) break;

        if (key == "bots") {
            ReadBlock(json, i, [&](const std::string& botName) {
                BotIdentity bi;
                bi.name = botName;
                bi.steamId = kSteamIdBase + static_cast<uint64_t>(m_Bots.size());
                bi.slot = -1;
                bi.applied = false;

                if (MatchChar(json, i, '{')) {
                    while (SkipWhitespace(json, i) && json[i] != '}') {
                        std::string fk = ReadKey(json, i);
                        if (fk.empty()) break;
                        if (!MatchChar(json, i, ':')) break;
                        if (fk == "steamid") {
                            bi.steamId = ReadUInt64(json, i);
                        } else if (fk == "name") {
                            bi.name = ReadString(json, i);
                        } else if (fk == "ping") {
                            bi.ping = (int)ReadUInt64(json, i);
                        } else if (fk == "crosshair") {
                            bi.crosshair = ReadString(json, i);
                        } else if (fk == "scoreboardFlair") {
                            bi.scoreboardFlair = (uint32_t)ReadUInt64(json, i);
                        }
                        if (!MatchChar(json, i, ',')) break;
                    }
                    MatchChar(json, i, '}');
                }

                if (bi.name.size() > kMaxPersonaNameBytes) {
                    LogWarning("warning: persona name truncated to %zu bytes: '%s'\n",
                               kMaxPersonaNameBytes, bi.name.c_str());
                    bi.name = TruncateUtf8(bi.name, kMaxPersonaNameBytes);
                    ++m_TruncatedOnLoad;
                }

                if (m_Bots.size() < static_cast<size_t>(kMaxBotIdentities)) {
                    m_Bots.push_back(bi);
                } else {
                    ++m_DroppedOnLoad;
                }
            });
        } else {
            SkipJsonValue(json, i);
        }
        if (!MatchChar(json, i, ',')) break;
    }
    MatchChar(json, i, '}');
    if (m_DroppedOnLoad > 0) {
        LogWarning("warning: bots list truncated at kMaxBotIdentities=%d (dropped %d)\n",
                   kMaxBotIdentities, m_DroppedOnLoad);
    }
    return true;
}

bool ReadCssServerLanguage(const char* path, std::string& out) {
    out.clear();
    std::ifstream f(path);
    if (!f) return false;
    std::stringstream ss; ss << f.rdbuf();
    std::string json = ss.str();
    size_t i = 0;
    if (!MatchChar(json, i, '{')) return false;

    while (SkipWhitespace(json, i) && json[i] != '}') {
        std::string key = ReadKey(json, i);
        if (key.empty()) break;
        if (!MatchChar(json, i, ':')) break;
        if (key == "ServerLanguage") {
            out = ReadString(json, i);
            return !out.empty();
        }
        SkipJsonValue(json, i);
        if (!MatchChar(json, i, ',')) break;
    }
    return false;
}

BotListSelection SelectBotList(const std::string& baseDir) {
    BotListSelection sel;
    const std::string corePath = baseDir + "/addons/counterstrikesharp/configs/core.json";
    sel.cssLanguageFound = ReadCssServerLanguage(corePath.c_str(), sel.cssLanguage);
    sel.matched = "default";
    sel.relativeFile = "bots.json";
    sel.absolutePath = baseDir + "/addons/BotIdentity/bots.json";
    if (sel.cssLanguageFound && IsSimplifiedChinese(sel.cssLanguage)) {
        sel.matched = "zh-CN";
        sel.relativeFile = "lang/zh-CN.json";
        sel.absolutePath = baseDir + "/addons/BotIdentity/lang/zh-CN.json";
    }
    return sel;
}

const BotIdentity* BotInfo::GetByIndex(int idx) const {
    if (idx < 0 || idx >= Count()) return nullptr;
    return &m_Bots[idx];
}

const BotIdentity* BotInfo::GetByName(const char* name) const {
    for (const auto& b : m_Bots) {
        if (b.name == name) return &b;
    }
    return nullptr;
}

bool BotInfo::IsSlotActive(int slot) {
    // ConnectionType != 0 means the slot is occupied
    if (g_pNetworkServerService) {
        auto* gameServer = g_pNetworkServerService->GetIGameServer();
        if (gameServer) {
            return gameServer->GetClientConnectionType(CPlayerSlot(slot)) != 0;
        }
    }
    return false;
}

static void ResetForReuse(BotIdentity& b) {
    b.slot = -1;
    b.applied = false;
    // reused is diagnostic only. ApplyDisguise writes the config SteamID64
    // unchanged so Steam CDN avatars keep resolving.
    b.reused = static_cast<uint16_t>(b.reused + 1);
}

// Returns true if a bot with this name is currently active (slot != -1).
// Prevents two live bots from sharing a persona name in the same match.
static bool IsNameInUse(const std::vector<BotIdentity>& bots, const std::string& name,
                        const BotIdentity* self = nullptr) {
    for (const auto& b : bots) {
        if (&b == self) continue;
        if (b.slot >= 0 && b.name == name) return true;
    }
    return false;
}

// Prevents two live bots from sharing a SteamID64 (scoreboard / avatar key).
static bool IsSteamIdInUse(const std::vector<BotIdentity>& bots, uint64_t steamId,
                           const BotIdentity* self = nullptr) {
    if (steamId == 0) return false;
    for (const auto& b : bots) {
        if (&b == self) continue;
        if (b.slot >= 0 && b.steamId == steamId) return true;
    }
    return false;
}

static bool IsAssignable(const std::vector<BotIdentity>& bots, const BotIdentity& candidate) {
    return !IsNameInUse(bots, candidate.name, &candidate) &&
           !IsSteamIdInUse(bots, candidate.steamId, &candidate);
}

BotIdentity* BotInfo::GetFree() {
    // Build a list of candidates: free entries (slot == -1) whose name
    // and SteamID64 are not currently used by another live bot.
    int freeCount = 0;
    for (int i = 0; i < Count(); ++i) {
        if (m_Bots[i].slot < 0 && IsAssignable(m_Bots, m_Bots[i])) ++freeCount;
    }
    if (freeCount > 0) {
        int pick = rand() % freeCount;
        for (int i = 0; i < Count(); ++i) {
            if (m_Bots[i].slot < 0 && IsAssignable(m_Bots, m_Bots[i])) {
                if (pick == 0) return &m_Bots[i];
                --pick;
            }
        }
    }
    // All in use — find one whose slot is no longer valid, also dedup by
    // name and SteamID64. Do not steal an identity from a live slot.
    for (int i = 0; i < Count(); ++i) {
        int s = m_Bots[i].slot;
        if ((s < 0 || s >= 64) && IsAssignable(m_Bots, m_Bots[i])) {
            ResetForReuse(m_Bots[i]);
            return &m_Bots[i];
        }
        if (s >= 0 && s < 64 && !IsSlotActive(s) && IsAssignable(m_Bots, m_Bots[i])) {
            ResetForReuse(m_Bots[i]);
            return &m_Bots[i];
        }
    }
    // Exhausted: refuse rather than hand out a duplicate SteamID64.
    return nullptr;
}

BotIdentity* BotInfo::At(int idx) {
    if (idx < 0 || idx >= Count()) return nullptr;
    return &m_Bots[idx];
}

// ─── IdentityManager ───────────────────────────────────────────────────────

bool IdentityManager::IsManaged(int slot) const {
    if (slot < 0 || slot >= kMaxSlots) return false;
    return m_Slots[slot] >= 0;
}

void IdentityManager::Mark(int slot, int botIndex) {
    if (slot < 0 || slot >= kMaxSlots || botIndex < 0 || botIndex >= kMaxBotIdentities) return;
    if (m_Slots[slot] < 0) ++m_ActiveCount;
    m_Slots[slot] = botIndex;
    m_BotToSlot[botIndex] = slot;
}

void IdentityManager::Unmark(int slot) {
    if (slot < 0 || slot >= kMaxSlots) return;
    int bi = m_Slots[slot];
    if (bi >= 0) {
        if (BotIdentity* identity = BotInfos().At(bi)) {
            identity->slot = -1;
            identity->applied = false;
        }
        m_Slots[slot] = -1;
        m_BotToSlot[bi] = -1;
        --m_ActiveCount;
    }
}

int IdentityManager::Lookup(int slot) const {
    if (slot < 0 || slot >= kMaxSlots) return -1;
    return m_Slots[slot];
}

BotIdentity* IdentityManager::GetIdentity(int slot) {
    int bi = Lookup(slot);
    if (bi < 0) return nullptr;
    return BotInfos().At(bi);
}

BotIdentity* IdentityManager::GetIdentityByBotIndex(int botIndex) {
    return BotInfos().At(botIndex);
}

// ─── Globals ───────────────────────────────────────────────────────────────

static BotInfo g_BotInfo;
static IdentityManager g_IdentityMgr;

BotInfo& BotInfos() { return g_BotInfo; }
IdentityManager& IdentityMgr() { return g_IdentityMgr; }

}  // namespace botid
