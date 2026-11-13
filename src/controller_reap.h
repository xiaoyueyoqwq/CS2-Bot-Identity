#pragma once

#include <cstdint>

namespace botid {

// Queue the controller belonging to a managed bot that is being kicked so
// leftover CCSPlayerController entities do not keep occupying team slots
// (scoreboard / team-select occupancy) after the client has disconnected.
bool QueueControllerRemovalForClient(void* client, int slot);

// Identity-checked UTIL_Remove of queued leftover controllers, then
// trailing leftover shrink of m_Clients.Count(). Run from GameFrame_Post
// after TickVoteTransaction and from population-command Post.
void DrainPendingControllerRemovals();

// Diagnostic: try alternate engine release paths on a dead client slot.
// mode 0=DisconnectClient(reason), 1=KickClient(reason), 2=SetFakePlayer.
void TestLeftoverRelease(int slot, int mode, int reason);

// Log every occupied CServerSideClient in m_Clients (userid/signon/netch).
void DumpOccupiedClients(const char* tag);

// Log remaining cs_player_controller entities (team/flags) in indices 1..64.
void DumpPlayerControllers(const char* tag);

// Log cs_team_manager entities and whether leftover controller handles still
// appear in their memory. Read-only; does not write CTeam vectors.
void DumpTeamManagers(const char* tag);

// Entity census over indices 1..4096: classname histogram plus detail lines
// for pawn/preview/intro/character/teamselect/observer classes. Diagnoses
// leftover entities (e.g. orphaned player pawns) that keep rendering on the
// team-select screen after bot_kick. Read-only.
void DumpGhostEntities(const char* tag);

// Per-entity field dump for team_select_*/team_intro_* positions:
// m_nVariant/m_nOrdinal/m_xuid/m_agentItem. Team-select ghosts mean a
// position keeps xuid/agent after its bot was kicked.
void DumpTeamSelectPositions(const char* tag);

// Scan every entity's first 0x1500 bytes for an exact u64 (e.g. a SteamID64).
// Finds which entity field binds team-select preview occupancy. Read-only.
void ScanEntitiesForU64(uint64_t needle, const char* tag);
// Write the +0x7a0 occupant xuid on team_select_* preview entities.
// matchArg: decimal steamid64 to match, or "all".
void PokeTeamSelectXuid(const char* matchArg, uint64_t newValue);

// Before kickid <userid>: leftover CServerSideClient entries can share that
// userid (signon=7, no netchan). Engine then kicks the ghost. Rewrite those
// leftover userids to 65535 so kickid hits keepSlot. Does not null m_Clients
// entries. Returns how many leftovers were rewritten.
int NeutralizeCollidingLeftoverUserIds(int keepSlot, uint16_t userId);

// Kick/ban ClientDisconnect: still-disguised managed bot on T/CT calls
// ChangeTeam(1) spectator before native-identity restore. JoinTeam is not
// hooked. Only the disconnecting slot is moved — kick Pre must not walk
// every managed bot or bot_kick ct/t sees an empty team.
bool MoveManagedBotToSpectator(int slot);

// After a kick/add command: leftover controllers still on T/CT are moved to
// spectator (or team 0 if already deleting) and UTIL_Remove'd even if they
// were not in the disconnect queue.
void ReapOrphanControllers();
// Master gate for all cleanup writes (deferred destroy, leftover reap,
// member sweep, post-disconnect normalize). When false, BotIdentity only
// does identity work — corpses are left for the engine exactly as-is.
extern bool g_CleanupEnabled;

// Drop the queue without writing entities (unload / server gone).
void ClearPendingControllerRemovals();

// True when the record carries the marked-teardown fingerprint
// (netch=nil && conn&0x08) or its m_nEntityIndex resolves to a dead,
// recycled, or deleting entity. Catches conn=0x01 corpses that look
// identical to live managed bots on every other field.
bool RecordLooksDead(void* client);

// ClientDisconnect POST for a managed slot: if engine teardown stalled,
// strip the record to the challenging fingerprint immediately (signon=0,
// userid=65535, all SteamID copies incl. m_PlayerInfo.xuid zeroed) so the
// slot's playerinfo entry stops replicating before clients snapshot it.
void NormalizeCorpseNow(int slot);

}  // namespace botid
