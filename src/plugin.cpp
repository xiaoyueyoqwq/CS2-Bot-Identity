// BotIdentity — minimal C++ Metamod plugin
// Converts Native bots to Fake players on connect,
// so CounterStrikeSharp C# plugins can apply Schema-based changes.

#include "plugin.h"
#include "bot_info.h"
#include "entity_access.h"
#include "shm_pub.h"
#include "ssc_ops.h"
#include "vote_transaction.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

PLUGIN_GLOBALVARS();

SH_DECL_HOOK6_void(IServerGameClients, OnClientConnected, SH_NOATTRIB, 0,
    CPlayerSlot, const char*, uint64, const char*, const char*, bool);
SH_DECL_HOOK4_void(IServerGameClients, ClientPutInServer, SH_NOATTRIB, 0,
    CPlayerSlot, const char*, int, uint64);
SH_DECL_HOOK5_void(IServerGameClients, ClientDisconnect, SH_NOATTRIB, 0,
    CPlayerSlot, ENetworkDisconnectionReason, const char*, uint64, const char*);
SH_DECL_HOOK3_void(IServerGameDLL, GameFrame, SH_NOATTRIB, 0, bool, bool, bool);
SH_DECL_HOOK3_void(ICvar, DispatchConCommand, SH_NOATTRIB, 0, ConCommandRef, const CCommandContext&, const CCommand&);

PLUGIN_GLOBALVARS();
BotIdentityPlugin g_BotIdentityPlugin;
PLUGIN_EXPOSE(BotIdentityPlugin, g_BotIdentityPlugin);

IVEngineServer*     engine      = nullptr;
IServerGameClients* gameclients = nullptr;
IServerGameDLL*     gameserver  = nullptr;
ICvar*              icvar       = nullptr;

static bool s_PluginActive = false;

namespace botid {

static int PickFakePing(const BotIdentity* identity) {
    const auto& f = BotInfos().Features();
    if (!f.enableFakePing) return 0;
    // If features provides an explicit base via per-bot "ping", prefer it
    if (identity->ping > 0 && identity->ping < f.fakePingMin)
        return identity->ping;  // small explicit override (e.g. 18)
    if (f.fakePingMax <= f.fakePingMin) return f.fakePingMin;
    return f.fakePingMin + (rand() % (f.fakePingMax - f.fakePingMin + 1));
}

static uint32_t PickScoreboardFlair(const BotIdentity* identity) {
    const auto& f = BotInfos().Features();
    if (!f.enableScoreboardFlair) return 0;
    if (f.scoreboardFlairProbability < 1.0 && (rand() % 1000) >= int(f.scoreboardFlairProbability * 1000.0))
        return 0;  // probability gate failed
    if (identity->scoreboardFlair > 0) return identity->scoreboardFlair;
    return f.defaultScoreboardFlair;  // 0 means no flair
}

static void ApplyDisguise(int slot, BotIdentity* identity) {
    if (!identity || identity->applied) return;

    void* client = ResolveClientBySlot(slot);
    if (!client) return;

    int entityIndex = GetEntityIndex(client);

    // If this identity was recycled, derive a slot-unique SteamID
    // so bots reusing the same name still get distinct SteamIDs.
    if (identity->reused > 0) {
        // Keep the upper 48 bits from the config SteamID, overwrite the
        // lower 16 bits with (reused * 64 + slot). This stays inside the
        // 48-bit account-id range and is deterministic.
        uint64_t base = identity->steamId & 0xFFFFFFFFFFFF0000ULL;
        uint64_t suffix = (static_cast<uint64_t>(identity->reused) * 64u + static_cast<uint64_t>(slot)) & 0xFFFFu;
        identity->steamId = base | suffix;
    }

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
    const auto& f = BotInfos().Features();
    botid::PublishAdopt(slot, identity->steamId, identity->name.c_str());
    botid::BumpIncarnation(slot);
    if (f.enableFakePing) {
        int p = PickFakePing(identity);
        if (p > 0) botid::PublishPing(slot, p);
    }
    if (f.enableCrosshair && !identity->crosshair.empty()) {
        botid::PublishCrosshair(slot, identity->crosshair.c_str());
    }
    uint32_t flair = PickScoreboardFlair(identity);
    if (flair > 0) {
        botid::PublishScoreboardFlair(slot, flair);
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
    CPlayerSlot slot, const char* pszName, uint64 xuid,
    const char* /*pszNetworkID*/, const char* /*pszAddress*/, bool bFakePlayer)
{
    int slotIdx = slot.Get();
    if (!s_PluginActive) RETURN_META(MRES_IGNORED);
    if (slotIdx < 0 || slotIdx >= botid::kMaxSlots) RETURN_META(MRES_IGNORED);
    if (pszName && std::strncmp(pszName, "HLTV", 4) == 0) RETURN_META(MRES_IGNORED);

    // If this slot was previously managed, ALWAYS release the shm state
    // before we determine whether to re-disguise. This prevents stale
    // entries from surviving across bot→real-player slot reassignments.
    if (botid::IdentityMgr().IsManaged(slotIdx)) {
        botid::IdentityMgr().Unmark(slotIdx);
        botid::PublishRelease(slotIdx);
        botid::ResetVoteTransactionSlot(slotIdx);
    }

    // Real players have non-zero xuid; bots have xuid=0
    if (xuid != 0) RETURN_META(MRES_IGNORED);
    // Also skip real players by bFakePlayer (belt-and-suspenders)
    if (!bFakePlayer) RETURN_META(MRES_IGNORED);

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

    // A vote transaction may hold a snapshot for this slot; drop it before
    // the entity goes away so the restore path cannot touch freed objects.
    botid::ResetVoteTransactionSlot(slotIdx);

    botid::RestoreIdentity(slotIdx);
    botid::IdentityMgr().Unmark(slotIdx);

    RETURN_META(MRES_IGNORED);
}

bool BotIdentityPlugin::Load(PluginId id, ISmmAPI* ismm, char* error, size_t maxlen, bool late) {
    PLUGIN_SAVEVARS();
    ismm_ = ismm;

    GET_V_IFACE_CURRENT(GetEngineFactory, engine, IVEngineServer, INTERFACEVERSION_VENGINESERVER);
    GET_V_IFACE_CURRENT(GetEngineFactory, icvar, ICvar, CVAR_INTERFACE_VERSION);

    // tier1/convar.cpp (compiled into this .so) dereferences its own g_pCVar
    // global inside ConCommandRef::GetName()/GetRawData(); it must be set
    // before the DispatchConCommand hooks below can safely run.
    g_pCVar = icvar;
    GET_V_IFACE_ANY(GetServerFactory, gameclients, IServerGameClients, INTERFACEVERSION_SERVERGAMECLIENTS);
    GET_V_IFACE_ANY(GetEngineFactory, gameserver, IServerGameDLL, INTERFACEVERSION_SERVERGAMEDLL);
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

    std::string baseDir = ismm->GetBaseDir();

    std::string featuresPath = baseDir + "/addons/BotIdentity/config.json";
    botid::BotInfos().LoadFeatures(featuresPath.c_str());

    std::string botsPath = baseDir + "/addons/BotIdentity/bots.json";
    botid::BotInfos().LoadBots(botsPath.c_str());

    ismm->ConPrintf("[BotIdentity] loaded version=%s bot_count=%d fakePing=%d-%d jitter=%d%% flair=%.0f%% voteHoldFrames=%d ctrlSteamIdWrite=scan\n",
                    GetVersion(),
                    botid::BotInfos().Count(),
                    botid::BotInfos().Features().fakePingMin,
                    botid::BotInfos().Features().fakePingMax,
                    botid::BotInfos().Features().pingJitterPercent,
                    botid::BotInfos().Features().scoreboardFlairProbability * 100.0,
                    botid::BotInfos().Features().voteTransactionHoldFrames);

    srand(static_cast<unsigned int>(time(nullptr)));

    SH_ADD_HOOK(IServerGameClients, OnClientConnected, gameclients,
        SH_MEMBER(this, &BotIdentityPlugin::Hook_OnClientConnected_Post), true);
    SH_ADD_HOOK(IServerGameClients, ClientPutInServer, gameclients,
        SH_MEMBER(this, &BotIdentityPlugin::Hook_ClientPutInServer_Post), true);
    SH_ADD_HOOK(IServerGameClients, ClientDisconnect, gameclients,
        SH_MEMBER(this, &BotIdentityPlugin::Hook_ClientDisconnect_Pre), false);

    META_CONPRINTF("[BotIdentity] hooks installed: gameclients=%p\n", (void*)gameclients);

    SH_ADD_HOOK(IServerGameDLL, GameFrame, gameserver,
        SH_MEMBER(this, &BotIdentityPlugin::Hook_GameFrame_Post), true);

    SH_ADD_HOOK(ICvar, DispatchConCommand, icvar,
        SH_MEMBER(this, &BotIdentityPlugin::Hook_DispatchConCommand_Pre), false);
    SH_ADD_HOOK(ICvar, DispatchConCommand, icvar,
        SH_MEMBER(this, &BotIdentityPlugin::Hook_DispatchConCommand_Post), true);

    srand((unsigned int)time(nullptr));
    s_PluginActive = true;
    return true;
}

bool BotIdentityPlugin::Unload(char* error, size_t maxlen) {
    s_PluginActive = false;

    botid::ResetVoteTransaction();

    SH_REMOVE_HOOK(IServerGameClients, OnClientConnected, gameclients,
        SH_MEMBER(this, &BotIdentityPlugin::Hook_OnClientConnected_Post), true);
    SH_REMOVE_HOOK(IServerGameClients, ClientPutInServer, gameclients,
        SH_MEMBER(this, &BotIdentityPlugin::Hook_ClientPutInServer_Post), true);
    SH_REMOVE_HOOK(IServerGameClients, ClientDisconnect, gameclients,
        SH_MEMBER(this, &BotIdentityPlugin::Hook_ClientDisconnect_Pre), false);
    SH_REMOVE_HOOK(IServerGameDLL, GameFrame, gameserver,
        SH_MEMBER(this, &BotIdentityPlugin::Hook_GameFrame_Post), true);
    SH_REMOVE_HOOK(ICvar, DispatchConCommand, icvar,
        SH_MEMBER(this, &BotIdentityPlugin::Hook_DispatchConCommand_Pre), false);
    SH_REMOVE_HOOK(ICvar, DispatchConCommand, icvar,
        SH_MEMBER(this, &BotIdentityPlugin::Hook_DispatchConCommand_Post), true);

    botid::ShutdownSharedMemory();
    META_CONPRINTF("[BotIdentity] unloaded\n");
    return true;
}

void BotIdentityPlugin::AllPluginsLoaded() {
}

void BotIdentityPlugin::Hook_DispatchConCommand_Pre(
    ConCommandRef command, const CCommandContext& /*ctx*/, const CCommand& /*arguments*/)
{
    if (!s_PluginActive || !command.IsValidRef()) RETURN_META(MRES_IGNORED);
    if (!std::strcmp(command.GetName(), "callvote")) {
        botid::BeginVoteTransaction();
    }
    RETURN_META(MRES_IGNORED);
}

void BotIdentityPlugin::Hook_DispatchConCommand_Post(
    ConCommandRef command, const CCommandContext& /*ctx*/, const CCommand& /*arguments*/)
{
    if (!command.IsValidRef() || std::strcmp(command.GetName(), "callvote") != 0) RETURN_META(MRES_IGNORED);
    if (botid::VoteTransactionActive()) {
        botid::ScheduleVoteTransactionEnd(
            botid::BotInfos().Features().voteTransactionHoldFrames);
    }
    RETURN_META(MRES_IGNORED);
}

void BotIdentityPlugin::Hook_GameFrame_Post(bool /*simulating*/, bool /*bFirstTick*/, bool /*bLastTick*/) {
    if (!s_PluginActive) return;

    botid::TickVoteTransaction();

    const auto& f = botid::BotInfos().Features();
    if (!f.enableFakePing) return;

    // Wall-clock based timer that fires every ~30s
    auto now = std::chrono::steady_clock::now();
    double nowSec = std::chrono::duration<double>(now.time_since_epoch()).count();
    if (m_LastJitterTime == 0.0) m_LastJitterTime = nowSec;
    if (nowSec - m_LastJitterTime < 30.0) return;
    m_LastJitterTime = nowSec;

    int pct = f.pingJitterPercent > 0 ? f.pingJitterPercent : 30;
    int jitteredCount = 0;
    for (int slot = 0; slot < botid::kMaxSlots; ++slot) {
        if (!botid::IdentityMgr().IsManaged(slot)) continue;
        botid::BotIdentity* identity = botid::IdentityMgr().GetIdentity(slot);
        if (!identity || identity->ping <= 0) continue;

        int base = identity->ping;
        int range = (base * pct) / 100; if (range < 2) range = 2;
        int jitter = base + (rand() % (2 * range + 1)) - range;
        if (jitter < 5) jitter = 5;
        if (jitter > 250) jitter = 250;

        botid::PublishPing(slot, jitter);
        jitteredCount++;
    }
    if (jitteredCount > 0) {
        META_CONPRINTF("[BotIdentity] ping_jitter %d bots (jitter=%d%%)\n", jitteredCount, pct);
    }
}
