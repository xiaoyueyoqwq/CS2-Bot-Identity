# BotIdentity

A Metamod:Source native plugin for CS2 dedicated servers. It disguises
Valve bots as players at the engine level (SteamID, persona name, ping,
crosshair, scoreboard flair) and publishes that identity to shared memory
so C# plugins can consume it.

Current version **0.1.33**. License: MIT.

There are two layers:

| Layer | What it is | Required when |
|---|---|---|
| Native `BotIdentity.so` | Metamod plugin. Rewrites bot identity in the engine. | Always |
| C# `BotIdentityImpl` | Reads the native plugin's shared-memory region, exposes `botidentity:api`, and assigns teammate colors | Only if CounterStrikeSharp is installed |

The native plugin writes `/dev/shm/CS2BotHider_Slots`. Same-host consumers
(this repo's `BotIdentityImpl`, or `BotHiderImpl.dll` in CS2-Bot-Improve)
read that region. They do not write engine fields themselves.

The plugin never Steam-authenticates as the borrowed accounts. SteamID64
values are only written into engine fields so the scoreboard can fetch a
CDN avatar. VAC on a borrowed profile does not VAC a secure server.

---

## What it does

On `IServerGameClients::OnClientConnected`, for fake-player (bot) clients:

1. Resolve the `CServerSideClient*` for the connecting slot.
2. Clear `m_bFakePlayer` and update `m_nConnectionTypeFlags`.
3. Write `m_SteamID` and `m_SteamIDMirror` from the config SteamID64 as-is
   (`sidReuseMutate=off`; two live slots never share one ID).
4. Clear controller `FL_FAKECLIENT` (`0x100`) when a
   `cs_player_controller` is reachable.
5. Publish slot metadata (state, SteamID, persona name, ping, crosshair,
   flair, incarnation) to `/dev/shm/CS2BotHider_Slots`.

On `OnClientDisconnect`, restore Valve's native bot markers and publish
slot release. Kick/ban disconnects first call
`CCSPlayerController::ChangeTeam(1)` (spectator) on **that slot** while
the bot is still disguised, then queue the controller for a deferred
`UTIL_Remove`. Kick Pre does not walk every managed bot: if it did,
`bot_kick ct` / `t` would see an empty team, leave bots in spectator, and
the next round would spawn them via `info_player_start` (missing on most
CS maps). Player-mode disguise otherwise makes Valve keep that controller
on T/CT (team-select occupancy and scoreboard ghosts) after the name has
already left the player list.

This plugin only writes a small, well-defined set of fields per bot. It
does not intercept `MaintainBotQuota`, `HandleCommand_JoinTeam`,
`SameMapTeardown`, or `PackEntities`. `UTIL_Remove` is resolved from
`gamedata.json` against the `libserver.so` that hosts
`IServerGameClients` (Metamod ships another file of the same name) and
called for leftover kick controllers; it is not detoured.

---

## Requirements

- Linux CS2 dedicated server
- [Metamod:Source](https://www.sourcemm.net/) (CS2)
- Optional: [CounterStrikeSharp](https://docs.cssharp.dev/) for teammate
  colors and `botidentity:api`

The native `.so` **cannot be hot-unloaded**. Changing the binary,
`gamedata.json`, or the identity list requires a full CS2 process
restart. `meta unload` is not sufficient.

---

## Install

Copy these files. `<server>` is the CS2 install root (the directory that
contains `game/csgo/`).

```text
<server>/game/csgo/addons/BotIdentity/bin/linuxsteamrt64/BotIdentity.so
<server>/game/csgo/addons/BotIdentity/gamedata.json
<server>/game/csgo/addons/BotIdentity/config.json
<server>/game/csgo/addons/BotIdentity/bots.json
<server>/game/csgo/addons/BotIdentity/lang/zh-CN.json
<server>/game/csgo/addons/metamod/BotIdentity.vdf
```

`BotIdentity.vdf` already points at the `.so` path.

For the C# side (teammate colors / capability):

```text
<server>/game/csgo/addons/counterstrikesharp/plugins/BotIdentityImpl/BotIdentityImpl.dll
<server>/game/csgo/addons/counterstrikesharp/shared/BotIdentityApi/BotIdentityApi.dll
```

`BotIdentityApi.dll` must live under `shared/` so the CSS loader publishes
it for other plugins to reference.

Restart the CS2 process. The console should log:

```text
[BotIdentity] loaded version=0.1.33 ...
```

If the C# side is installed:

```text
[BotIdentityImpl] shm bridge connected; botidentity:api registered
```

`shared memory region not found` means the native plugin did not load, or
the shm region was not created. Check Metamod for `BotIdentity` first.

---

## Configuration

The plugin loads `config.json` and one bot list at startup. Either file
may be omitted; defaults apply.

Which bot list is used follows CounterStrikeSharp
`addons/counterstrikesharp/configs/core.json` key `ServerLanguage`
(RFC 4646). The native plugin reads that JSON itself. It does **not**
use the C# `CoreConfig.ServerLanguage` API: Metamod loads before CSS, and
the first map's bots connect before C# is up.

| `ServerLanguage` | List |
|---|---|
| Simplified Chinese family: `zh`, `zh-CN`, `zh-Hans`, `zh-Hans-CN`, … | `lang/zh-CN.json` |
| Traditional Chinese: `zh-TW`, `zh-Hant`, `zh-HK`, `zh-MO` | `bots.json` |
| Any other language, missing CSS, missing key, or parse failure | `bots.json` |

If `lang/zh-CN.json` is selected but missing, empty, or unreadable, the
plugin falls back to `bots.json` and logs a warning. The load log includes
the CSS tag, the match (`zh-CN` or `default`), and the file used.

The identity pool cap is `kMaxBotIdentities` (256). Live engine slots stay
64; `GetFree()` draws at random from the pool. Persona names are at most
31 bytes of UTF-8 (Chinese ≈ 10 characters); longer names are truncated on
a code-point boundary and logged.

### `config.json` — plugin-wide feature toggles

```json
{
  "features": {
    "enableFakePing": true,
    "fakePingMin": 20,
    "fakePingMax": 90,
    "enableScoreboardFlair": true,
    "scoreboardFlairProbability": 0.3,
    "enableCrosshair": true,
    "defaultScoreboardFlair": 0,
    "pingJitterPercent": 30,
    "voteTransactionHoldFrames": 3
  },
  "mapBlacklist": ["3171695956", "cabin"]
}
```

| Field | Type | Default | Effect |
|---|---|---|---|
| `enableFakePing` | bool | true | Master switch for ping overrides |
| `fakePingMin` | int | 20 | Low end of the random sample range |
| `fakePingMax` | int | 90 | High end of the random sample range |
| `enableScoreboardFlair` | bool | true | Master switch for scoreboard flair |
| `scoreboardFlairProbability` | double | 0.3 | Per-bot roll for flair assignment (0.0–1.0) |
| `enableCrosshair` | bool | true | Master switch for crosshair codes |
| `defaultScoreboardFlair` | uint32 | 0 | Fallback flair when the per-bot value is unset |
| `pingJitterPercent` | int | 30 | ±N% per-bot ping jitter applied every ~30s |
| `voteTransactionHoldFrames` | int | 3 | GameFrame ticks to keep native bot markers after `callvote` / kick returns (clamped 1–32) |
| `mapBlacklist` | string[] | `[]` | Top-level (not inside `features`). See [Map blacklist](#map-blacklist) |

Per-bot overrides take precedence: if a bot has `"ping": 18` in `bots.json`
and `18 < fakePingMin`, the bot keeps 18 as its base, then jitter applies.
If the per-bot value falls within the range, the range is used instead.

### `bots.json` / `lang/zh-CN.json` — per-bot identity list

```json
{
  "bots": {
    "Bot01": {
      "steamid": 76561197961483905,
      "name": "henry",
      "ping": 62,
      "crosshair": "CSGO-M9OZ1-5NPW6-SY5O0-LAMEI-G4Y7V",
      "scoreboardFlair": 2481
    }
  }
}
```

| Field | Type | Required | Effect |
|---|---|---|---|
| `steamid` | uint64 | yes | SteamID64 written to `m_SteamID` / `m_SteamIDMirror` as-is (CDN avatar key) |
| `name` | string | yes | Persona name (31 bytes UTF-8 + NUL; truncated if longer) |
| `ping` | int | no | Base ping; out-of-range treated as override |
| `crosshair` | string | no | Crosshair code (64 bytes) |
| `scoreboardFlair` | uint32 | no | ItemDefIndex written to `InventoryServices::m_rank[]` by consumers |

Pick SteamID64 values whose community profiles have a **non-default**
avatar, so the scoreboard can fetch it from the Steam CDN. Clicking the
avatar opens that real profile. Identities are not bound to pro player
IDs. Recycle no longer rewrites the low 16 bits of the SteamID64.

`lang/zh-CN.json` keeps 二次元 names and CNCS 神人 IDs, and replaces bland
路人 names with shorter meme lines. Verify avatars with:

```bash
python3 tools/verify_steamids.py --json lang/zh-CN.json
```

---

## Map blacklist

`mapBlacklist` is a [PluginToggle](https://github.com/xiaoyueyoqwq/CS2-Plugin-Toggle)-style case-insensitive substring list
(workshop maps look like `workshop/<id>/<bsp>`). A match **suspends
hosting** without unloading the `.so`:

- restore already-managed slots to Valve bots
- skip disguise, vote/kick identity windows, named `kickid`, and ping jitter

Leaving the map resumes hosting for **new** bots only. Already-native
bots on the server are not re-disguised. Missing or empty list never
suspends. Default tokens match [PluginToggle](https://github.com/xiaoyueyoqwq/CS2-Plugin-Toggle) cabin: `3171695956`, `cabin`.
The gate does not kick bots and does not null `m_Clients` pointers.

Matching uses live `IGameServer::GetMapName()` first (the same string
[PluginToggle](https://github.com/xiaoyueyoqwq/CS2-Plugin-Toggle) reads as `Server.MapName`), then `GetAddonName()` if the map
name is still empty. Workshop maps can leave `GetAddonName` set after a
later official `changelevel`; the two strings are **not** concatenated, or
cabin tokens would stay hot.

The gate is evaluated in all of these places, because map name is empty
at `Load()` and can stay empty through `OnLevelInit` on
`host_workshop_map`:

1. `Load()` after `AddListener` (Metamod does not deliver `OnLevelInit`
   unless the plugin is in `CPlugin::m_Events`)
2. `IMetamodListener::OnLevelInit`
3. every `GameFrame_Post` (`PollMapGate`)
4. `host_workshop_map` / `ds_workshop_changelevel` / `changelevel` / `map`
   Arg(1) as an early **suspend-only** hint — a miss does not unsuspend,
   because the live map is still the previous one

Empty-string substring match is a no-op (does not unsuspend).
`botidentity_dump` prints `live=` and `addon=` next to `map=` (`s_GateMap`).

---

## Identity transaction (vote / kick)

Player-mode disguise makes Valve count managed bots as human voters and
treat `bot_kick` as a human disconnect. `callvote`, `bot_kick`, `kick`,
`kickid`, and `banid` therefore share one identity transaction.

This does **not** replace the native vote UI. CounterStrikeSharp plugins
such as BotVoteFix still cannot rewrite Valve's internal voter ledger by
patching `CVoteController`. BotVoteFix can Schema-zero
`CBasePlayerController::m_steamID` during the same window (the write path
BotHiderImpl already uses for the scoreboard). Zeroing more live SteamID
copies or bumping `voteTransactionHoldFrames` does not change that ledger.

### Window

1. `DispatchConCommand` Pre snapshots each managed slot (full window) or
   only the named slot (see [Kick paths](#kick-paths)) and restores
   Valve's native bot markers **without** `MarkEntityStateChanged`
   (otherwise the scoreboard flashes BOT / empty SteamID):
   `m_bFakePlayer`, controller `FL_FAKECLIENT` (`0x100`), and the
   `CServerSideClient` SteamID pair set to 0. It then scans the SSC
   (first 2048 bytes) and controller (first 4096 bytes) for extra
   `uint64` copies of that slot's disguise SteamID64 and zeros only
   those matches. Hardcoded controller `+1800`
   (`CBasePlayerController::m_steamID`, schema `0x0708`) is **not**
   written unless that scan finds the disguise SteamID64 there — live
   reads at 1800 have been a heap pointer, not a SteamID.
2. `DispatchConCommand` Post does **not** close the transaction. Valve
   builds the voter pool on the first vote Think, and processes kick
   disconnects, after the command returns. Ending the window in that
   Post hook immediately re-disguises still-connected slots.
3. `GameFrame_Post` holds those markers for `voteTransactionHoldFrames`
   (default 3, about 50ms at 64 tick). Each hold tick re-zeros the
   known copies if another plugin rewrote them and logs
   `steamid reappeared` when that happens. The first hold tick also
   logs `GetClientXUID` / `GetClientSteamID` /
   `GetPlayerNetworkIDString`. After the hold, the player disguise and
   the snapshotted SteamID copies are written back.

Per-slot client/controller-handle checks skip a slot that disconnects or
is rebound during the hold. If the game server goes away, snapshots are
dropped and no restore write is attempted.

`bot_add` / `bot_add_t` / `bot_add_ct` do **not** open this window. New
bots must be disguised during the add command: with `bot_quota 0` the
engine Console-kicks native bots (`NETWORK_DISCONNECT_KICKED`) before
disguise can land. If add arrives while a kick/vote hold is still open,
that hold is force-ended first.

---

## Kick paths

Valve `bot_kick <name>` matches Valve profile names (`status` still shows
names like BeastTamer). CSS / BotQuotaManager send the **persona** from
the identity list. Named `bot_kick <persona>` maps that persona to the
slot's userid and issues `kickid` inside the identity window so BQM `-1`
can remove one bot.

| Command | Identity window | Spectator before kick | Notes |
|---|---|---|---|
| `bot_kick <persona>` | **That slot only** | `ChangeTeam(1)` on that slot while still disguised | Remaining managed bots keep SteamIDs so the scoreboard does not drop their rows |
| `bot_kick all` / bare `bot_kick` | All managed slots | `ChangeTeam(1)` every managed slot while still disguised, **then** open the window | Otherwise the team-select backdrop keeps pawn occupancy and later `bot_add` stacks on ghosts |
| `bot_kick t` / `ct` | All managed slots | **No** Pre-`ChangeTeam` | Pre-moving the whole team makes the engine see an empty team (fake-death / `info_player_start` respawn). Those bots move to spectator in `ClientDisconnect` Pre |
| `kick` / `kickid` / `banid` | All managed slots | Disconnect-time `ChangeTeam(1)` on the disconnecting slot only | Nested `kickid` from a named `bot_kick` only increments vote depth; it does not recapture everyone |

Userid **0 is valid** (first bot on this dedicated build). Only **65535**
is rejected as leftover.

Leftover `CServerSideClient` rows can keep a kicked bot's userid
(`signon=7`, no netchan). Named `kickid <userid>` then hits that ghost
while the disguised slot only went spectator and respawns next round.
Before `kickid`, unmanaged clients with the same userid, no netchan, and
SteamID 0 are rewritten to userid 65535. `m_Clients` pointers are not
nulled.

Kick leftovers are reaped on a later GameFrame after handle/userid
checks. Hibernate with 0 humans often skips GameFrame, so population
command Post also drains the queue. Disconnect-time restore is
best-effort: if a bot is removed and re-added in the same tick, the
entity may already be gone. A shm consumer then sees the slot as released
and skips re-apply.

---

## Shared memory protocol

`/dev/shm/CS2BotHider_Slots`, 1 064 960 bytes, magic `'BHID'` (`0x44494842`),
version 1. Layout must match BotHider `slot_shm.h`.

| Offset | Field | Type | Notes |
|---|---|---|---|
| 0 | Magic | `uint32` | `'BHID'` |
| 4 | Version | `uint32` | `1` |
| 8 | MaxSlots | `uint32` | `64` |
| 16 | SlotState | `byte[64]` | 0 = unmanaged, 1 = managed |
| 80 | SyntheticSid | `uint64[64]` | disguise SteamID64 |
| 592 | PersonaName | `char[64][32]` | 31 bytes UTF-8 + NUL |
| 5720 | CurrentPing | `int32[64]` | 0 = do not override |
| 5976 | Crosshair | `char[64][64]` | |
| 10400 | ScoreboardFlair | `uint32[64]` | ItemDefIndex; 0 = none |
| 13216 | Incarnation | `uint64[64]` | bumps on adopt so C# can detect reuse |

Other fields exist in the upstream protocol (sig entries, avatar data)
and are not written by this plugin. Consumers must validate
`Slot + Incarnation` together because the engine reuses slots.

---

## Console

| Command | Effect |
|---|---|
| `botidentity_dump` | Read-only occupancy dump: version, `suspended=`, `map=` / `live=` / `addon=`, blacklist tokens, then the same client / controller / team-manager lines population Post prints. Does not kick anyone |

---

## C# API

`BotIdentityImpl` is a pure reader of the shm region. It registers the
CounterStrikeSharp capability `botidentity:api` (`IBotIdentityApi`) and
assigns teammate colors so managed bots do not collide with humans.

Do **not** call `SetModel` on zombies (CS2 `tm_leet_*` or s2ze workshop
walker/frozen models) and then apply colors: those models crash with
segfault 139 when color slots 1–4 are written.

Interface details: [`csharp/README.md`](csharp/README.md). Cached snapshots
must re-validate `slot + incarnation`.

---

## File layout

```
CS2-Bot-Identity/
├── src/
│   ├── plugin.cpp            IServerGameClients + IServerGameDLL hooks, map gate, kick routing
│   ├── vote_transaction.cpp  callvote / kick / add identity window
│   ├── controller_reap.cpp   deferred UTIL_Remove of leftover controllers; leftover userid rewrite
│   ├── ssc_ops.h             fake-client flags, SSC SteamID, controller m_steamID
│   ├── entity_access.cpp     resolve CServerSideClient* and entity controller; gamedata load
│   ├── bot_info.cpp          JSON parsers for config.json / bots.json / CSS core.json
│   └── shm_pub.cpp           shm region creator + data publishers
├── csharp/
│   ├── BotIdentityApi/       IBotIdentityApi contract (net10.0)
│   └── BotIdentityImpl/      shm reader + capability + teammate colors
├── CMakeLists.txt
├── config.json               plugin-wide feature toggles + mapBlacklist
├── bots.json                 default per-bot identity list
├── lang/zh-CN.json           Simplified Chinese identity list
├── gamedata.json             memory-offset / signature overrides
├── BotIdentity.vdf           Metamod plugin descriptor
└── tools/verify_steamids.py  SteamID64 avatar check
```

---

## Build

Requires `hl2sdk-cs2` and `metamod-source` headers (not the full engine).

```bash
mkdir build && cd build
HL2SDKCS2=/path/to/hl2sdk-cs2 \
MMSOURCE_DEV=/path/to/metamod-source \
cmake .. -DCMAKE_BUILD_TYPE=Release
make
```

C# side:

```bash
dotnet build csharp/BotIdentityApi/BotIdentityApi.csproj -c Release
dotnet build csharp/BotIdentityImpl/BotIdentityImpl.csproj -c Release
```

Outputs: `build/BotIdentity.so` and each project's `bin/Release/net10.0/`.

---

## Offsets and updates

`CServerSideClient` member offsets are compiled in (`src/ssc_ops.h`) and
overridden at runtime from `gamedata.json` when present. If a CS2 update
moves them, update both and rebuild.

Linux defaults currently compiled in:

| Field | Offset |
|---|---|
| `CServerSideClient::m_nClientSlot` | 72 |
| `CServerSideClient::m_nEntityIndex` | 76 |
| `CServerSideClient::m_NetChannel` | 88 |
| `CServerSideClient::m_nConnectionTypeFlags` | 96 |
| `CServerSideClient::m_nSignonState` | 100 |
| `CServerSideClient::m_bFakePlayer` | 160 |
| `CServerSideClient::m_UserID` | 168 |
| `CServerSideClient::m_SteamID` | 171 |
| `CServerSideClient::m_SteamIDMirror` | 179 |
| `CBaseEntity::m_fFlags` (`FL_FAKECLIENT`) | 904 |
| `CBaseEntity::m_iTeamNum` | 1572 |
| `CBasePlayerController::m_steamID` (do not write blindly) | 1800 |
| `CNetworkGameServerBase::m_Clients` | 584 |
| `CCSPlayerController::ChangeTeam` vtable (linux) | 102 |

`UTIL_Remove` is a signature in `gamedata.json`, resolved against the
`libserver.so` that hosts `IServerGameClients`.

---

## Related plugins

| Plugin | Relationship |
|---|---|
| CS2-Bot-Improve `BotHiderImpl` | Reads the same shm region for scoreboard appearance |
| This repo `BotIdentityImpl` | Reads the same shm region; publishes `botidentity:api`; writes teammate colors |
| BotQuotaManager-style quota plugins | Add/remove via `bot_kick <persona>` or `bot_kick all`; do not kick Valve default names |
| [PluginToggle](https://github.com/xiaoyueyoqwq/CS2-Plugin-Toggle) | Unloads C# plugins by map. Cabin-style maps must also be in `mapBlacklist`, or the native layer still disguises |
| BotVoteFix / CS2-Vote-Improver | Valve's voter ledger is still Valve's. BotIdentity only temporarily restores native bot markers in the command window |

Vote-window research notes that are not install docs live in
`HANDOVER-FABLE-5.1.md` (and the matching file in CS2-Vote-Improver).

---

## License

MIT.
