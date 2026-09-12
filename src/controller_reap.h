#pragma once

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

// Kick Pre: still-disguised managed bots on T/CT call ChangeTeam(1) spectator
// before native-identity restore. JoinTeam is not hooked.
void MoveManagedBotsToSpectator();

// After a kick/add command: leftover controllers still on T/CT are moved to
// spectator (or team 0 if already deleting) and UTIL_Remove'd even if they
// were not in the disconnect queue.
void ReapOrphanControllers();

// Drop the queue without writing entities (unload / server gone).
void ClearPendingControllerRemovals();

}  // namespace botid
