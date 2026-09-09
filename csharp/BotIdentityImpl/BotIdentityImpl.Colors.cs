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

    private readonly int[] _assignedColors = Enumerable.Repeat(ColorUnset, MaxSlots).ToArray();
    private readonly ulong[] _assignedIncarnations = new ulong[MaxSlots];

    private HookResult OnPlayerTeam(EventPlayerTeam @event, GameEventInfo info)
    {
        Server.NextFrame(ReconcileTeammateColors);
        return HookResult.Continue;
    }

    private HookResult OnPlayerSpawn(EventPlayerSpawn @event, GameEventInfo info)
    {
        Server.NextFrame(ReconcileTeammateColors);
        return HookResult.Continue;
    }

    private HookResult OnRoundStart(EventRoundStart @event, GameEventInfo info)
    {
        Server.NextFrame(ReconcileTeammateColors);
        return HookResult.Continue;
    }

    private void OnClientDisconnect(int slot)
    {
        if ((uint)slot >= MaxSlots)
            return;
        _assignedColors[slot] = ColorUnset;
        _assignedIncarnations[slot] = 0;
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

            var humanColor = player.CompTeammateColor;
            if (humanColor >= 0 && humanColor < ColorCount)
                humanColors.Add(humanColor);
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
            if (_assignedIncarnations[slot] != incarnation)
            {
                _assignedColors[slot] = ColorUnset;
                _assignedIncarnations[slot] = incarnation;
            }

            var current = bot.CompTeammateColor;
            var preferred = _assignedColors[slot];
            var color = ColorUnset;
            if (current >= 0 && current < ColorCount && !taken.Contains(current))
                color = current;
            else if (preferred >= 0 && preferred < ColorCount && !taken.Contains(preferred))
                color = preferred;
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
            // do not steal with slot%5 — that fights the 2s timer forever.
            if (color == ColorUnset)
            {
                _assignedColors[slot] = ColorUnset;
                continue;
            }

            taken.Add(color);
            _assignedColors[slot] = color;
            if (current == color)
                continue;

            if (!TryWriteTeammateColor(bot, color))
                continue;

            Logger.LogInformation(
                "[BotIdentityImpl] assigned teammate color slot={Slot} color={Color}",
                slot,
                color);
        }
    }

    private static bool TryWriteTeammateColor(CCSPlayerController player, int color)
    {
        try
        {
            Schema.SetSchemaValue(player.Handle, "CCSPlayerController", "m_iCompTeammateColor", color);
            Schema.SetSchemaValue(player.Handle, "CCSPlayerController", "m_iTeammatePreferredColor", color);
            Schema.SetSchemaValue(player.Handle, "CCSPlayerController", "m_bAttemptedToGetColor", true);
            Utilities.SetStateChanged(player, "CCSPlayerController", "m_iCompTeammateColor");
            Utilities.SetStateChanged(player, "CCSPlayerController", "m_iTeammatePreferredColor");
            Utilities.SetStateChanged(player, "CCSPlayerController", "m_bAttemptedToGetColor");
            return true;
        }
        catch (Exception)
        {
            return false;
        }
    }
}
