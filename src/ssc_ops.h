#pragma once

#include <cstdint>
#include <cstring>

namespace botid {

// CServerSideClient memory offsets (Linux defaults; gamedata overrides at runtime)
inline int OFF_m_nConnectionTypeFlags = 96;   // byte
inline int OFF_m_bFakePlayer          = 160;  // bool
inline int OFF_m_SteamID             = 171;  // uint64
inline int OFF_m_SteamIDMirror       = 179;  // uint64
inline int OFF_m_nEntityIndex        = 76;   // int

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

// ─── Controller helpers ──────────────────────────────────────────────────────

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
