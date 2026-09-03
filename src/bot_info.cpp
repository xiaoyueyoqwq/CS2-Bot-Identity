#include "bot_info.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <eiface.h>
#include <fstream>
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

static std::string ReadKey(const std::string& s, size_t& i) {
    return ReadString(s, i);
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

        if (key == "bots") {
            if (!MatchChar(json, i, '{')) break;
            while (SkipWhitespace(json, i) && json[i] != '}') {
                std::string botName = ReadKey(json, i);
                if (botName.empty()) break;
                if (!MatchChar(json, i, ':')) break;
                if (!MatchChar(json, i, '{')) break;

                BotIdentity bi;
                bi.name = botName;
                bi.steamId = kSteamIdBase + static_cast<uint64_t>(m_Bots.size());
                bi.slot = -1;
                bi.applied = false;

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
                if (!MatchChar(json, i, '}')) break;
                if (m_Bots.size() < static_cast<size_t>(kMaxBotIdentities)) {
                    m_Bots.push_back(bi);
                }
                if (!MatchChar(json, i, ',')) break;
            }
            if (!MatchChar(json, i, '}')) break;
        }
        if (!MatchChar(json, i, ',')) break;
    }
    MatchChar(json, i, '}');
    return true;
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
    // When recycled, derive a slot-specific synthetic SteamID so each
    // bot on the server ends up with a unique id even if it reuses a name.
    // The high 48 bits come from the base, the low 16 bits encode reuse count
    // and slot-relative index (assigned at Mark time via a separate rewrite).
    b.reused = static_cast<uint16_t>(b.reused + 1);
}

BotIdentity* BotInfo::GetFree() {
    // First pass: return any unused entry
    for (int i = 0; i < Count(); ++i) {
        if (m_Bots[i].slot < 0) return &m_Bots[i];
    }
    // Second pass: all in use — find one whose slot is no longer valid
    for (int i = 0; i < Count(); ++i) {
        int s = m_Bots[i].slot;
        if (s < 0 || s >= 64) {
            ResetForReuse(m_Bots[i]);
            return &m_Bots[i];
        }
        // If slot isn't actually a bot anymore, recycle
        if (!IsSlotActive(s)) {
            ResetForReuse(m_Bots[i]);
            return &m_Bots[i];
        }
    }
    // All slots valid — recycle the first one (handles over-quota joins)
    ResetForReuse(m_Bots[0]);
    return &m_Bots[0];
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
