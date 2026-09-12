#include "entity_access.h"
#include "ssc_ops.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <ISmmPlugin.h>
#include <eiface.h>
#include <entity2/entityidentity.h>
#include <entity2/entityinstance.h>
#include <interfaces/interfaces.h>
#include <iserver.h>
#include <tier1/utlvector.h>

#if !defined(_WIN32)
#include <elf.h>
#include <link.h>
#endif

namespace botid {

using UtilRemoveFn = void (*)(void*);

// Entity-system offsets (overridden by LoadGamedata if present in gamedata.json)
static int s_kEntSys_OffsetInGameResSvc   = 0x58;
static int s_kEntSys_IdentityChunksOffset = 0x10;
static int s_kEntIdentity_Size            = 0x70;
static int s_kEntIdentity_InstanceOffset  = 0x00;
static int s_kEntIdentity_ClassNameOffset = 0x20;

static void* g_pGameResourceService = nullptr;
static void* g_ppEntSysGlobal       = nullptr;  // resolved via UTIL_Remove signature
static UtilRemoveFn g_pfnUtilRemove = nullptr;
static std::string g_UtilRemoveModule;
static std::string s_UtilRemoveSig =
    "48 89 FE 48 85 FF 74 ? 48 8D 05 ? ? ? ? 48";

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

static std::string ReadLinuxString(const std::string& json, const std::string& key) {
    const std::string k = "\"" + key + "\"";
    auto pos = json.find(k);
    if (pos == std::string::npos) return {};
    pos = json.find("\"linux\"", pos);
    if (pos == std::string::npos) return {};
    pos = json.find(':', pos);
    if (pos == std::string::npos) return {};
    pos++;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
    if (pos >= json.size() || json[pos] != '"') return {};
    ++pos;
    const size_t start = pos;
    while (pos < json.size() && json[pos] != '"') ++pos;
    if (pos >= json.size()) return {};
    return json.substr(start, pos - start);
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
    tryApply("CServerSideClient::m_UserID",               OFF_m_UserID);
    tryApply("CServerSideClient::m_SteamID",              OFF_m_SteamID);
    tryApply("CServerSideClient::m_SteamIDMirror",        OFF_m_SteamIDMirror);
    tryApply("CServerSideClient::m_nEntityIndex",         OFF_m_nEntityIndex);
    tryApply("CServerSideClient::m_nClientSlot",          OFF_m_nClientSlot);
    tryApply("CServerSideClient::m_NetChannel",           OFF_m_NetChannel);
    tryApply("CServerSideClient::m_nSignonState",         OFF_m_nSignonState);
    tryApply("CBasePlayerController::FakeClientFlags",    OFF_Controller_FakeClientFlags);
    tryApply("CBasePlayerController::m_steamID",          OFF_Controller_SteamID);
    tryApply("CBaseEntity::m_iTeamNum",                   OFF_Controller_TeamNum);
    tryApply("CCSPlayerController_ChangeTeam",            OFF_ChangeTeamVtable);
    tryApply("CNetworkGameServerBase::m_Clients",         OFF_ClientList);
    tryApply("GameResourceServiceServer::m_pEntitySystem", s_kEntSys_OffsetInGameResSvc);
    tryApply("CEntitySystem::m_EntityList",               s_kEntSys_IdentityChunksOffset);
    tryApply("CEntityIdentity::Size",                     s_kEntIdentity_Size);
    tryApply("CEntityIdentity::m_pInstance",              s_kEntIdentity_InstanceOffset);
    tryApply("CEntityIdentity::m_designerName",           s_kEntIdentity_ClassNameOffset);

    std::string utilSig = ReadLinuxString(json, "UTIL_Remove");
    if (!utilSig.empty()) s_UtilRemoveSig = utilSig;
}

namespace {

CUtlVector<void*>* ClientVector() {
    if (!g_pNetworkServerService) return nullptr;
    auto* gameServer = g_pNetworkServerService->GetIGameServer();
    if (!gameServer) return nullptr;
    return reinterpret_cast<CUtlVector<void*>*>(
        reinterpret_cast<unsigned char*>(gameServer) + OFF_ClientList);
}

}  // namespace

void* ResolveClientBySlot(int slot) {
    auto* clients = ClientVector();
    if (!clients) return nullptr;
    const int count = clients->Count();
    if (count < 0 || count > 256 || slot < 0 || slot >= count) return nullptr;
    return clients->Element(slot);
}

int ClientListCount() {
    auto* clients = ClientVector();
    if (!clients) return 0;
    const int count = clients->Count();
    if (count < 0 || count > 256) return 0;
    return count;
}

bool ShrinkClientListTo(int newCount) {
    auto* clients = ClientVector();
    if (!clients) return false;
    const int count = clients->Count();
    if (count < 0 || count > 256) return false;
    if (newCount < 0 || newCount >= count) return false;
    // CUtlVectorBase lays m_Size first. Count() already matches live
    // occupancy, so this is the same field. SetCount() is RemoveAll +
    // AddMultipleToTail (wipes humans). FastRemove shifts later slots.
    // In-place nullptr is the 0.1.19 segfault at +0x64. Only drop Count.
    int* sizeField = reinterpret_cast<int*>(clients);
    if (*sizeField != count) return false;
    *sizeField = newCount;
    return clients->Count() == newCount;
}

static bool SafeReadPointer(const void* address, void** output) {
    if (!address) { *output = nullptr; return false; }
    *output = *reinterpret_cast<void* const*>(address);
    return true;
}

void* ResolveEntityInstance(int entityIndex, char* classnameOut, size_t classnameCap, bool /*debug*/) {
    if (classnameOut && classnameCap) classnameOut[0] = '\0';
    if (entityIndex <= 0 || entityIndex >= 0x8000) {
        return nullptr;
    }

    void* entitySystem = nullptr;
    if (g_ppEntSysGlobal) SafeReadPointer(g_ppEntSysGlobal, &entitySystem);
    if (!entitySystem && g_pGameResourceService) {
        SafeReadPointer(
            reinterpret_cast<unsigned char*>(g_pGameResourceService) +
                s_kEntSys_OffsetInGameResSvc, &entitySystem);
    }
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

void MarkEntityFieldChanged(void* instance, uint32_t localOffset) {
    if (!instance) return;
    NetworkStateChangedData changed(localOffset);
    reinterpret_cast<CEntityInstance*>(instance)->NetworkStateChanged(changed);
}

void SetGameResourceServicePtr(void* p) { g_pGameResourceService = p; }
void* GetGameResourceServicePtr()       { return g_pGameResourceService; }
void SetEntSysGlobalPtr(void* p)        { g_ppEntSysGlobal = p; }

bool SafeReadPtr(const void* address, void** output) {
    return SafeReadPointer(address, output);
}

namespace {

bool ParseSigString(const std::string& sigStr, std::vector<uint8_t>& outBytes, std::vector<bool>& outWild) {
    outBytes.clear();
    outWild.clear();
    const char* p = sigStr.c_str();
    while (*p) {
        if (*p == ' ') { ++p; continue; }
        if (*p == '?') {
            outBytes.push_back(0);
            outWild.push_back(true);
            ++p;
            if (*p == '?') ++p;
            continue;
        }
        char* end = nullptr;
        unsigned long v = std::strtoul(p, &end, 16);
        if (end == p || end - p > 2 || v > 0xFF) return false;
        outBytes.push_back(static_cast<uint8_t>(v));
        outWild.push_back(false);
        p = end;
    }
    return !outBytes.empty();
}

struct ModuleSegment {
    unsigned char* base = nullptr;
    size_t size = 0;
};

struct ServerModule {
    std::string path;
    std::vector<ModuleSegment> segments;
};

#if !defined(_WIN32)
void AppendExecutableSegments(dl_phdr_info* info, std::vector<ModuleSegment>& segments) {
    for (int i = 0; i < info->dlpi_phnum; ++i) {
        const ElfW(Phdr)& ph = info->dlpi_phdr[i];
        if (ph.p_type != PT_LOAD || ph.p_memsz == 0 || (ph.p_flags & PF_X) == 0) continue;
        segments.push_back({
            reinterpret_cast<unsigned char*>(info->dlpi_addr + ph.p_vaddr),
            static_cast<size_t>(ph.p_memsz)
        });
    }
}

bool AddressInModule(dl_phdr_info* info, uintptr_t address) {
    for (int i = 0; i < info->dlpi_phnum; ++i) {
        const ElfW(Phdr)& ph = info->dlpi_phdr[i];
        if (ph.p_type != PT_LOAD || ph.p_memsz == 0) continue;
        const uintptr_t start = info->dlpi_addr + ph.p_vaddr;
        const uintptr_t end = start + ph.p_memsz;
        if (address >= start && address < end) return true;
    }
    return false;
}

struct FindByAddressCtx {
    uintptr_t address = 0;
    ServerModule result;
};

int FindByAddressCallback(dl_phdr_info* info, size_t, void* data) {
    auto* ctx = static_cast<FindByAddressCtx*>(data);
    if (!info || !AddressInModule(info, ctx->address)) return 0;
    ctx->result.path = (info->dlpi_name && info->dlpi_name[0]) ? info->dlpi_name : "";
    AppendExecutableSegments(info, ctx->result.segments);
    return ctx->result.segments.empty() ? 0 : 1;
}

struct CollectServerCtx {
    std::vector<ServerModule> modules;
};

int CollectLibServerCallback(dl_phdr_info* info, size_t, void* data) {
    if (!info || !info->dlpi_name) return 0;
    const char* slash = std::strrchr(info->dlpi_name, '/');
    const char* base = slash ? slash + 1 : info->dlpi_name;
    if (std::strcmp(base, "libserver.so") != 0) return 0;

    ServerModule module;
    module.path = info->dlpi_name;
    AppendExecutableSegments(info, module.segments);
    if (!module.segments.empty()) {
        static_cast<CollectServerCtx*>(data)->modules.push_back(std::move(module));
    }
    return 0;
}
#endif

void* FindPattern(const std::vector<ModuleSegment>& segments,
                  const std::vector<uint8_t>& pattern,
                  const std::vector<bool>& wild) {
    if (pattern.empty() || pattern.size() != wild.size()) return nullptr;
    const size_t plen = pattern.size();
    for (const auto& segment : segments) {
        if (!segment.base || segment.size < plen) continue;
        for (size_t i = 0; i + plen <= segment.size; ++i) {
            bool match = true;
            for (size_t j = 0; j < plen; ++j) {
                if (!wild[j] && segment.base[i + j] != pattern[j]) {
                    match = false;
                    break;
                }
            }
            if (match) return segment.base + i;
        }
    }
    return nullptr;
}

}  // namespace

void ResolveUtilRemove(void* serverInterface) {
    g_pfnUtilRemove = nullptr;
    g_ppEntSysGlobal = nullptr;
    g_UtilRemoveModule.clear();

#if defined(_WIN32)
    (void)serverInterface;
    return;
#else
    std::vector<uint8_t> bytes;
    std::vector<bool> wild;
    if (!ParseSigString(s_UtilRemoveSig, bytes, wild)) return;

    auto scanModule = [&](const ServerModule& module) -> unsigned char* {
        if (module.segments.empty()) return nullptr;
        auto* hit = static_cast<unsigned char*>(FindPattern(module.segments, bytes, wild));
        if (hit) g_UtilRemoveModule = module.path;
        return hit;
    };

    unsigned char* hit = nullptr;
    if (serverInterface) {
        void* vtable = *reinterpret_cast<void**>(serverInterface);
        if (vtable) {
            FindByAddressCtx ctx;
            ctx.address = reinterpret_cast<uintptr_t>(vtable);
            dl_iterate_phdr(FindByAddressCallback, &ctx);
            hit = scanModule(ctx.result);
        }
    }
    if (!hit) {
        CollectServerCtx ctx;
        dl_iterate_phdr(CollectLibServerCallback, &ctx);
        for (const auto& module : ctx.modules) {
            hit = scanModule(module);
            if (hit) break;
        }
    }
    if (!hit) return;
    g_pfnUtilRemove = reinterpret_cast<UtilRemoveFn>(hit);

    // Linux: lea rax, [rip+disp32] inside the signature.
    for (size_t i = 0; i + 7 <= bytes.size(); ++i) {
        if (bytes[i] != 0x48 || bytes[i + 1] != 0x8D || bytes[i + 2] != 0x05) continue;
        unsigned char* displacementAddress = hit + i + 3;
        const int32_t displacement = *reinterpret_cast<int32_t*>(displacementAddress);
        g_ppEntSysGlobal = reinterpret_cast<void*>(displacementAddress + 4 + displacement);
        break;
    }
#endif
}

const char* UtilRemoveModulePath() {
    return g_UtilRemoveModule.c_str();
}

void* UtilRemoveTarget() { return reinterpret_cast<void*>(g_pfnUtilRemove); }

bool RemoveEntity(void* instance) {
    if (!g_pfnUtilRemove || !instance) return false;
    g_pfnUtilRemove(instance);
    return true;
}

bool IsEntityBeingDeleted(void* instance) {
    if (!instance) return true;
    auto* entity = reinterpret_cast<CEntityInstance*>(instance);
    if (!entity->m_pEntity) return true;
    const uint32_t flags = static_cast<uint32_t>(entity->m_pEntity->m_flags);
    return (flags & (EF_DELETE_IN_PROGRESS | EF_MARKED_FOR_DELETE)) != 0;
}

int ChangeTeamVtableIndex() {
    if (OFF_ChangeTeamVtable < 0 || OFF_ChangeTeamVtable > 512) return -1;
    return OFF_ChangeTeamVtable;
}

bool CallControllerChangeTeam(void* controller, int team) {
    if (!controller) return false;
    const int index = ChangeTeamVtableIndex();
    if (index < 0) return false;
    void** vtable = *reinterpret_cast<void***>(controller);
    if (!vtable) return false;
    using ChangeTeamFn = void (*)(void*, int);
    auto fn = reinterpret_cast<ChangeTeamFn>(vtable[index]);
    if (!fn) return false;
    fn(controller, team);
    return true;
}

}  // namespace botid
