#pragma once

#include <cstdint>

namespace botid {

// Wire format (must match BotHider's slot_shm.h)
constexpr uint32_t kShmMagic    = 0x44494842;  // 'BHID'
constexpr uint32_t kShmVersion = 1;
constexpr int      kShmMaxSlots = 64;
constexpr int      kShmNameLen  = 32;
// Total size must match BotHider slot_shm.h (1_064_960 bytes)
constexpr int      kShmTotalSize = 1'064'960;

// Offsets
constexpr int kOff_Magic         = 0;     // uint32
constexpr int kOff_Version       = 4;     // uint32
constexpr int kOff_MaxSlots      = 8;     // uint32
constexpr int kOff_SlotState     = 16;    // byte[64]
constexpr int kOff_SyntheticSid  = 80;    // uint64[64]
constexpr int kOff_PersonaName   = 592;   // char[64][32]
constexpr int kOff_CurrentPing   = 5720;  // int32[64]
constexpr int kOff_Crosshair     = 5976;  // char[64][64]
constexpr int kCrosshairLen      = 64;
constexpr int kOff_ScoreboardFlair = 10400;  // uint32[64]
constexpr int kOff_Incarnation   = 13216;  // uint64[64]

// Initialize the shared memory mapping.
// Returns true if mapping exists and is ready.
bool InitSharedMemory();

// Mark a slot as managed by our plugin.
// Writes SlotState=1, SyntheticSid, PersonaName.
void PublishAdopt(int slot, uint64_t steamId, const char* name);

// Mark a slot as released.
void PublishRelease(int slot);

// Write ping value (read by C# BotHiderImpl)
void PublishPing(int slot, int ping);

// Write crosshair code
void PublishCrosshair(int slot, const char* code);

// Write scoreboard flair itemDefIndex
void PublishScoreboardFlair(int slot, uint32_t itemDefIndex);

// Bump the slot incarnation counter so C# detects identity change
void BumpIncarnation(int slot);

// Cleanup at plugin unload.
void ShutdownSharedMemory();

}  // namespace botid
