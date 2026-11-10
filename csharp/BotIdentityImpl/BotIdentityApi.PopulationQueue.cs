using CounterStrikeSharp.API;
using CounterStrikeSharp.API.Core;
using CounterStrikeSharp.API.Modules.Timers;
using Microsoft.Extensions.Logging;

namespace BotIdentityImpl;

internal enum PendingRemoveKind
{
    OneFromTeam,
    Slot,
    RemainingAll,
}

internal readonly struct PendingRemove
{
    public PendingRemoveKind Kind { get; init; }
    public int Team { get; init; }
    public int Slot { get; init; }
    public ulong Incarnation { get; init; }
}

public sealed partial class BotIdentityImplPlugin
{
    private const float PopulationFlushDelaySeconds = 0.3f;

    private readonly List<PendingRemove> _pendingRemoves = [];
    private bool _flushArmedThisRound;
    private bool _flushTimerScheduled;
    private CounterStrikeSharp.API.Modules.Timers.Timer? _flushTimer;

    internal void EnqueueOneFromTeam(int team)
    {
        _pendingRemoves.Add(new PendingRemove
        {
            Kind = PendingRemoveKind.OneFromTeam,
            Team = team,
        });
    }

    internal void EnqueueSlot(int slot, ulong incarnation)
    {
        _pendingRemoves.Add(new PendingRemove
        {
            Kind = PendingRemoveKind.Slot,
            Slot = slot,
            Incarnation = incarnation,
        });
    }

    internal void EnqueueRemainingAll()
    {
        foreach (var pending in _pendingRemoves)
        {
            if (pending.Kind == PendingRemoveKind.RemainingAll)
            {
                return;
            }
        }

        _pendingRemoves.Add(new PendingRemove { Kind = PendingRemoveKind.RemainingAll });
    }

    private void NotifyPopulationRoundStart()
    {
        if (_pendingRemoves.Count == 0)
        {
            return;
        }

        _flushArmedThisRound = true;
        _flushTimerScheduled = false;
        KillPopulationFlushTimer();

        if (!AnyUnmanagedHuman())
        {
            SchedulePopulationFlush();
        }
    }

    private void NotifyPopulationPlayerSpawn(EventPlayerSpawn @event)
    {
        if (!_flushArmedThisRound || _flushTimerScheduled)
        {
            return;
        }

        var player = @event.Userid;
        if (player == null || !player.IsValid || player.IsHLTV)
        {
            return;
        }

        if (_client.IsConnected && _client.IsManagedBot(player.Slot))
        {
            return;
        }

        SchedulePopulationFlush();
    }

    private void OnMapStartClearPopulationQueue(string mapName)
    {
        ClearPopulationQueue();
    }

    private void SchedulePopulationFlush()
    {
        if (_flushTimerScheduled)
        {
            return;
        }

        _flushTimerScheduled = true;
        _flushTimer = AddTimer(PopulationFlushDelaySeconds, OnPopulationFlushTimer, TimerFlags.STOP_ON_MAPCHANGE);
    }

    private void OnPopulationFlushTimer()
    {
        _flushTimer = null;
        FlushPendingRemoves();
    }

    private void FlushPendingRemoves()
    {
        _flushArmedThisRound = false;
        _flushTimerScheduled = false;

        if (_pendingRemoves.Count == 0)
        {
            return;
        }

        var batch = _pendingRemoves.ToArray();
        _pendingRemoves.Clear();

        if (_apiImpl == null)
        {
            Logger.LogInformation(
                "[BotIdentityImpl] dropped {Count} deferred removals: api unbound",
                batch.Length);
            return;
        }

        Logger.LogInformation("[BotIdentityImpl] flushing {Count} deferred removals", batch.Length);

        foreach (var item in batch)
        {
            switch (item.Kind)
            {
                case PendingRemoveKind.OneFromTeam:
                    _apiImpl.RemoveOneFromTeam(item.Team, allowDefer: false);
                    break;
                case PendingRemoveKind.Slot:
                    _apiImpl.RemoveQueuedSlot(item.Slot, item.Incarnation);
                    break;
                case PendingRemoveKind.RemainingAll:
                    _apiImpl.RemoveAll(allowDefer: false);
                    break;
            }
        }
    }

    private void ClearPopulationQueue()
    {
        KillPopulationFlushTimer();
        _flushArmedThisRound = false;
        _flushTimerScheduled = false;
        if (_pendingRemoves.Count > 0)
        {
            Logger.LogInformation(
                "[BotIdentityImpl] cleared {Count} deferred removals",
                _pendingRemoves.Count);
        }

        _pendingRemoves.Clear();
    }

    private void KillPopulationFlushTimer()
    {
        _flushTimer?.Kill();
        _flushTimer = null;
    }

    private bool AnyUnmanagedHuman()
    {
        foreach (var player in Utilities.GetPlayers())
        {
            if (player == null || !player.IsValid || player.IsHLTV)
            {
                continue;
            }

            if (_client.IsConnected && _client.IsManagedBot(player.Slot))
            {
                continue;
            }

            return true;
        }

        return false;
    }
}
