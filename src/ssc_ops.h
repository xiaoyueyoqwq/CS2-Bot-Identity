#pragma once

#include <cstdint>
#include <cstring>

namespace botid {

// CServerSideClient memory offsets (Linux defaults; gamedata overrides at runtime)
inline int OFF_m_nConnectionTypeFlags = 96;   // byte
inline int OFF_m_bFakePlayer          = 160;  // bool
inline int OFF_m_UserID               = 168;  // uint16
inline int OFF_m_SteamID             = 171;  // uint64
inline int OFF_m_SteamIDMirror       = 179;  // uint64
inline int OFF_m_nEntityIndex        = 76;   // int
inline int OFF_m_nClientSlot         = 72;   // int
inline int OFF_m_NetChannel          = 88;   // INetChannel*
inline int OFF_m_nSignonState        = 100;  // SignonState_t (int)

// CBaseEntity::m_iTeamNum on the controller (uint8/uint32). 0.1.22 live scan
// CT=3 T=2. Writing 0 here (0.1.23) did not clear M occupancy; kick Pre
// ChangeTeam(1) does.
inline int OFF_Controller_TeamNum = 1572;
// CCSPlayerController::ChangeTeam vtable index (live CSS gamedata linux 102).
// Called as a function; JoinTeam is not hooked.
inline int OFF_ChangeTeamVtable = 102;
// CBaseEntity::m_fFlags on the controller (gamedata key keeps the old name).
// Bit 0x100 is FL_FAKECLIENT, which CS:GO IsFakeClient() used to exclude voters.
inline int OFF_Controller_FakeClientFlags = 904;  // uint32_t
// Documented schema dump offset for CBasePlayerController::m_steamID (0x0708).
// 0.1.2 live vote-window reads at this offset were a heap pointer, not a
// SteamID64. The vote path must not write it unless a scan shows the
// disguise SteamID64 actually lives here.
inline int OFF_Controller_SteamID = 1800;  // uint64, schema 0x0708 (unverified live)

// CNetworkGameServerBase::m_Clients
inline int OFF_ClientList = 584;  // CUtlVector<CServerSideClient*>

constexpr uint32_t kFakeClientBit = 0x100;

// ─── Raw write helpers ───────────────────────────────────────────────────────

inline void ClearFakePlayer(void* client) {
    auto* raw = reinterpret_cast<unsigned char*>(client);
    auto& flags = raw[OFF_m_nConnectionTypeFlags];
    flags = static_cast<unsigned char>((flags & ~0x08u) | 0x01u);
    raw[OFF_m_bFakePlayer] = 0;
}

inline void SetFakePlayer(void* client) {
    auto* raw = reinterpret_cast<unsigned char*>(client);
    auto& flags = raw[OFF_m_nConnectionTypeFlags];
    flags = static_cast<unsigned char>((flags & ~0x01u) | 0x08u);
    raw[OFF_m_bFakePlayer] = 1;
}

inline bool IsFakePlayerSet(const void* client) {
    auto* raw = reinterpret_cast<const unsigned char*>(client);
    return raw[OFF_m_bFakePlayer] == 0x01;
}

inline void WriteSteamId(void* client, uint64_t steamId) {
    auto* raw = reinterpret_cast<unsigned char*>(client);
    std::memcpy(raw + OFF_m_SteamID,       &steamId, sizeof(steamId));
    std::memcpy(raw + OFF_m_SteamIDMirror, &steamId, sizeof(steamId));
}

inline uint64_t ReadSteamId(const void* client) {
    auto* raw = reinterpret_cast<const unsigned char*>(client);
    uint64_t steamId = 0;
    std::memcpy(&steamId, raw + OFF_m_SteamID, sizeof(steamId));
    return steamId;
}

inline int GetEntityIndex(const void* client) {
    auto* raw = reinterpret_cast<const unsigned char*>(client);
    return *reinterpret_cast<const int*>(raw + OFF_m_nEntityIndex);
}

inline uint16_t ReadUserId(const void* client) {
    auto* raw = reinterpret_cast<const unsigned char*>(client);
    uint16_t userId = 0;
    std::memcpy(&userId, raw + OFF_m_UserID, sizeof(userId));
    return userId;
}

inline int ReadClientSlot(const void* client) {
    auto* raw = reinterpret_cast<const unsigned char*>(client);
    return *reinterpret_cast<const int*>(raw + OFF_m_nClientSlot);
}

inline int ReadSignonState(const void* client) {
    auto* raw = reinterpret_cast<const unsigned char*>(client);
    return *reinterpret_cast<const int*>(raw + OFF_m_nSignonState);
}

inline void* ReadNetChannel(const void* client) {
    auto* raw = reinterpret_cast<const unsigned char*>(client);
    void* netChannel = nullptr;
    std::memcpy(&netChannel, raw + OFF_m_NetChannel, sizeof(netChannel));
    return netChannel;
}

inline unsigned char ReadConnectionTypeFlags(const void* client) {
    auto* raw = reinterpret_cast<const unsigned char*>(client);
    return raw[OFF_m_nConnectionTypeFlags];
}

// ─── Controller helpers ──────────────────────────────────────────────────────

inline uint8_t ReadControllerTeam(const void* controller) {
    auto* raw = reinterpret_cast<const unsigned char*>(controller);
    uint32_t team = 0;
    std::memcpy(&team, raw + OFF_Controller_TeamNum, sizeof(team));
    return static_cast<uint8_t>(team);
}

inline void WriteControllerTeam(void* controller, uint8_t team) {
    auto* raw = reinterpret_cast<unsigned char*>(controller);
    uint32_t value = team;
    std::memcpy(raw + OFF_Controller_TeamNum, &value, sizeof(value));
}

inline void ClearControllerFakeClientFlag(void* controller) {
    auto* raw = reinterpret_cast<unsigned char*>(controller);
    auto* flags = reinterpret_cast<uint32_t*>(raw + OFF_Controller_FakeClientFlags);
    *flags &= ~kFakeClientBit;
}

inline void SetControllerFakeClientFlag(void* controller) {
    auto* raw = reinterpret_cast<unsigned char*>(controller);
    auto* flags = reinterpret_cast<uint32_t*>(raw + OFF_Controller_FakeClientFlags);
    *flags |= kFakeClientBit;
}

inline uint64_t ReadControllerSteamId(const void* controller) {
    auto* raw = reinterpret_cast<const unsigned char*>(controller);
    uint64_t steamId = 0;
    std::memcpy(&steamId, raw + OFF_Controller_SteamID, sizeof(steamId));
    return steamId;
}

inline void WriteControllerSteamId(void* controller, uint64_t steamId) {
    auto* raw = reinterpret_cast<unsigned char*>(controller);
    std::memcpy(raw + OFF_Controller_SteamID, &steamId, sizeof(steamId));
}

}  // namespace botid
