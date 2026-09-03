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

// CBasePlayerController FakeClientFlags (controller-side bit for client display)
inline int OFF_Controller_FakeClientFlags = 904;  // uint32_t, bit 0x100 = "is bot"

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

}  // namespace botid
