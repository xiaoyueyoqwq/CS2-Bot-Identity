# BotIdentity

A minimal **drop-in replacement** for the native BotHider plugin, focused on
stability and simplicity. Implements the same shared-memory wire format as
BotHider so the BotHiderImpl C# plugin (in CS2-Bot-Improve) works unchanged.

## Why

The upstream BotHider project:

- Is **~2500 lines of C++** with multiple funchook detours
- Implements a fragile **population transaction** that races with the engine
  during `bot_quota` changes and `MaintainBotQuota` calls
- Crashes frequently when bots are added/removed, especially on competitive
  mode where `bot_quota` is adjusted mid-round by game systems

BotIdentity is **~300 lines of C++** with **no funchook, no transactions, no
multi-hook coordination**. It does exactly one thing:

> **On bot connect: write `m_bFakePlayer = 0`, write synthetic SteamID, write
> FakeClientFlags = 0, write shared-memory slot metadata. On disconnect:
> restore all fields. Nothing else.**

## Architecture

```
┌──────────────────────────────────────────────────────────────┐
│  BotIdentity native (this project)                            │
│  ─ OnClientConnected hook:                                    │
│    1. Locate CServerSideClient* by slot                        │
│    2. ClearFakePlayer (m_bFakePlayer=0, conn_flags=0x01)        │
│    3. WriteSteamId (m_SteamID + m_SteamIDMirror)               │
│    4. ResolveEntityInstance + ClearControllerFakeClientFlags  │
│    5. PublishAdopt → shm                                       │
│  ─ OnClientDisconnect hook:                                    │
│    1. Restore everything to native bot state                   │
│    2. PublishRelease → shm                                     │
└────────────────────────┬─────────────────────────────────────────┘
                         │  /dev/shm/CS2BotHider_Slots
                         │  (BotHider-compatible wire format)
                         ▼
┌──────────────────────────────────────────────────────────────┐
│  BotHiderImpl C# (in CS2-Bot-Improve)                          │
│  ─ Reads shm, applies per-bot identity via Schema API:        │
│    m_iszPlayerName, m_steamID, m_iPing, m_szCrosshairCodes,    │
│    InventoryServices.m_rank (scoreboard flair)                 │
│  ─ Adds IBotHiderApi capability for other plugins             │
└──────────────────────────────────────────────────────────────┘
```

## File layout

```
CS2-Bot-Identity/
├── src/
│   ├── plugin.cpp        ── IServerGameClients hooks, lifecycle
│   ├── ssc_ops.h         ── ClearFakePlayer / SetFakePlayer / WriteSteamId
│   ├── entity_access.cpp ── resolve CServerSideClient* and entity controller
│   ├── bot_info.cpp      ── bot_info.json parser (no nlohmann dependency)
│   └── shm_pub.cpp       ── shm region creator + data publishers
├── CMakeLists.txt
├── bot_info.json         ── per-bot config (name, steamid, ping, crosshair, flair)
└── gamedata.json         ── offset overrides
```

## Drop-in replacement

Replace `<server>/game/csgo/addons/metamod/BotHider.vdf` and the
corresponding `.so`/`BotHider.dll` with `BotIdentity.vdf` and `BotIdentity.so`.
The shm name `CS2BotHider_Slots` is preserved, so:

- `BotHiderImpl.dll` (in `addons/counterstrikesharp/plugins/BotHiderImpl/`)
  works unchanged
- `shared/BotHiderApi/BotHiderApi.dll` works unchanged
- `bot_info.json` is the new config file (replaces BotHider's C++ persona
  registry)

## Configuration

```json
{
  "bots": {
    "TestBot1": {
      "steamid": 76561198000000001,
      "name": "Test Bot 1",
      "ping": 35,
      "crosshair": "CSGO-pE5f8-6RQvk-HLpdN-KW3J6-BQwLA",
      "scoreboardFlair": 874
    }
  }
}
```

| Field | Type | Required | Effect |
|---|---|---|---|
| `steamid` | uint64 | yes | Synthetic SteamID written to `m_SteamID` |
| `name` | string | yes | Persona name (32 bytes NUL-padded UTF-8) |
| `ping` | int | no | When > 0, written to `CCSPlayerController::m_iPing` |
| `crosshair` | string | no | Crosshair code (64 bytes) |
| `scoreboardFlair` | uint32 | no | ItemDefIndex written to `InventoryServices::m_rank[]` |

## Build

```bash
mkdir build && cd build
HL2SDKCS2=/path/to/hl2sdk-cs2 \
MMSOURCE_DEV=/path/to/metamod-source \
cmake .. -DCMAKE_BUILD_TYPE=Release
make
```

## Install

```bash
# Copy
cp BotIdentity.so \
  <server>/game/csgo/addons/BotIdentity/bin/linuxsteamrt64/
cp bot_info.json gamedata.json \
  <server>/game/csgo/addons/BotIdentity/
cp BotIdentity.vdf \
  <server>/game/csgo/addons/metamod/

# Disable BotHider native (BotHiderImpl C# stays)
mv <server>/game/csgo/addons/BotController/BotHider.disabled \
   <server>/game/csgo/addons/BotController/BotHider  # if applicable

# Restart CS2
```

## Stability notes

- **No** `MaintainBotQuota` / `HandleCommand_JoinTeam` / `SameMapTeardown`
  hooks. The population transaction race that plagued BotHider is impossible
  here because we never touch the bot quota.
- **No** `funchook` detours. Only SourceHook vtable hooks on
  `IServerGameClients` — same stability profile as the rest of Metamod.
- **Disconnect-time restore** is best-effort: if a bot is killed and re-added
  in the same tick, the entity may already be gone. BotHiderImpl will detect
  the released slot and not try to re-apply.
- **Game update survival**: `CServerSideClient` member offsets are loaded
  from `gamedata.json` at startup. If a CS2 update moves them, only the
  gamedata file needs updating (no rebuild required).

## Limitations

This plugin intentionally does **not** implement:

- bot_kick voting coordination
- Same-map teardown preservation
- Population transaction snapshot/restore
- Avatar upload (BotHiderImpl handles this separately)
- Custom crosshair UI
- Vote Improver integration

These features rely on complex multi-hook orchestration in the original
BotHider and were excluded to keep the implementation auditable. If a use
case arises, file an issue and we can add it incrementally.
