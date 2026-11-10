using BotIdentityApi;
using CounterStrikeSharp.API;
using CounterStrikeSharp.API.Core;
using CounterStrikeSharp.API.Core.Capabilities;
using Microsoft.Extensions.Logging;

namespace BotIdentityImpl;

public sealed partial class BotIdentityImplPlugin : BasePlugin
{
    public override string ModuleName => "BotIdentityImpl";
    public override string ModuleVersion => "0.2.3";
    public override string ModuleAuthor => "CS2-Bot-Identity";
    public override string ModuleDescription =>
        "Reads bot identity metadata from the BotIdentity native plugin via shared memory.";

    public static PluginCapability<IBotIdentityApi> Capability { get; } =
        new("botidentity:api");

    private readonly SharedMemoryClient _client = new();
    private IBotIdentityApi? _api;
    private BotIdentityApiImpl? _apiImpl;
    private CounterStrikeSharp.API.Modules.Timers.Timer? _colorDelayShort;
    private CounterStrikeSharp.API.Modules.Timers.Timer? _colorDelayLong;

    public override void Load(bool hotReload)
    {
        if (_client.TryConnect())
        {
            BindApi();
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

        RegisterEventHandler<EventPlayerTeam>(OnPlayerTeam);
        RegisterEventHandler<EventPlayerSpawn>(OnPlayerSpawn);
        RegisterEventHandler<EventRoundStart>(OnRoundStart);
        RegisterListener<Listeners.OnClientDisconnect>(OnClientDisconnect);
        RegisterListener<Listeners.OnMapStart>(OnMapStartClearPopulationQueue);
    }

    public override void OnAllPluginsLoaded(bool hotReload)
    {
        if (_api == null) TryRegister();
        ScheduleTeammateColorReconcile();
    }

    private void TryRegister()
    {
        if (_client.TryConnect())
        {
            BindApi();
            Capabilities.RegisterPluginCapability(Capability, () => _api);
            Logger.LogInformation("[BotIdentityImpl] botidentity:api registered (late)");
        }
    }

    public override void Unload(bool hotReload)
    {
        KillColorDelayTimers();
        ClearPopulationQueue();
        RemoveListener<Listeners.OnMapStart>(OnMapStartClearPopulationQueue);
        _api = null;
        _apiImpl = null;
        _client.Dispose();
    }

    private void BindApi()
    {
        _apiImpl = new BotIdentityApiImpl(_client, Logger, this);
        _api = _apiImpl;
    }

    private void ScheduleTeammateColorReconcile()
    {
        Server.NextFrame(ReconcileTeammateColors);
        KillColorDelayTimers();
        _colorDelayShort = AddTimer(0.5f, () =>
        {
            _colorDelayShort = null;
            ReconcileTeammateColors();
        });
        _colorDelayLong = AddTimer(2.0f, () =>
        {
            _colorDelayLong = null;
            ReconcileTeammateColors();
        });
    }

    private void KillColorDelayTimers()
    {
        _colorDelayShort?.Kill();
        _colorDelayShort = null;
        _colorDelayLong?.Kill();
        _colorDelayLong = null;
    }
}

internal sealed partial class BotIdentityApiImpl : IBotIdentityApi
{
    private readonly SharedMemoryClient _client;
    private readonly ILogger _logger;
    private readonly BotIdentityImplPlugin _plugin;
    private readonly Random _random = new();

    public BotIdentityApiImpl(SharedMemoryClient client, ILogger logger, BotIdentityImplPlugin plugin)
    {
        _client = client;
        _logger = logger;
        _plugin = plugin;
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
