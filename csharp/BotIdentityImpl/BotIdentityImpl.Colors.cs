using CounterStrikeSharp.API;
using CounterStrikeSharp.API.Core;
using CounterStrikeSharp.API.Modules.Memory;
using CounterStrikeSharp.API.Modules.Utils;
using Microsoft.Extensions.Logging;

namespace BotIdentityImpl;

public sealed partial class BotIdentityImplPlugin
{
    private const int ColorUnset = -1;
    private const int ColorCount = 5;
    private const int MaxSlots = 64;
    private const int MaxForceWrites = 3;

    private readonly int[] _assignedColors = Enumerable.Repeat(ColorUnset, MaxSlots).ToArray();
    private readonly ulong[] _assignedIncarnations = new ulong[MaxSlots];
    private readonly byte[] _assignedTeams = new byte[MaxSlots];
    private readonly int[] _colorForceWrites = new int[MaxSlots];
    private readonly bool[] _colorGaveUp = new bool[MaxSlots];

    private HookResult OnPlayerTeam(EventPlayerTeam @event, GameEventInfo info)
    {
        ScheduleTeammateColorReconcile();
        return HookResult.Continue;
    }

    private HookResult OnPlayerSpawn(EventPlayerSpawn @event, GameEventInfo info)
    {
        ScheduleTeammateColorReconcile();
        return HookResult.Continue;
    }

    private HookResult OnRoundStart(EventRoundStart @event, GameEventInfo info)
    {
        ScheduleTeammateColorReconcile();
        return HookResult.Continue;
    }

    private void OnClientDisconnect(int slot)
    {
        if ((uint)slot >= MaxSlots)
            return;
        ClearColorLease(slot);
    }

    private void ClearColorLease(int slot)
    {
        _assignedColors[slot] = ColorUnset;
        _assignedIncarnations[slot] = 0;
        _assignedTeams[slot] = 0;
        _colorForceWrites[slot] = 0;
        _colorGaveUp[slot] = false;
    }

    private void ReconcileTeammateColors()
    {
        if (!_client.IsConnected)
            return;

        try
        {
            ReconcileTeamColors(CsTeam.Terrorist);
            ReconcileTeamColors(CsTeam.CounterTerrorist);
        }
        catch (Exception exception)
        {
            Logger.LogDebug(exception, "[BotIdentityImpl] teammate color reconcile failed");
        }
    }

    private void ReconcileTeamColors(CsTeam team)
    {
        var players = Utilities.GetPlayers();
        var humanColors = new HashSet<int>();
        var bots = new List<CCSPlayerController>();

        foreach (var player in players)
        {
            if (player is null || !player.IsValid || player.IsHLTV)
                continue;
            if ((CsTeam)player.TeamNum != team)
                continue;

            var slot = player.Slot;
            if ((uint)slot < MaxSlots && _client.IsManagedBot(slot))
            {
                bots.Add(player);
                continue;
            }

            if (player.IsBot || player.SteamID == 0)
                continue;

            AddTakenColor(humanColors, player.CompTeammateColor);
            AddTakenColor(humanColors, player.TeammatePreferredColor);
        }

        if (bots.Count == 0)
            return;

        bots.Sort((left, right) => left.Slot.CompareTo(right.Slot));
        var taken = new HashSet<int>(humanColors);

        foreach (var bot in bots)
        {
            var slot = bot.Slot;
            if ((uint)slot >= MaxSlots)
                continue;

            var incarnation = _client.GetSlotIncarnation(slot);
            var teamNum = bot.TeamNum;
            if (_assignedIncarnations[slot] != incarnation || _assignedTeams[slot] != teamNum)
            {
                _assignedColors[slot] = ColorUnset;
                _assignedIncarnations[slot] = incarnation;
                _assignedTeams[slot] = teamNum;
                _colorForceWrites[slot] = 0;
                _colorGaveUp[slot] = false;
            }

            var current = bot.CompTeammateColor;
            var valvePreferred = bot.TeammatePreferredColor;
            if (_colorGaveUp[slot])
            {
                ReserveObservedColor(taken, current, _assignedColors[slot]);
                continue;
            }

            var cached = _assignedColors[slot];
            var color = ColorUnset;
            if (IsFreeColor(valvePreferred, taken))
                color = valvePreferred;
            else if (IsFreeColor(current, taken))
                color = current;
            else if (IsFreeColor(cached, taken))
                color = cached;
            else
            {
                for (var i = 0; i < ColorCount; i++)
                {
                    if (taken.Contains(i))
                        continue;
                    color = i;
                    break;
                }
            }

            // Five competitive colors. A 6th teammate has no unique slot;
            // do not steal with slot%5 — that retriggers Valve color claims.
            if (color == ColorUnset)
            {
                _assignedColors[slot] = ColorUnset;
                continue;
            }

            taken.Add(color);
            _assignedColors[slot] = color;
            if (current == color)
                continue;

            if (_colorForceWrites[slot] >= MaxForceWrites)
            {
                _colorGaveUp[slot] = true;
                Logger.LogInformation(
                    "[BotIdentityImpl] teammate color write gave up slot={Slot} current={Current} preferred={Preferred} color={Color}",
                    slot,
                    current,
                    valvePreferred,
                    color);
                continue;
            }

            if (!TryWriteTeammateColor(bot, color))
                continue;

            _colorForceWrites[slot]++;
            Logger.LogInformation(
                "[BotIdentityImpl] assigned teammate color slot={Slot} current={Current} preferred={Preferred} color={Color}",
                slot,
                current,
                valvePreferred,
                color);
        }
    }

    private static void AddTakenColor(HashSet<int> taken, int color)
    {
        if (color >= 0 && color < ColorCount)
            taken.Add(color);
    }

    private static bool IsFreeColor(int color, HashSet<int> taken)
    {
        return color >= 0 && color < ColorCount && !taken.Contains(color);
    }

    private static void ReserveObservedColor(HashSet<int> taken, int current, int assigned)
    {
        if (IsFreeColor(current, taken))
            taken.Add(current);
        else if (IsFreeColor(assigned, taken))
            taken.Add(assigned);
    }

    private static bool TryWriteTeammateColor(CCSPlayerController player, int color)
    {
        try
        {
            Schema.SetSchemaValue(player.Handle, "CCSPlayerController", "m_iCompTeammateColor", color);
            Schema.SetSchemaValue(player.Handle, "CCSPlayerController", "m_iTeammatePreferredColor", color);
            Schema.SetSchemaValue(player.Handle, "CCSPlayerController", "m_bAttemptedToGetColor", true);
            Utilities.SetStateChanged(player, "CCSPlayerController", "m_iCompTeammateColor");
            return true;
        }
        catch (Exception)
        {
            return false;
        }
    }
}
