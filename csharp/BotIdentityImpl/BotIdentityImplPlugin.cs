using BotIdentityApi;
using CounterStrikeSharp.API;
using CounterStrikeSharp.API.Core;
using CounterStrikeSharp.API.Core.Capabilities;
using Microsoft.Extensions.Logging;

namespace BotIdentityImpl;

public sealed class BotIdentityImplPlugin : BasePlugin
{
    public override string ModuleName => "BotIdentityImpl";
    public override string ModuleVersion => "0.1.0";
    public override string ModuleAuthor => "CS2-Bot-Identity";
    public override string ModuleDescription =>
        "Reads bot identity metadata from the BotIdentity native plugin via shared memory.";

    public static PluginCapability<IBotIdentityApi> Capability { get; } =
        new("botidentity:api");

    private readonly SharedMemoryClient _client = new();
    private IBotIdentityApi? _api;

    public override void Load(bool hotReload)
    {
        if (_client.TryConnect())
        {
            _api = new BotIdentityApiImpl(_client);
            Capabilities.RegisterPluginCapability(Capability, () => _api);
            Logger.LogInformation("[BotIdentityImpl] shm bridge connected; botidentity:api registered");
        }
        else
        {
            // Soft dependency: log a warning, fall back to engine IsBot
            // for any consumer that gracefully handles null capability.
            Logger.LogWarning("[BotIdentityImpl] shared memory region not found; " +
                "botidentity:api not registered. Did the BotIdentity native plugin load?");
        }
    }

    public override void OnAllPluginsLoaded(bool hotReload)
    {
        if (_api == null) TryRegister();
    }

    private void TryRegister()
    {
        if (_client.TryConnect())
        {
            _api = new BotIdentityApiImpl(_client);
            Capabilities.RegisterPluginCapability(Capability, () => _api);
            Logger.LogInformation("[BotIdentityImpl] botidentity:api registered (late)");
        }
    }

    public override void Unload(bool hotReload)
    {
        _api = null;
        _client.Dispose();
    }
}

internal sealed class BotIdentityApiImpl : IBotIdentityApi
{
    private readonly SharedMemoryClient _client;

    public BotIdentityApiImpl(SharedMemoryClient client)
    {
        _client = client;
    }

    public bool IsConnected => _client.IsConnected;

    public int[] GetManagedSlots() => _client.GetManagedSlots();
    public bool IsManagedBot(int slot) => _client.IsManagedBot(slot);
    public ulong GetBotSteamId(int slot) => _client.GetBotSteamId(slot);
    public string GetPersonaName(int slot) => _client.GetPersonaName(slot);
    public ulong GetSlotIncarnation(int slot) => _client.GetSlotIncarnation(slot);
    public bool IsManagedBotIncarnation(int slot, ulong incarnation) =>
        _client.IsManagedBotIncarnation(slot, incarnation);
    public ManagedBotSnapshot[] GetManagedBotSnapshots() =>
        _client.GetManagedBotSnapshots();
    public BotIdentityMode GetIdentityMode() => _client.GetIdentityMode();
}
