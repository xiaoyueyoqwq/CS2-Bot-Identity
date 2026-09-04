# BotIdentity

A Metamod:Source native plugin for CS2 that handles bot identity at the
engine level. Publishes bot identity data via the shared-memory protocol
`CS2BotHider_Slots`, so any consumer that reads from this region (such as
`BotHiderImpl.dll` in CS2-Bot-Improve) can pick it up.

## What it does

On `IServerGameClients::OnClientConnected`, for fake-player (bot) clients:

1. Locate the `CServerSideClient*` for the connecting slot
2. Clear `m_bFakePlayer` and update `m_nConnectionTypeFlags`
3. Write a synthetic `m_SteamID` (and `m_SteamIDMirror`) from the config
4. Clear `FakeClientFlags` bit `0x100` on the controller entity, when
   reachable
5. Publish slot metadata (state, SteamID, persona name) to
   `/dev/shm/CS2BotHider_Slots`

On `OnClientDisconnect`, restore the bot to its native state and publish
slot release to the same shm region.

## File layout

```
CS2-Bot-Identity/
├── src/
│   ├── plugin.cpp        ── IServerGameClients + IServerGameDLL hooks, lifecycle
│   ├── ssc_ops.h         ── ClearFakePlayer / SetFakePlayer / WriteSteamId
│   ├── entity_access.cpp ── resolve CServerSideClient* and entity controller
│   ├── bot_info.cpp      ── JSON parsers for config.json / bots.json
│   └── shm_pub.cpp       ── shm region creator + data publishers
├── CMakeLists.txt
├── config.json           ── plugin-wide feature toggles
├── bots.json             ── per-bot identity list
├── gamedata.json         ── memory-offset overrides
└── README.md
```

## Configuration

Two files. The plugin loads both at startup; either may be omitted (defaults
apply).

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
    "pingJitterPercent": 30
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

Per-bot overrides take precedence: if a bot has `"ping": 18` in `bots.json`
and `18 < fakePingMin`, the bot keeps 18 as its base, then the jitter
applies. If the per-bot value falls within the range, the range is used
instead.

### `bots.json` — per-bot identity list

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
