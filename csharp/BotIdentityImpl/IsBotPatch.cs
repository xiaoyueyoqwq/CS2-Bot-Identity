using CounterStrikeSharp.API.Core;
using HarmonyLib;

namespace BotIdentityImpl;

// Harmony postfix on CCSPlayerController.get_IsBot. Disguise clears the
// controller's FL_FAKECLIENT flag, so managed bots read IsBot=false and
// plugins that detect bots via IsBot (BotRandomizer, BotBuy, ...) skip
// them. This patch reports managed slots as bots again — replacement for
// the retired BotHiderImpl patch.
[HarmonyPatch(typeof(CCSPlayerController), nameof(CCSPlayerController.IsBot), MethodType.Getter)]
internal static class IsBotPatch
{
    internal static SharedMemoryClient? Client;

    private static void Postfix(CCSPlayerController __instance, ref bool __result)
    {
        if (__result) return;
        var client = Client;
        if (client is null || !client.HasView) return;
        try
        {
            var slot = __instance.Slot;
            if (slot >= 0 && client.IsManagedBot(slot)) __result = true;
        }
        catch
        {
        }
    }
}
