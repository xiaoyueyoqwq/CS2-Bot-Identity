// BotIdentityApi: public, read-only contract that the BotIdentity
// native plugin exposes via CounterStrikeSharp's PluginCapability
// system. Key = "botidentity:api".
//
// Snapshot fields mirror the SlotState/SyntheticSid/PersonaName/
// Incarnation entries in /dev/shm/CS2BotHider_Slots that the
// native plugin writes. Slot is not durable — a fresh occupant
// of the same engine slot may have a new Incarnation. Consumers
// must re-validate slot+incarnation pairs.

namespace BotIdentityApi;

public enum BotIdentityMode
{
    Player = 0, // bot is disguised to look like a real player
    Bot = 1,    // bot is left with native Valve bot markers
}

public readonly struct ManagedBotSnapshot
{
    public int Slot { get; init; }
    public ulong Incarnation { get; init; }
    public bool Connected { get; init; }
    public BotIdentityMode IdentityMode { get; init; }
    public ulong SteamId { get; init; }
    public string PersonaName { get; init; }
}

public interface IBotIdentityApi
{
    /// <summary>
    /// True when the shared-memory bridge to the native plugin is reachable.
    /// </summary>
    bool IsConnected { get; }

    /// <summary>
    /// Returns the engine slots currently claimed by the native plugin.
    /// Empty array when no bots are managed or the bridge is not connected.
    /// </summary>
    int[] GetManagedSlots();

    /// <summary>
    /// True if <paramref name="slot"/> is currently occupied by a
    /// BotIdentity-managed bot at the time of the call.
    /// </summary>
    bool IsManagedBot(int slot);

    /// <summary>
    /// Monotonic incarnation counter. Re-used slots get a new incarnation
    /// on the next adopt. Consumers that cached a previous snapshot must
    /// re-validate this against the bridge before trusting it.
    /// </summary>
    ulong GetSlotIncarnation(int slot);

    /// <summary>
    /// Synthetic SteamID64 the native plugin has written into the engine.
    /// Returns 0 for unmanaged slots.
    /// </summary>
    ulong GetBotSteamId(int slot);

    /// <summary>
    /// Persona name the native plugin has written.
    /// Returns empty string for unmanaged slots.
    /// </summary>
    string GetPersonaName(int slot);

    /// <summary>
    /// Whether <paramref name="slot"/> is still managed and its incarnation
    /// matches. Used to validate cached snapshots across slot reuse.
    /// </summary>
    bool IsManagedBotIncarnation(int slot, ulong incarnation);

    /// <summary>
    /// Atomic snapshot of every currently managed slot. Empty if no bots are
    /// managed or the bridge is not connected.
    /// </summary>
    ManagedBotSnapshot[] GetManagedBotSnapshots();

    /// <summary>
    /// Returns the current global identity mode the native plugin is in.
    /// </summary>
    BotIdentityMode GetIdentityMode();
}
