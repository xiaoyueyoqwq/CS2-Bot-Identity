# BotIdentity: A minimal native replacement for BotHider

## Summary

I've written a **drop-in replacement** for the native BotHider plugin that:

- **Same shared-memory wire format** as BotHider (`/dev/shm/CS2BotHider_Slots`)
- Works **unmodified** with the existing `BotHiderImpl.dll` C# plugin
- **~300 lines of C++** vs BotHider's ~2500 lines
- **No `funchook` detours** — only standard SourceHook vtable hooks
- **No population transaction logic** — the source of most BotHider crashes
- Survives aggressive `bot_quota` stress testing on a live competitive server

## Stress test results

On `de_dust2` competitive, server PID 1230033, no crash on:

| Test | Operations | Result |
|------|------------|--------|
| Rapid `bot_add`/`bot_kick` | 30 cycles × 0.6s | 0 crashes |
| Rapid `bot_quota 0..10` change | 20 transitions × 0.5s | 0 crashes |
| Mixed quota + add + kick | 5 × 3 ops | 0 crashes |

`BotHiderImpl.dll` reported 0 `write failed` errors throughout.

## Why is BotHider so much more complex?

Looking at BotHider's recent commit log, the same areas are repeatedly
patched:

```
c7b8488 fix: make player disguise population transaction-safe
d4f404c fix: keep bot identity stable during population updates
8bce3f4 fix: correct bot_quota doubling in player mode
420c0a0 fix: guard bot identity hooks during teardown
40e3f32 fix: serialize identity restoration during teardown
01643b5 fix: skip population identity transactions in bot mode
```

BotHider implements a **"population transaction"** because:

1. `MaintainBotQuota` runs every few seconds and may add/remove bots
2. `bot_quota` may change from CVars (e.g. `!gamemode` plugins)
3. Each of these can race with the disguise write path
4. If a bot is moved between player-mode and bot-mode while identity is being
   applied, the client sees a half-baked state and disconnects

The transaction logic is ~600 lines of C++ that does:

- A snapshot/rollback mechanism
- Hooks on `MaintainBotQuota`, `HandleCommand_JoinTeam`,
  `SameMapTeardown`, `PackEntities`, `HumanTeamRestriction`
- A `populationCommandDepth` / `callVoteCommandDepth` counter
- Coordination with `Vote Improver` for vote-triggered population changes

**BotIdentity has none of this.** It only intercepts `OnClientConnected` and
writes a small set of fields. The risk of half-baked identity is the same as
with the Valve original — the client only sees a name + SteamID + a few
fields, none of which can crash the client if set in any order.

## Implementation

```
src/
├── plugin.cpp        ─ IServerGameClients hooks (OnClientConnected, Disconnect)
├── ssc_ops.h         ─ ClearFakePlayer / SetFakePlayer / WriteSteamId
├── entity_access.cpp ─ resolve CServerSideClient* and entity controller
├── bot_info.cpp      ─ bot_info.json parser (no nlohmann dependency)
└── shm_pub.cpp       ─ shm region creator + data publishers
```

The shared memory publisher implements the same offsets as BotHider
(see `CS2-Bot-Hider/src/SharedMemory/slot_shm.h`):

```c
constexpr int kOff_SlotState        = 16;     // byte[64]
constexpr int kOff_SyntheticSid     = 80;     // uint64[64]
constexpr int kOff_PersonaName      = 592;    // char[64][32]
constexpr int kOff_CurrentPing      = 5720;   // int32[64]
constexpr int kOff_Crosshair        = 5976;   // char[64][64]
constexpr int kOff_ScoreboardFlair  = 10400;  // uint32[64]
constexpr int kOff_Incarnation      = 13216;  // uint64[64]
```

## Proposed integration

```
1. Add CS2-Bot-Identity as a git submodule of CS2-Bot-Improve
2. Move its build artifact to addons/BotIdentity/bin/linuxsteamrt64/
3. Update shared/BotHiderApi to depend on BotIdentity's shm as primary
4. BotHiderImpl.dll works unchanged (same shm protocol)
5. Keep plugins/BotHiderImpl/ in Bot-Improve for the C# side
```

## What I need from you

- Permission to add this to the Bot-Improve deployment pipeline
- Review of the C# side of `IBotHiderApi` to see if any read-side helper
  is worth moving into the new plugin
- Feedback on whether to keep BotHider (legacy) as an option, or fully
  replace

## Source

The full plugin source is at:
`https://github.com/xiaoyueyoqwq/CS2-Bot-Identity` (or your fork)
