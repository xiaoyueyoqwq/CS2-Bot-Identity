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

// Log every occupied CServerSideClient in m_Clients (userid/signon/netch).
void DumpOccupiedClients(const char* tag);

// Log remaining cs_player_controller entities (team/flags) in indices 1..64.
void DumpPlayerControllers(const char* tag);

// Log cs_team_manager entities and whether leftover controller handles still
// appear in their memory. Read-only; does not write CTeam vectors.
void DumpTeamManagers(const char* tag);

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

// Drop the queue without writing entities (unload / server gone).
void ClearPendingControllerRemovals();

}  // namespace botid
