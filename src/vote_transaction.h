#pragma once

namespace botid {

// Opens the native identity transaction for a vote command. While active,
// every managed slot temporarily carries Valve's native bot markers
// (m_bFakePlayer, FL_FAKECLIENT, CServerSideClient SteamID pair = 0, plus
// any extra uint64 copies of the disguise SteamID64 found on the SSC or
// controller). Hardcoded controller +1800 is not written unless a scan
// shows the disguise SteamID64 actually lives there.
void BeginVoteTransaction();

// Closes one nesting level of the vote transaction. When the outermost level
// closes, the player disguise is re-applied to every slot that is still
// managed and whose client/controller did not change identity mid-vote.
void EndVoteTransaction();

// After callvote dispatch returns, keep native markers for `frames` more
// GameFrame_Post callbacks so Valve's first vote Think still sees bots as
// bots. Nested callvotes extend the same window rather than stacking closes.
void ScheduleVoteTransactionEnd(int frames);

// Count down a scheduled end. Must run at the start of GameFrame_Post,
// before any early-return in that hook. Drops the transaction without
// writing entities if the game server has already gone away.
void TickVoteTransaction();

// True while native bot markers are applied for a vote, including the
// post-dispatch GameFrame hold.
bool VoteTransactionActive();

// Emergency drop used on disconnect/teardown paths; discards snapshots so the
// restore step will not touch entities that no longer belong to us.
void ResetVoteTransaction();

// Drops only the snapshot of one slot (used when that slot disconnects or is
// rebound mid-vote). The rest of the transaction stays intact.
void ResetVoteTransactionSlot(int slot);

}  // namespace botid
