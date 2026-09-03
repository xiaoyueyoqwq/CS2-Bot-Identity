// BotIdentity — minimal C++ Metamod plugin
// Converts Native bots to Fake players on connect,
// so CounterStrikeSharp C# plugins can apply Schema-based changes.

#include "plugin.h"
#include "bot_info.h"
#include "ssc_ops.h"
#include "entity_access.h"
#include "shm_pub.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

PLUGIN_GLOBALVARS();

SH_DECL_HOOK6_void(IServerGameClients, OnClientConnected, SH_NOATTRIB, 0,
    CPlayerSlot, const char*, uint64, const char*, const char*, bool);
SH_DECL_HOOK4_void(IServerGameClients, ClientPutInServer, SH_NOATTRIB, 0,
    CPlayerSlot, const char*, int, uint64);
SH_DECL_HOOK5_void(IServerGameClients, ClientDisconnect, SH_NOATTRIB, 0,
    CPlayerSlot, ENetworkDisconnectionReason, const char*, uint64, const char*);

PLUGIN_GLOBALVARS();
BotIdentityPlugin g_BotIdentityPlugin;
PLUGIN_EXPOSE(BotIdentityPlugin, g_BotIdentityPlugin);

IVEngineServer*     engine      = nullptr;
IServerGameClients* gameclients = nullptr;

static bool s_PluginActive = false;

namespace botid {

static void ApplyDisguise(int slot, BotIdentity* identity) {
    if (!identity || identity->applied) return;

    void* client = ResolveClientBySlot(slot);
    if (!client) return;

    int entityIndex = GetEntityIndex(client);

    // Step 1: Clear CServerSideClient::m_bFakePlayer (set bit pattern: 0x01 mask)
    ClearFakePlayer(client);
    // Step 2: Write synthetic SteamID
    WriteSteamId(client, identity->steamId);

    // Step 3: Try to find controller and clear its FakeClientFlags too
    char className[64] = {0};
    void* controller = botid::ResolveEntityInstance(entityIndex, className, sizeof(className));
    if (controller && std::strcmp(className, "cs_player_controller") == 0) {
        ClearControllerFakeClientFlag(controller);
        MarkEntityStateChanged(controller);
    }

    identity->slot = slot;
    identity->applied = true;

    // Notify Bot-Improver C# plugins via shared memory
    botid::PublishAdopt(slot, identity->steamId, identity->name.c_str());
    botid::BumpIncarnation(slot);
    if (identity->ping > 0) {
        botid::PublishPing(slot, identity->ping);
    }
    if (!identity->crosshair.empty()) {
        botid::PublishCrosshair(slot, identity->crosshair.c_str());
    }
    if (identity->scoreboardFlair > 0) {
        botid::PublishScoreboardFlair(slot, identity->scoreboardFlair);
    }

    META_CONPRINTF("[BotIdentity] disguised slot=%d name='%s' steamid=%llu controller=%s\n",
                   slot, identity->name.c_str(), identity->steamId,
                   controller ? "ok" : "deferred");
}

static void RestoreIdentity(int slot) {
    void* client = ResolveClientBySlot(slot);
    if (!client) return;

    int entityIndex = GetEntityIndex(client);
    char className[64] = {0};
    void* controller = botid::ResolveEntityInstance(entityIndex, className, sizeof(className));

    if (controller && std::strcmp(className, "cs_player_controller") == 0) {
        SetControllerFakeClientFlag(controller);
        MarkEntityStateChanged(controller);
    }
    SetFakePlayer(client);
    WriteSteamId(client, 0);

    // Notify Bot-Improver C# plugins
    botid::PublishRelease(slot);
}

}  // namespace botid

void BotIdentityPlugin::Hook_OnClientConnected_Post(
    CPlayerSlot slot, const char* pszName, uint64 /*xuid*/,
    const char* /*pszNetworkID*/, const char* /*pszAddress*/, bool bFakePlayer)
{
    int slotIdx = slot.Get();
    if (!s_PluginActive) RETURN_META(MRES_IGNORED);
    if (!bFakePlayer) RETURN_META(MRES_IGNORED);

    if (slotIdx < 0 || slotIdx >= botid::kMaxSlots) RETURN_META(MRES_IGNORED);
    if (pszName && std::strncmp(pszName, "HLTV", 4) == 0) RETURN_META(MRES_IGNORED);

    botid::BotIdentity* identity = botid::BotInfos().GetFree();
    if (!identity) {
        META_CONPRINTF("[BotIdentity] OnClientConnected no free identity slot=%d\n", slotIdx);
        RETURN_META(MRES_IGNORED);
    }

    // Find index in vector
    int botIndex = -1;
    for (int i = 0; i < botid::BotInfos().Count(); ++i) {
        if (botid::BotInfos().GetByIndex(i) == identity) { botIndex = i; break; }
    }
    if (botIndex < 0) RETURN_META(MRES_IGNORED);
    botid::IdentityMgr().Mark(slotIdx, botIndex);
    botid::ApplyDisguise(slotIdx, identity);

    RETURN_META(MRES_IGNORED);
}

void BotIdentityPlugin::Hook_ClientPutInServer_Post(
    CPlayerSlot slot, const char* /*pszName*/, int /*type*/, uint64 /*xuid*/)
{
    if (!s_PluginActive) RETURN_META(MRES_IGNORED);

    int slotIdx = slot.Get();
    if (slotIdx < 0 || slotIdx >= botid::kMaxSlots) RETURN_META(MRES_IGNORED);
    if (!botid::IdentityMgr().IsManaged(slotIdx)) RETURN_META(MRES_IGNORED);

    botid::BotIdentity* identity = botid::IdentityMgr().GetIdentity(slotIdx);
    if (!identity || identity->applied) RETURN_META(MRES_IGNORED);

    // Retry disguise — entity controller should exist now
    botid::ApplyDisguise(slotIdx, identity);

    RETURN_META(MRES_IGNORED);
}

void BotIdentityPlugin::Hook_ClientDisconnect_Pre(
    CPlayerSlot slot, ENetworkDisconnectionReason /*reason*/, const char* /*pszName*/,
    uint64 /*xuid*/, const char* /*pszNetworkID*/)
{
    if (!s_PluginActive) RETURN_META(MRES_IGNORED);

    int slotIdx = slot.Get();
    if (slotIdx < 0 || slotIdx >= botid::kMaxSlots) RETURN_META(MRES_IGNORED);
    if (!botid::IdentityMgr().IsManaged(slotIdx)) RETURN_META(MRES_IGNORED);

    botid::RestoreIdentity(slotIdx);
    botid::IdentityMgr().Unmark(slotIdx);

    RETURN_META(MRES_IGNORED);
}

bool BotIdentityPlugin::Load(PluginId id, ISmmAPI* ismm, char* error, size_t maxlen, bool late) {
    PLUGIN_SAVEVARS();

    GET_V_IFACE_CURRENT(GetEngineFactory, engine, IVEngineServer, INTERFACEVERSION_VENGINESERVER);
    GET_V_IFACE_ANY(GetServerFactory, gameclients, IServerGameClients, INTERFACEVERSION_SERVERGAMECLIENTS);
    GET_V_IFACE_ANY(GetEngineFactory, g_pNetworkServerService, INetworkServerService, NETWORKSERVERSERVICE_INTERFACE_VERSION);

    // Resolve GameResourceService for entity lookups
    void* gameResourceService = ismm->GetEngineFactory(false)("GameResourceServiceServerV001", nullptr);
    botid::SetGameResourceServicePtr(gameResourceService);
    META_CONPRINTF("[BotIdentity] gameResourceService=%p\n", gameResourceService);

    // Initialize shared memory for Bot-Improver C# integration
    if (!botid::InitSharedMemory()) {
        ismm->ConPrintf("[BotIdentity] warning: shared memory init failed\n");
    }

    std::string gamedataPath = ismm->GetBaseDir();
    gamedataPath += "/addons/BotIdentity/gamedata.json";
    botid::LoadGamedata(gamedataPath.c_str());

    std::string configPath = ismm->GetBaseDir();
    configPath += "/addons/BotIdentity/bot_info.json";
    if (!botid::BotInfos().Load(configPath.c_str())) {
        ismm->ConPrintf("[BotIdentity] warning: bot_info.json not found at %s\n", configPath.c_str());
    }

    ismm->ConPrintf("[BotIdentity] loaded bot_count=%d\n", botid::BotInfos().Count());

    SH_ADD_HOOK(IServerGameClients, OnClientConnected, gameclients,
        SH_MEMBER(this, &BotIdentityPlugin::Hook_OnClientConnected_Post), true);
    SH_ADD_HOOK(IServerGameClients, ClientPutInServer, gameclients,
        SH_MEMBER(this, &BotIdentityPlugin::Hook_ClientPutInServer_Post), true);
    SH_ADD_HOOK(IServerGameClients, ClientDisconnect, gameclients,
        SH_MEMBER(this, &BotIdentityPlugin::Hook_ClientDisconnect_Pre), false);

    META_CONPRINTF("[BotIdentity] hooks installed: gameclients=%p\n", (void*)gameclients);

    s_PluginActive = true;
    return true;
}

bool BotIdentityPlugin::Unload(char* error, size_t maxlen) {
    s_PluginActive = false;

    SH_REMOVE_HOOK(IServerGameClients, OnClientConnected, gameclients,
        SH_MEMBER(this, &BotIdentityPlugin::Hook_OnClientConnected_Post), true);
    SH_REMOVE_HOOK(IServerGameClients, ClientPutInServer, gameclients,
        SH_MEMBER(this, &BotIdentityPlugin::Hook_ClientPutInServer_Post), true);
    SH_REMOVE_HOOK(IServerGameClients, ClientDisconnect, gameclients,
        SH_MEMBER(this, &BotIdentityPlugin::Hook_ClientDisconnect_Pre), false);

    botid::ShutdownSharedMemory();
    META_CONPRINTF("[BotIdentity] unloaded\n");
    return true;
}

void BotIdentityPlugin::AllPluginsLoaded() {
}
