# BotIdentity

A Metamod:Source native plugin for CS2 that handles bot identity at the
engine level. Publishes bot identity data via the shared-memory protocol
`CS2BotHider_Slots`, so any consumer that reads from this region (such as
`BotHiderImpl.dll` in CS2-Bot-Improve) can pick it up.

## What it does

On `IServerGameClients::OnClientConnected`, for fake-player (bot) clients:

1. Locate the `CServerSideClient*` for the connecting slot
2. Clear `m_bFakePlayer` and update `m_nConnectionTypeFlags`
3. Write `m_SteamID` (and `m_SteamIDMirror`) from the config SteamID64 as-is
4. Clear `FakeClientFlags` bit `0x100` on the controller entity, when
   reachable
5. Publish slot metadata (state, SteamID, persona name) to
   `/dev/shm/CS2BotHider_Slots`

On `OnClientDisconnect`, restore the bot to its native state and publish
slot release to the same shm region.

### Native vote window

Player-mode disguise makes Valve count managed bots as human voters.
`callvote` is wrapped in an identity transaction:

1. `DispatchConCommand` pre snapshots each managed slot and restores
   Valve's native bot markers without `MarkEntityStateChanged`:
   `m_bFakePlayer`, controller `FL_FAKECLIENT` (`0x100`), and the
   `CServerSideClient` SteamID pair set to 0. It then scans the SSC
   (first 2048 bytes) and controller (first 4096 bytes) for extra
   `uint64` copies of that slot's disguise SteamID64 and zeros only
   those matches. Hardcoded controller `+1800` is **not** written
   unless that scan finds the disguise SteamID64 there — 0.1.2 live
   reads at 1800 were a heap pointer, not a SteamID.
2. `DispatchConCommand` post does **not** close the transaction. Valve
   builds the voter pool on the first vote Think, after the command
   returns.
3. `GameFrame_Post` holds those markers for `voteTransactionHoldFrames`
   (default 3, about 50ms at 64 tick). Each hold tick re-zeros the
   known copies if another plugin rewrote them and logs
   `steamid reappeared` when that happens. The first hold tick also
   logs `GetClientXUID` / `GetClientSteamID` /
   `GetPlayerNetworkIDString`. After the hold, the player disguise
   and the snapshotted SteamID copies are written back.

0.1.1 held only the fake-client flags; live votes still saw Valve write
`potential=4` during that hold. 0.1.2 also zeroed live SSC SteamID and
unconditionally wrote controller `+1800`; a global changelevel vote
still got `potential=6`. 0.1.3 stopped writing `+1800`, scanned for
SteamID64 copies (live hits `s171,s179,s432,s440,c2528`), and paired
with BotVoteFix 1.1.2 Schema zero of `m_steamID` at offset **2528**.
A live changelevel (`team=-1`, 1 human + 9 bots, 2026-09-05 20:23)
still got Valve `potential=10` during the hold while
`GetClientXUID=0`, `GetClientSteamID=0`,
`GetPlayerNetworkIDString=BOT`, and schema `m_steamID=0`. Stop: do
not bump hold; do not zero another live SteamID copy. Handover:
`HANDOVER-FABLE-5.1.md` and CS2-Vote-Improver
`docs/HANDOVER-FABLE-5.1.md`.

Per-slot client/controller-handle checks skip a slot that disconnects or
is rebound during the hold. If the game server goes away, snapshots are
dropped and no restore write is attempted.

This does not replace the native vote UI. CounterStrikeSharp plugins such
as BotVoteFix still cannot rewrite Valve's internal voter ledger by
patching `CVoteController`. BotVoteFix 1.1.2 uses Schema to zero
`CBasePlayerController::m_steamID` during the same window (the write
path BotHiderImpl already uses for the scoreboard).

## File layout

```
CS2-Bot-Identity/
├── src/
│   ├── plugin.cpp            ── IServerGameClients + IServerGameDLL hooks, lifecycle
│   ├── vote_transaction.cpp  ── callvote identity window
│   ├── ssc_ops.h             ── fake-client flags, SSC SteamID, controller m_steamID
│   ├── entity_access.cpp     ── resolve CServerSideClient* and entity controller
│   ├── bot_info.cpp          ── JSON parsers for config.json / bots.json / CSS core.json
│   └── shm_pub.cpp           ── shm region creator + data publishers
├── CMakeLists.txt
├── config.json           ── plugin-wide feature toggles
├── bots.json             ── default per-bot identity list
├── lang/
│   └── zh-CN.json        ── Simplified Chinese identity list
├── gamedata.json         ── memory-offset overrides
└── README.md
```

## Configuration

`config.json` and a bot list. The plugin loads both at startup; either may be
omitted (defaults apply). Which bot list is used follows CounterStrikeSharp
`ServerLanguage` (see below).

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
  }
}
```

| Field | Type | Default | Effect |
|---|---|---|---|
| `enableFakePing` | bool | true | Master switch for ping overrides |
| `fakePingMin` | int | 20 | Low end of random sample range |
| `fakePingMax` | int | 90 | High end of random sample range |
| `enableScoreboardFlair` | bool | true | Master switch for scoreboard flair |
| `scoreboardFlairProbability` | double | 0.3 | Per-bot roll for flair assignment (0.0–1.0) |
| `enableCrosshair` | bool | true | Master switch for crosshair code |
| `defaultScoreboardFlair` | uint32 | 0 | Fallback flair when per-bot value is unset |
| `pingJitterPercent` | int | 30 | ±N% per-bot ping jitter applied every 30s |
| `voteTransactionHoldFrames` | int | 3 | GameFrame ticks to keep native bot markers after `callvote` returns (clamped 1–32) |

Per-bot overrides take precedence: if a bot has `"ping": 18` in `bots.json`
and `18 < fakePingMin`, the bot keeps 18 as its base, then the jitter
applies. If the per-bot value falls within the range, the range is used
instead.

### Language-selected bot lists

On load the native plugin reads CounterStrikeSharp
`addons/counterstrikesharp/configs/core.json` key `ServerLanguage` (RFC 4646).
It does **not** use the C# `CoreConfig.ServerLanguage` API: Metamod loads
before CSS, and the first map's bots connect before C# is up.

| `ServerLanguage` | List |
|---|---|
| Simplified Chinese family: `zh`, `zh-CN`, `zh-Hans`, `zh-Hans-CN`, … | `lang/zh-CN.json` |
| Traditional Chinese: `zh-TW`, `zh-Hant`, `zh-HK`, `zh-MO` | `bots.json` |
| Any other language, missing CSS, missing key, or parse failure | `bots.json` |

If `lang/zh-CN.json` is selected but missing, empty, or unreadable, the
plugin falls back to `bots.json` and logs a warning. Load log includes the
CSS tag, the match (`zh-CN` or `default`), and the file used.

The cap is 64 identities (`kMaxBotIdentities`). Persona names are at most
31 bytes of UTF-8 (Chinese ≈ 10 characters); longer names are truncated on
a code-point boundary and logged.

`lang/zh-CN.json` keeps the 15 二次元 names and the CNCS 神人 IDs, and
replaces bland 路人 names with shorter meme lines. Each `steamid` in that
file is a real SteamID64 whose community profile has a non-default avatar,
so the scoreboard can fetch it from the Steam CDN. Clicking the avatar
opens that real profile. Identities are not bound to VAC-banned accounts
or pro player IDs. Recycle no longer rewrites the low 16 bits of the
SteamID64 (`sidReuseMutate=off`); two live slots never share one ID.

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
| `ping` | int | no | Base ping value; out-of-range treated as override |
| `crosshair` | string | no | Crosshair code (64 bytes) |
| `scoreboardFlair` | uint32 | no | ItemDefIndex written to `InventoryServices::m_rank[]` |

## Build

Requires `hl2sdk-cs2` and `metamod-source` (only the headers, not the
full engine).

```bash
mkdir build && cd build
HL2SDKCS2=/path/to/hl2sdk-cs2 \
MMSOURCE_DEV=/path/to/metamod-source \
cmake .. -DCMAKE_BUILD_TYPE=Release
make
```

## Install

```bash
cp BotIdentity.so <server>/game/csgo/addons/BotIdentity/bin/linuxsteamrt64/
cp config.json    <server>/game/csgo/addons/BotIdentity/
cp bots.json      <server>/game/csgo/addons/BotIdentity/
mkdir -p          <server>/game/csgo/addons/BotIdentity/lang
cp lang/zh-CN.json <server>/game/csgo/addons/BotIdentity/lang/
cp BotIdentity.vdf <server>/game/csgo/addons/metamod/
# addons/metamod/BotIdentity.vdf points to the .so path
```

A restart of the CS2 process is required (standard for any Metamod native).

## Shared memory protocol

`/dev/shm/CS2BotHider_Slots`, 1 064 960 bytes, magic `'BHID'`.

| Offset | Field | Notes |
|---|---|---|
| 16 | SlotState | `byte[64]` 0=unmanaged 1=managed |
| 80 | SyntheticSid | `uint64[64]` |
| 592 | PersonaName | `char[64][32]` |
| 5720 | CurrentPing | `int32[64]` |
| 5976 | Crosshair | `char[64][64]` |
| 10400 | ScoreboardFlair | `uint32[64]` |
| 13216 | Incarnation | `uint64[64]` |

Other fields exist in the upstream protocol (sig entries, avatar data) and
are not written by this plugin.

## Notes

- This plugin only writes a small, well-defined set of fields per bot. It
  does not intercept `MaintainBotQuota`, `HandleCommand_JoinTeam`,
  `SameMapTeardown`, or `PackEntities`.
- CServerSideClient member offsets are compiled in. If a CS2 update moves
  them, the offsets in `src/ssc_ops.h` and `src/entity_access.cpp` need
  updating and a rebuild is required.
- Disconnect-time restore is best-effort. If a bot is removed and re-added
  in the same tick, the entity may already be gone. A consumer that reads
  from the shm will see the slot as released and skip re-apply.

## License

MIT.
