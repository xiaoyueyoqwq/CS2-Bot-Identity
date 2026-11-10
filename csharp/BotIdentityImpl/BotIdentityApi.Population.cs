using BotIdentityApi;
using CounterStrikeSharp.API;
using CounterStrikeSharp.API.Core;
using Microsoft.Extensions.Logging;

namespace BotIdentityImpl;

internal sealed partial class BotIdentityApiImpl
{
    private const int TeamTerrorist = 2;
    private const int TeamCounterTerrorist = 3;

    public BotPopulationResult TryAddBot(int team)
    {
        if (!IsConnected)
        {
            return BotPopulationResult.NotConnected;
        }

        if (!TryTeamCommand(team, out var commandTeam))
        {
            return BotPopulationResult.InvalidTeam;
        }

        // Native DispatchConCommand_Pre already closes the kick/vote
        // window so disguise can run during this bot_add.
        _logger.LogInformation("[BotIdentityImpl] TryAddBot team={Team}", commandTeam);
        Server.ExecuteCommand($"bot_add {commandTeam}");
        return BotPopulationResult.Ok;
    }

    public BotPopulationResult TryRemoveOneFromTeam(int team)
    {
        return RemoveOneFromTeam(team, allowDefer: true);
    }

    internal BotPopulationResult RemoveOneFromTeam(int team, bool allowDefer)
    {
        if (!IsConnected)
        {
            return BotPopulationResult.NotConnected;
        }

        if (!TryTeamCommand(team, out _))
        {
            return BotPopulationResult.InvalidTeam;
        }

        var safe = new List<CCSPlayerController>();
        var sawManaged = false;
        var sawControlled = false;
        foreach (var player in Utilities.GetPlayers())
        {
            if (!player.IsValid || (int)player.Team != team || !IsManagedBot(player.Slot))
            {
                continue;
            }

            sawManaged = true;
            if (IsBeingTakenOver(player))
            {
                sawControlled = true;
                continue;
            }

            if (SanitizeKickName(KickName(player)).Length == 0)
            {
                continue;
            }

            safe.Add(player);
        }

        // Global takeover with zero identified bots anywhere: this
        // team's remaining bots cannot be proven safe. If another
        // team's bot is already identified, this team stays kickable.
        if (HasAnyTakeoverSignal() && !sawControlled && sawManaged && !AnyIdentifiedTakenOver())
        {
            sawControlled = true;
            safe.Clear();
            _logger.LogInformation(
                "[BotIdentityImpl] TryRemove team={Team} unidentified takeover; treating remaining bots as taken over",
                team);
        }

        if (safe.Count > 0)
        {
            return KickManaged(safe[_random.Next(safe.Count)]);
        }

        if (!sawManaged)
        {
            return BotPopulationResult.NoBot;
        }

        if (sawControlled && allowDefer)
        {
            _logger.LogInformation(
                "[BotIdentityImpl] TryRemove team={Team} deferred: every remaining bot is being taken over",
                team);
            return BotPopulationResult.Deferred;
        }

        return sawControlled
            ? BotPopulationResult.BeingControlled
            : BotPopulationResult.NoBot;
    }

    public BotPopulationResult TryRemoveSlot(int slot)
    {
        return RemoveSlot(slot, allowDefer: true);
    }

    internal BotPopulationResult RemoveQueuedSlot(int slot, ulong incarnation)
    {
        if (!IsManagedBotIncarnation(slot, incarnation))
        {
            _logger.LogInformation(
                "[BotIdentityImpl] deferred slot={Slot} incarnation={Incarnation} dropped: slot reused or unmanaged",
                slot,
                incarnation);
            return BotPopulationResult.NotManaged;
        }

        return RemoveSlot(slot, allowDefer: false);
    }

    private BotPopulationResult RemoveSlot(int slot, bool allowDefer)
    {
        if (!IsConnected)
        {
            return BotPopulationResult.NotConnected;
        }

        if (!IsManagedBot(slot))
        {
            return BotPopulationResult.NotManaged;
        }

        var player = Utilities.GetPlayers().FirstOrDefault(candidate =>
            candidate.IsValid && candidate.Slot == slot);
        if (player == null)
        {
            return BotPopulationResult.NoBot;
        }

        if (IsBeingTakenOver(player))
        {
            if (allowDefer)
            {
                _logger.LogInformation(
                    "[BotIdentityImpl] TryRemove slot={Slot} deferred: being taken over",
                    slot);
                return BotPopulationResult.Deferred;
            }

            _logger.LogInformation(
                "[BotIdentityImpl] deferred slot={Slot} dropped: still being taken over",
                slot);
            return BotPopulationResult.BeingControlled;
        }

        return KickManaged(player);
    }

    public BotRemoveAllResult TryRemoveAll()
    {
        return RemoveAll(allowDefer: true);
    }

    internal BotRemoveAllResult RemoveAll(bool allowDefer)
    {
        if (!IsConnected)
        {
            return new BotRemoveAllResult { Result = BotPopulationResult.NotConnected };
        }

        var takeoverSignal = HasAnyTakeoverSignal();
        var safe = new List<CCSPlayerController>();
        var skippedControlled = 0;
        foreach (var player in Utilities.GetPlayers().OrderByDescending(player => player.Slot))
        {
            if (!player.IsValid || !IsManagedBot(player.Slot))
            {
                continue;
            }

            if (IsBeingTakenOver(player))
            {
                skippedControlled++;
                continue;
            }

            if (SanitizeKickName(KickName(player)).Length == 0)
            {
                continue;
            }

            safe.Add(player);
        }

        var unidentified = takeoverSignal && skippedControlled == 0 && safe.Count > 0;
        if (unidentified)
        {
            if (allowDefer)
            {
                _logger.LogInformation(
                    "[BotIdentityImpl] TryRemoveAll removed={Removed} skippedControlled={Skipped} unidentifiedTakeover={Unidentified}",
                    0,
                    safe.Count,
                    true);
                return new BotRemoveAllResult
                {
                    Removed = 0,
                    SkippedControlled = safe.Count,
                    Result = BotPopulationResult.Deferred,
                };
            }

            _logger.LogInformation(
                "[BotIdentityImpl] deferred remaining-all dropped: unidentifiedTakeover stillControlled={Count}",
                safe.Count);
            return new BotRemoveAllResult
            {
                Removed = 0,
                SkippedControlled = safe.Count,
                Result = BotPopulationResult.BeingControlled,
            };
        }

        var removed = 0;
        foreach (var player in safe)
        {
            if (KickManaged(player) == BotPopulationResult.Ok)
            {
                removed++;
            }
        }

        if (skippedControlled > 0 && allowDefer)
        {
            _logger.LogInformation(
                "[BotIdentityImpl] TryRemoveAll removed={Removed} skippedControlled={Skipped} unidentifiedTakeover={Unidentified}",
                removed,
                skippedControlled,
                false);
            return new BotRemoveAllResult
            {
                Removed = removed,
                SkippedControlled = skippedControlled,
                Result = removed > 0 ? BotPopulationResult.Ok : BotPopulationResult.Deferred,
            };
        }

        if (skippedControlled > 0 && !allowDefer)
        {
            _logger.LogInformation(
                "[BotIdentityImpl] deferred remaining-all dropped: stillControlled={Count}",
                skippedControlled);
        }
        else
        {
            _logger.LogInformation(
                "[BotIdentityImpl] TryRemoveAll removed={Removed} skippedControlled={Skipped} unidentifiedTakeover={Unidentified}",
                removed,
                skippedControlled,
                false);
        }

        return new BotRemoveAllResult
        {
            Removed = removed,
            SkippedControlled = skippedControlled,
            Result = removed > 0
                ? BotPopulationResult.Ok
                : skippedControlled > 0
                    ? BotPopulationResult.BeingControlled
                    : BotPopulationResult.NoBot,
        };
    }

    private BotPopulationResult KickManaged(CCSPlayerController player)
    {
        var kickName = SanitizeKickName(KickName(player));
        if (kickName.Length == 0)
        {
            return BotPopulationResult.Failed;
        }

        _logger.LogInformation(
            "[BotIdentityImpl] TryRemove slot={Slot} name={Name}",
            player.Slot,
            kickName);
        Server.ExecuteCommand($"bot_kick \"{kickName}\"");
        return BotPopulationResult.Ok;
    }

    private string KickName(CCSPlayerController player)
    {
        try
        {
            var persona = GetPersonaName(player.Slot);
            if (!string.IsNullOrWhiteSpace(persona))
            {
                return persona;
            }
        }
        catch
        {
            // Fall back to the scoreboard name.
        }

        return player.PlayerName ?? string.Empty;
    }

    private bool HasAnyTakeoverSignal()
    {
        foreach (var player in Utilities.GetPlayers())
        {
            if (player == null || !player.IsValid || player.IsHLTV)
            {
                continue;
            }

            try
            {
                if (player.ControllingBot)
                {
                    return true;
                }
            }
            catch
            {
                return true;
            }

            if (IsManagedBot(player.Slot))
            {
                continue;
            }

            try
            {
                var original = ControllerOfCurrentPawn(player);
                if (original != null && IsManagedBot(original.Slot))
                {
                    return true;
                }
            }
            catch
            {
                return true;
            }
        }

        return false;
    }

    private bool AnyIdentifiedTakenOver()
    {
        foreach (var player in Utilities.GetPlayers())
        {
            if (player == null || !player.IsValid || !IsManagedBot(player.Slot))
            {
                continue;
            }

            if (IsBeingTakenOver(player))
            {
                return true;
            }
        }

        return false;
    }

    private bool IsBeingTakenOver(CCSPlayerController bot)
    {
        try
        {
            // Inverted takeover: a disguised bot controller is driving a human.
            if (bot.ControllingBot)
            {
                return true;
            }

            var botPawn = CurrentPawn(bot);
            var botPawnValid = botPawn != null;
            var sawHumanTakeover = false;

            foreach (var other in Utilities.GetPlayers())
            {
                if (!other.IsValid || other.Slot == bot.Slot || IsManagedBot(other.Slot))
                {
                    continue;
                }

                bool otherControlling;
                try
                {
                    otherControlling = other.ControllingBot;
                }
                catch
                {
                    return true;
                }

                if (otherControlling)
                {
                    sawHumanTakeover = true;
                }

                var otherPawn = CurrentPawn(other);
                var original = ControllerOfCurrentPawn(other);
                if (original != null && original.Slot == bot.Slot)
                {
                    return true;
                }

                var pawnOriginal = PawnOriginalController(otherPawn);
                if (pawnOriginal != null && pawnOriginal.Slot == bot.Slot)
                {
                    return true;
                }

                var occupying = PawnController(botPawn);
                if (occupying != null && occupying.Slot == other.Slot)
                {
                    return true;
                }

                if (botPawnValid && otherPawn != null && otherPawn.Index == botPawn!.Index)
                {
                    return true;
                }
            }

            // Pawn unreadable while a human is in takeover: cannot prove
            // this bot is not the possessed one, so do not kick it.
            if (!botPawnValid && sawHumanTakeover)
            {
                return true;
            }
        }
        catch
        {
            return true;
        }

        return false;
    }

    private static CCSPlayerPawn? CurrentPawn(CCSPlayerController player)
    {
        try
        {
            if (player.Pawn?.Value is CCSPlayerPawn pawn && pawn.IsValid)
            {
                return pawn;
            }
        }
        catch
        {
            // Fall through to PlayerPawn.
        }

        try
        {
            var playerPawn = player.PlayerPawn?.Value;
            if (playerPawn != null && playerPawn.IsValid)
            {
                return playerPawn;
            }
        }
        catch
        {
            // Unreadable pawn is handled by the caller.
        }

        return null;
    }

    private static CCSPlayerController? ControllerOfCurrentPawn(CCSPlayerController player)
    {
        var original = player.OriginalControllerOfCurrentPawn.Value;
        return original != null && original.IsValid ? original : null;
    }

    private static CCSPlayerController? PawnOriginalController(CCSPlayerPawn? pawn)
    {
        if (pawn == null)
        {
            return null;
        }

        var original = pawn.OriginalController.Value;
        return original != null && original.IsValid ? original : null;
    }

    private static CCSPlayerController? PawnController(CCSPlayerPawn? pawn)
    {
        if (pawn == null)
        {
            return null;
        }

        return pawn.Controller?.Value is CCSPlayerController controller && controller.IsValid
            ? controller
            : null;
    }

    private static bool TryTeamCommand(int team, out string commandTeam)
    {
        switch (team)
        {
            case TeamTerrorist:
                commandTeam = "t";
                return true;
            case TeamCounterTerrorist:
                commandTeam = "ct";
                return true;
            default:
                commandTeam = string.Empty;
                return false;
        }
    }

    private static string SanitizeKickName(string? name)
    {
        if (string.IsNullOrWhiteSpace(name))
        {
            return string.Empty;
        }

        return name.Replace("\"", string.Empty)
            .Replace(";", string.Empty)
            .Replace("\n", string.Empty)
            .Replace("\r", string.Empty)
            .Trim();
    }
}
