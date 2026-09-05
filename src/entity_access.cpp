#include "entity_access.h"
#include "ssc_ops.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#include <ISmmPlugin.h>
#include <eiface.h>
#include <entity2/entityinstance.h>
#include <interfaces/interfaces.h>
#include <iserver.h>
#include <tier1/utlvector.h>

namespace botid {

// Entity-system offsets (overridden by LoadGamedata if present in gamedata.json)
static int s_kEntSys_OffsetInGameResSvc   = 0x58;
static int s_kEntSys_IdentityChunksOffset = 0x10;
static int s_kEntIdentity_Size            = 0x70;
static int s_kEntIdentity_InstanceOffset  = 0x00;
static int s_kEntIdentity_ClassNameOffset = 0x20;

static void* g_pGameResourceService = nullptr;
static void* g_ppEntSysGlobal       = nullptr;  // resolved via UTIL_Remove signature

// Tiny gamedata reader: only enough to read "Key": { "linux": <num> }
static int ReadOffset(const std::string& json, const std::string& key) {
    const std::string k = "\"" + key + "\"";
    auto pos = json.find(k);
    if (pos == std::string::npos) return -1;
    pos = json.find("\"linux\"", pos);
    if (pos == std::string::npos) return -1;
    pos = json.find(':', pos);
    if (pos == std::string::npos) return -1;
    pos++;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
    int sign = 1;
    if (pos < json.size() && json[pos] == '-') { sign = -1; pos++; }
    int n = 0;
    bool any = false;
    while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9') {
        n = n * 10 + (json[pos] - '0');
        pos++; any = true;
    }
    return any ? sign * n : -1;
}

void LoadGamedata(const char* path) {
    std::ifstream f(path);
    if (!f) return;
    std::stringstream ss; ss << f.rdbuf();
    std::string json = ss.str();

    auto tryApply = [&](const char* key, int& target) {
        int v = ReadOffset(json, key);
        if (v >= 0) target = v;
    };

    tryApply("CServerSideClient::m_nConnectionTypeFlags", OFF_m_nConnectionTypeFlags);
    tryApply("CServerSideClient::m_bFakePlayer",          OFF_m_bFakePlayer);
    tryApply("CServerSideClient::m_SteamID",              OFF_m_SteamID);
    tryApply("CServerSideClient::m_SteamIDMirror",        OFF_m_SteamIDMirror);
    tryApply("CServerSideClient::m_nEntityIndex",         OFF_m_nEntityIndex);
    tryApply("CBasePlayerController::FakeClientFlags",    OFF_Controller_FakeClientFlags);
    tryApply("CBasePlayerController::m_steamID",          OFF_Controller_SteamID);
    tryApply("CNetworkGameServerBase::m_Clients",         OFF_ClientList);
    tryApply("GameResourceServiceServer::m_pEntitySystem", s_kEntSys_OffsetInGameResSvc);
    tryApply("CEntitySystem::m_EntityList",               s_kEntSys_IdentityChunksOffset);
    tryApply("CEntityIdentity::Size",                     s_kEntIdentity_Size);
    tryApply("CEntityIdentity::m_pInstance",              s_kEntIdentity_InstanceOffset);
    tryApply("CEntityIdentity::m_designerName",           s_kEntIdentity_ClassNameOffset);
}

void* ResolveClientBySlot(int slot) {
    if (!g_pNetworkServerService) return nullptr;
    auto* gameServer = g_pNetworkServerService->GetIGameServer();
    if (!gameServer) return nullptr;
    auto* clients = reinterpret_cast<CUtlVector<void*>*>(
        reinterpret_cast<unsigned char*>(gameServer) + OFF_ClientList);
    const int count = clients->Count();
    if (count < 0 || count > 256 || slot < 0 || slot >= count) return nullptr;
    return clients->Element(slot);
}

static bool SafeReadPointer(const void* address, void** output) {
    if (!address) { *output = nullptr; return false; }
    *output = *reinterpret_cast<void* const*>(address);
    return true;
}

void* ResolveEntityInstance(int entityIndex, char* classnameOut, size_t classnameCap, bool /*debug*/) {
    if (classnameOut && classnameCap) classnameOut[0] = '\0';
    if (!g_pGameResourceService || entityIndex <= 0 || entityIndex >= 0x8000) {
        return nullptr;
    }

    void* entitySystem = nullptr;
    SafeReadPointer(
        reinterpret_cast<unsigned char*>(g_pGameResourceService) +
            s_kEntSys_OffsetInGameResSvc, &entitySystem);
    if (!entitySystem) return nullptr;

    constexpr int kEntListChunkSize = 512;
    void* chunk = nullptr;
    const void* chunkSlot = reinterpret_cast<unsigned char*>(entitySystem) +
                            s_kEntSys_IdentityChunksOffset +
                            (entityIndex / kEntListChunkSize) * sizeof(void*);
    if (!SafeReadPointer(chunkSlot, &chunk) || !chunk) return nullptr;

    unsigned char* identity =
        reinterpret_cast<unsigned char*>(chunk) +
        (entityIndex % kEntListChunkSize) * s_kEntIdentity_Size;

    if (classnameOut && classnameCap) {
        const char* name = *reinterpret_cast<const char* const*>(
            identity + s_kEntIdentity_ClassNameOffset);
        if (name) {
            size_t i = 0;
            for (; i + 1 < classnameCap && name[i]; ++i) classnameOut[i] = name[i];
            classnameOut[i] = '\0';
        }
    }

    void* instance = nullptr;
    if (!SafeReadPointer(identity + s_kEntIdentity_InstanceOffset, &instance) || !instance) {
        return nullptr;
    }
    return instance;
}

void MarkEntityStateChanged(void* instance) {
    if (!instance) return;
    NetworkStateChangedData changed(true);
    reinterpret_cast<CEntityInstance*>(instance)->NetworkStateChanged(changed);
}

void SetGameResourceServicePtr(void* p) { g_pGameResourceService = p; }
void* GetGameResourceServicePtr()       { return g_pGameResourceService; }
void SetEntSysGlobalPtr(void* p)        { g_ppEntSysGlobal = p; }

bool SafeReadPtr(const void* address, void** output) {
    return SafeReadPointer(address, output);
}

}  // namespace botid
