#pragma once

#include <cstdint>
#include <cstring>
#include <string>

// Memory offsets for CServerSideClient (Linux)
// These may need adjustment for different game versions
namespace offsets {
    inline int m_Name = 64;              // CUtlString
    inline int m_bFakePlayer = 160;      // bool
    inline int m_SteamID = 171;          // CSteamID (uint64)
    inline int m_SteamIDMirror = 179;    // mirrored CSteamID
    inline int m_nConnectionTypeFlags = 96; // byte
}

// Direct memory operations for CServerSideClient
namespace memory_ops {

    // Set fake player flag
    inline void SetFakePlayer(void* client) {
        auto* raw = reinterpret_cast<unsigned char*>(client);
        raw[offsets::m_bFakePlayer] = 1;
        // Also clear the connection type flag
        raw[offsets::m_nConnectionTypeFlags] &= ~0x08;
    }

    // Clear fake player flag
    inline void ClearFakePlayer(void* client) {
        auto* raw = reinterpret_cast<unsigned char*>(client);
        raw[offsets::m_bFakePlayer] = 0;
        // Set the connection type flag
        raw[offsets::m_nConnectionTypeFlags] |= 0x08;
    }

    // Check if fake player flag is set
    inline bool IsFakePlayerSet(const void* client) {
        auto* raw = reinterpret_cast<const unsigned char*>(client);
        return raw[offsets::m_bFakePlayer] == 0x01;
    }

    // Write SteamID directly to memory
    inline void WriteSteamId(void* client, uint64_t steamId) {
        auto* raw = reinterpret_cast<unsigned char*>(client);
        std::memcpy(raw + offsets::m_SteamID, &steamId, sizeof(steamId));
        std::memcpy(raw + offsets::m_SteamIDMirror, &steamId, sizeof(steamId));
    }

    // Read SteamID from memory
    inline uint64_t ReadSteamId(const void* client) {
        auto* raw = reinterpret_cast<const unsigned char*>(client);
        uint64_t steamId = 0;
        std::memcpy(&steamId, raw + offsets::m_SteamID, sizeof(steamId));
        return steamId;
    }

    // Set player name (requires CUtlString manipulation)
    // This is more complex and may need additional implementation
    inline void SetPlayerName(void* client, const char* name) {
        // TODO: Implement CUtlString manipulation
        // This requires understanding the CUtlString structure
    }

    // Get player name
    inline const char* GetPlayerName(const void* client) {
        auto* raw = reinterpret_cast<const unsigned char*>(client);
        auto* utl = reinterpret_cast<const char* const*>(raw + offsets::m_Name);
        return *utl;
    }
}
