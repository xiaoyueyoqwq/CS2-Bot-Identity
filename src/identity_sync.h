#pragma once

#include <cstdint>

namespace botid {

struct BotIdentity;

// Client-visible xuid sync. The scoreboard/avatar resolves from the
// controller's networked m_steamID, which the engine fills from the
// client identity at its own transitions. Since 1.41.9.7 that fill can
// lag until round-live on match resets, leaving raw/zero/stale values
// visible through freeze time (black "?" or wrong avatars). This module
// pins the field to the disguise for as long as the slot stays managed.
//
// It deliberately does nothing while a native-identity transaction is
// active: the vote window's zeroed copies must win over this sync.
void NotifyIdentityBound(int slot);
void NotifyIdentityDisconnected(int slot);
void SyncControllerIdentity(int slot, BotIdentity* identity);
void TickIdentitySync();
void ResetIdentitySync();

}  // namespace botid
