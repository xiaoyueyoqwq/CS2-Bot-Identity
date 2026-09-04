#pragma once

namespace botid {

// Opens the native identity transaction for a vote command. While active,
// every managed slot temporarily carries Valve's native bot markers
// (CServerSideClient::m_bFakePlayer + controller FakeClientFlags bit 0x100)
// so the engine's voter-pool construction excludes managed bots.
void BeginVoteTransaction();

// Closes one nesting level of the vote transaction. When the outermost level
// closes, the player disguise is re-applied to every slot that is still
// managed and whose client/controller did not change identity mid-vote.
void EndVoteTransaction();

// True while a vote command is being dispatched.
bool VoteTransactionActive();

// Emergency drop used on disconnect/teardown paths; discards snapshots so the
// restore step will not touch entities that no longer belong to us.
void ResetVoteTransaction();

// Drops only the snapshot of one slot (used when that slot disconnects or is
// rebound mid-vote). The rest of the transaction stays intact.
void ResetVoteTransactionSlot(int slot);

}  // namespace botid
