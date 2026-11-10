# C# sub-projects

Two CounterStrikeSharp plugins that expose the native plugin's
managed-bot registry to other C# plugins.

## `BotIdentityApi/`

Contract published as the `botidentity:api` CounterStrikeSharp
capability. Snapshot methods mirror the metadata the native plugin
writes to `/dev/shm/CS2BotHider_Slots` (filename is a historical leftover;
BotHider is not loaded). Population methods add and
remove disguised bots so callers do not have to guess persona names
or kick a bot that is being taken over.

- `IsConnected` — true when the shared-memory bridge is reachable
- `GetManagedSlots()` — engine slots currently claimed by the native plugin
- `IsManagedBot(int slot)` — single-slot liveness check
- `GetSlotIncarnation(int slot)` — monotonic counter, increments on slot reuse
- `GetBotSteamId(int slot)` / `GetPersonaName(int slot)` — identity fields
- `IsManagedBotIncarnation(int, ulong)` — cache-friendly validation
- `GetManagedBotSnapshots()` — atomic snapshot of every managed slot
- `GetIdentityMode()` — global mode (always `Player` in this plugin)
- `TryAddBot(int team)` — `bot_add t|ct` (team `2` = T, `3` = CT). Native still intercepts that command to apply disguise.
- `TryRemoveOneFromTeam(int team)` — kick a random managed bot on that team that is not being taken over, by persona name. Skip is per bot, not per menu opener: a possessed bot is skipped for every requester. If every remaining bot on the team is being taken over, returns `Deferred` and does not issue `bot_kick` this tick. Unidentified takeover (a human is possessing someone, but no managed bot on any team could be named) also returns `Deferred` for that team; if another team's bot is already identified, this team's unmarked bots stay kickable. Callers should ask the operator to retry after the next round starts.
- `TryRemoveSlot(int slot)` — kick one managed slot, or return `Deferred` if it is being taken over
- `TryRemoveAll()` — kick bots that are not being taken over one by one; skip possessed bots this tick. If takeover is happening but no specific bot on any team can be identified, kicks nobody this tick. Does **not** issue `bot_kick all`

New population methods have default implementations that return
`Unsupported`, so an older `BotIdentityImpl` still loads against a
newer contract dll.

The struct `ManagedBotSnapshot` carries the same fields as the rows
in that snapshot array. Consumers must validate `Slot + Incarnation`
together because the engine may re-use a slot between snapshot capture
and use.

This project has no dependencies beyond `Microsoft.NET.Sdk` and
targets `net10.0`. Build with `dotnet build -c Release`.

## `BotIdentityImpl/`

The implementation. Registers the capability on `Load` and
`OnAllPluginsLoaded` (a soft re-register covers the case where the
native plugin started before CounterStrikeSharp finished loading).
The implementation is a thin reader on top of `SharedMemoryClient`,
which opens the same shared-memory region the native plugin writes.

The plugin does **not** write to the shared region. Snapshot calls are
read-only. Population calls issue `bot_add` / named `bot_kick` so the
native plugin's existing command hooks run (spectator while disguised,
`kickid`, leftover controller reap). It does not pin `bot_quota`.

Targets `net10.0`. Depends on the sibling `BotIdentityApi` project.
Build with `dotnet build -c Release`.

## Deployment

Copy `BotIdentityImpl/bin/Release/net10.0/BotIdentityImpl.dll` and
`BotIdentityApi/bin/Release/net10.0/BotIdentityApi.dll` to:

```
game/csgo/addons/counterstrikesharp/shared/BotIdentityApi/
game/csgo/addons/counterstrikesharp/plugins/BotIdentityImpl/
```

The shared DLL must be in the `shared/` subtree so the loader
publishes it for other plugins to reference.
