# C# sub-projects

Two CounterStrikeSharp plugins that expose the native plugin's
managed-bot registry to other C# plugins.

## `BotIdentityApi/`

Public, read-only contract published as the `botidentity:api`
CounterStrikeSharp capability. The interface mirrors the metadata the
native plugin writes to `/dev/shm/CS2BotHider_Slots`:

- `IsConnected` — true when the shared-memory bridge is reachable
- `GetManagedSlots()` — engine slots currently claimed by the native plugin
- `IsManagedBot(int slot)` — single-slot liveness check
- `GetSlotIncarnation(int slot)` — monotonic counter, increments on slot reuse
- `GetBotSteamId(int slot)` / `GetPersonaName(int slot)` — identity fields
- `IsManagedBotIncarnation(int, ulong)` — cache-friendly validation
- `GetManagedBotSnapshots()` — atomic snapshot of every managed slot
- `GetIdentityMode()` — global mode (always `Player` in this plugin)

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

The plugin does **not** write to the shared region. It is a pure
read consumer. The capability registration is the only side effect.

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
