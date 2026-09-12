#include "controller_reap.h"

#include "bot_info.h"
#include "entity_access.h"
#include "plugin.h"
#include "ssc_ops.h"

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include <entity2/entityidentity.h>
#include <entity2/entityinstance.h>
#include <entityhandle.h>
#include <interfaces/interfaces.h>
#include <iserver.h>

class INetworkServerService;
extern INetworkServerService* g_pNetworkServerService;

namespace botid {

namespace {

struct PendingControllerRemoval {
    void* controller = nullptr;
    uint32_t handle = 0xFFFFFFFFu;
    int slot = -1;
    uint16_t userId = 0;
    unsigned int referencedFrames = 0;
};

std::vector<PendingControllerRemoval> g_Pending;

void ReapLog(const char* fmt, ...) {
    if (!g_BotIdentityPlugin.ismm_) return;
    char buffer[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    g_BotIdentityPlugin.ismm_->ConPrintf("[BotIdentity] %s", buffer);
}

bool GameServerAlive() {
    return g_pNetworkServerService && g_pNetworkServerService->GetIGameServer() != nullptr;
}

bool IsTrailingLeftoverSlot(int slot) {
    void* client = ResolveClientBySlot(slot);
    if (!client) return true;
    if (IdentityMgr().IsManaged(slot)) return false;
    if (ReadNetChannel(client) != nullptr) return false;
    if (ReadUserId(client) != 65535) return false;
    if (ReadSignonState(client) != 0) return false;
    if (ReadSteamId(client) != 0) return false;
    return true;
}

void ShrinkTrailingLeftoverClients() {
    const int count = ClientListCount();
    if (count <= 0) return;

    int keep = count;
    while (keep > 0 && IsTrailingLeftoverSlot(keep - 1)) {
        --keep;
    }
    if (keep == count) return;

    if (!ShrinkClientListTo(keep)) {
        ReapLog("client list shrink failed from=%d to=%d now=%d\n",
                count, keep, ClientListCount());
        return;
    }
    ReapLog("client list shrink from=%d to=%d dropped=%d\n",
            count, keep, count - keep);
}

bool ClientLooksLeftoverOrEmpty(int slot) {
    void* client = ResolveClientBySlot(slot);
    if (!client) return true;
    if (IdentityMgr().IsManaged(slot)) return false;
    if (ReadNetChannel(client) != nullptr) return false;
    if (ReadUserId(client) != 65535) return false;
    if (ReadSignonState(client) != 0) return false;
    if (ReadSteamId(client) != 0) return false;
    return true;
}

constexpr int kSpectatorTeam = 1;

uint8_t DetachControllerFromTeam(void* controller) {
    const uint8_t team = ReadControllerTeam(controller);
    if (team != 2 && team != 3) return team;
    // ChangeTeam on a 0x1210 entity is the crash risk. Offset write is the
    // leftover path after kick already marked delete.
    if (!IsEntityBeingDeleted(controller) &&
        CallControllerChangeTeam(controller, kSpectatorTeam)) {
        return team;
    }
    WriteControllerTeam(controller, 0);
    MarkEntityFieldChanged(controller, static_cast<uint32_t>(OFF_Controller_TeamNum));
    MarkEntityStateChanged(controller);
    return team;
}

bool IsControllerReferencedByClient(void* controller, uint16_t* userIdOut) {
    for (int slot = 0; slot < kMaxSlots; ++slot) {
        void* client = ResolveClientBySlot(slot);
        if (!client) continue;
        const int entityIndex = GetEntityIndex(client);
        char className[64] = {0};
        void* current = ResolveEntityInstance(entityIndex, className, sizeof(className));
        if (current != controller || std::strcmp(className, "cs_player_controller") != 0) {
            continue;
        }
        if (userIdOut) *userIdOut = ReadUserId(client);
        return true;
    }
    return false;
}

}  // namespace

bool QueueControllerRemovalForClient(void* client, int slot) {
    if (!client) return false;
    if (!UtilRemoveTarget()) {
        ReapLog("deferred destroy unavailable: UTIL_Remove unresolved\n");
        return false;
    }

    const int entityIndex = GetEntityIndex(client);
    char className[64] = {0};
    void* controller = ResolveEntityInstance(entityIndex, className, sizeof(className));
    if (!controller) {
        ReapLog("deferred destroy skipped: entity resolve failed slot=%d entIdx=%d\n",
                slot, entityIndex);
        return false;
    }
    if (std::strcmp(className, "cs_player_controller") != 0) {
        ReapLog("deferred destroy skipped slot=%d entIdx=%d cls='%s'\n",
                slot, entityIndex, className);
        return false;
    }
    if (IsEntityBeingDeleted(controller)) {
        ReapLog("deferred destroy skipped slot=%d entIdx=%d: already deleting\n",
                slot, entityIndex);
        return false;
    }

    const uint32_t handle =
        static_cast<uint32_t>(reinterpret_cast<CEntityInstance*>(controller)->GetRefEHandle().ToInt());
    auto duplicate = std::find_if(g_Pending.begin(), g_Pending.end(),
                                  [handle](const PendingControllerRemoval& item) {
                                      return item.handle == handle;
                                  });
    if (duplicate != g_Pending.end()) return true;

    g_Pending.push_back({controller, handle, slot, ReadUserId(client), 0});
    ReapLog("deferred destroy queued slot=%d handle=0x%08x userid=%u entIdx=%d\n",
            slot, handle, static_cast<unsigned int>(ReadUserId(client)), entityIndex);
    return true;
}

void DrainPendingControllerRemovals() {
    if (!GameServerAlive()) {
        if (!g_Pending.empty()) {
            ReapLog("deferred destroy cleared: game server gone count=%zu\n", g_Pending.size());
        }
        g_Pending.clear();
        return;
    }
    if (!UtilRemoveTarget()) {
        if (!g_Pending.empty()) {
            ReapLog("deferred destroy cleared: UTIL_Remove unresolved count=%zu\n",
                    g_Pending.size());
        }
        g_Pending.clear();
        ShrinkTrailingLeftoverClients();
        return;
    }

    if (!g_Pending.empty()) {
        ReapLog("deferred destroy drain pending=%zu\n", g_Pending.size());
    }

    for (auto item = g_Pending.begin(); item != g_Pending.end();) {
        const int entityIndex = CEntityHandle(item->handle).GetEntryIndex();
        char className[64] = {0};
        void* current = ResolveEntityInstance(entityIndex, className, sizeof(className));
        if (!current) {
            ReapLog("deferred destroy dropped slot=%d handle=0x%08x: resolve miss entIdx=%d\n",
                    item->slot, item->handle, entityIndex);
            item = g_Pending.erase(item);
            continue;
        }
        if (current != item->controller) {
            ReapLog("deferred destroy dropped slot=%d handle=0x%08x: pointer mismatch entIdx=%d\n",
                    item->slot, item->handle, entityIndex);
            item = g_Pending.erase(item);
            continue;
        }
        if (std::strcmp(className, "cs_player_controller") != 0) {
            ReapLog("deferred destroy dropped slot=%d handle=0x%08x: cls='%s' entIdx=%d\n",
                    item->slot, item->handle, className, entityIndex);
            item = g_Pending.erase(item);
            continue;
        }

        auto* entity = reinterpret_cast<CEntityInstance*>(current);
        if (!entity->m_pEntity) {
            ReapLog("deferred destroy dropped slot=%d handle=0x%08x: no identity entIdx=%d\n",
                    item->slot, item->handle, entityIndex);
            item = g_Pending.erase(item);
            continue;
        }
        const uint32_t flags = static_cast<uint32_t>(entity->m_pEntity->m_flags);
        // Kick already sets EF_MARKED_FOR_DELETE|EF_DELETE_IN_PROGRESS
        // (0.1.12 live: 0x1210). Hibernate then skips GameFrame, so the
        // engine never finishes. Still call UTIL_Remove.

        const uint32_t liveHandle = static_cast<uint32_t>(entity->GetRefEHandle().ToInt());
        if (liveHandle != item->handle) {
            ReapLog("deferred destroy dropped slot=%d handle=0x%08x: live handle=0x%08x\n",
                    item->slot, item->handle, liveHandle);
            item = g_Pending.erase(item);
            continue;
        }

        uint16_t currentUserId = 0;
        if (IsControllerReferencedByClient(current, &currentUserId)) {
            // 0.1.13 live: leftover [NoChan] challenging slots keep the
            // controller pointer with userid 65535. That is not a new player.
            if (currentUserId == 65535) {
                ReapLog("deferred destroy leftover slot=%d handle=0x%08x: challenging userid=65535\n",
                        item->slot, item->handle);
            } else if (currentUserId != item->userId) {
                ReapLog("deferred destroy abandoned slot=%d handle=0x%08x: rebound userid=%u\n",
                        item->slot, item->handle, static_cast<unsigned int>(currentUserId));
                item = g_Pending.erase(item);
                continue;
            } else if (item->referencedFrames++ == 0) {
                ReapLog("deferred destroy wait slot=%d handle=0x%08x: still referenced userid=%u\n",
                        item->slot, item->handle, static_cast<unsigned int>(currentUserId));
                ++item;
                continue;
            }
        }

        const uint8_t team = DetachControllerFromTeam(current);
        RemoveEntity(current);
        ReapLog("deferred destroy removed slot=%d handle=0x%08x flags=0x%x team=%u\n",
                item->slot, item->handle, flags, static_cast<unsigned int>(team));
        item = g_Pending.erase(item);
        continue;
    }

    ShrinkTrailingLeftoverClients();
}

void ClearPendingControllerRemovals() { g_Pending.clear(); }

void DumpPlayerControllers(const char* tag) {
    const char* label = tag && tag[0] ? tag : "?";
    int found = 0;
    int onTeam = 0;
    for (int entityIndex = 1; entityIndex <= kMaxSlots; ++entityIndex) {
        char className[64] = {0};
        void* instance = ResolveEntityInstance(entityIndex, className, sizeof(className));
        if (!instance) continue;
        if (std::strcmp(className, "cs_player_controller") != 0) continue;
        ++found;
        auto* entity = reinterpret_cast<CEntityInstance*>(instance);
        const uint32_t flags =
            entity->m_pEntity ? static_cast<uint32_t>(entity->m_pEntity->m_flags) : 0;
        const uint32_t handle = static_cast<uint32_t>(entity->GetRefEHandle().ToInt());
        const uint8_t team = ReadControllerTeam(instance);
        if (team == 2 || team == 3) ++onTeam;
        ReapLog(
            "controller entIdx=%d team836=%u flags=0x%x handle=0x%08x deleting=%d tag=%s\n",
            entityIndex,
            static_cast<unsigned int>(team),
            flags,
            handle,
            IsEntityBeingDeleted(instance) ? 1 : 0,
            label);
        auto* raw = reinterpret_cast<unsigned char*>(instance);
        char scan[384];
        int n = std::snprintf(scan, sizeof(scan), "teamscan entIdx=%d", entityIndex);
        int hits = 0;
        for (int off = 0; off + 4 <= 4096; off += 4) {
            uint32_t value = 0;
            std::memcpy(&value, raw + off, sizeof(value));
            if (value != 2 && value != 3) continue;
            n += std::snprintf(scan + n, sizeof(scan) - n, " +%d=%u", off, value);
            if (++hits >= 12) break;
        }
        if (hits == 0) {
            n += std::snprintf(scan + n, sizeof(scan) - n, " none");
        }
        ReapLog("%s tag=%s\n", scan, label);
    }
    ReapLog("controller occupancy found=%d onTeam=%d tag=%s\n", found, onTeam, label);
}

void DumpTeamManagers(const char* tag) {
    const char* label = tag && tag[0] ? tag : "?";
    uint32_t leftoverHandles[kMaxSlots];
    int leftoverCount = 0;
    for (int entityIndex = 1; entityIndex <= kMaxSlots; ++entityIndex) {
        char className[64] = {0};
        void* instance = ResolveEntityInstance(entityIndex, className, sizeof(className));
        if (!instance) continue;
        if (std::strcmp(className, "cs_player_controller") != 0) continue;
        auto* entity = reinterpret_cast<CEntityInstance*>(instance);
        leftoverHandles[leftoverCount++] =
            static_cast<uint32_t>(entity->GetRefEHandle().ToInt());
    }

    int found = 0;
    int handleHits = 0;
    for (int entityIndex = 1; entityIndex <= 2048; ++entityIndex) {
        char className[64] = {0};
        void* instance = ResolveEntityInstance(entityIndex, className, sizeof(className));
        if (!instance) continue;
        if (std::strcmp(className, "cs_team_manager") != 0 &&
            std::strcmp(className, "team_manager") != 0) {
            continue;
        }
        ++found;
        auto* entity = reinterpret_cast<CEntityInstance*>(instance);
        const uint32_t flags =
            entity->m_pEntity ? static_cast<uint32_t>(entity->m_pEntity->m_flags) : 0;
        const uint8_t team = ReadControllerTeam(instance);
        auto* raw = reinterpret_cast<unsigned char*>(instance);
        char scan[384];
        int n = std::snprintf(
            scan, sizeof(scan),
            "teammanager entIdx=%d team=%u flags=0x%x handles",
            entityIndex,
            static_cast<unsigned int>(team),
            flags);
        int hits = 0;
        for (int off = 0; off + 4 <= 2048; off += 4) {
            uint32_t value = 0;
            std::memcpy(&value, raw + off, sizeof(value));
            bool match = false;
            for (int i = 0; i < leftoverCount; ++i) {
                if (value == leftoverHandles[i]) {
                    match = true;
                    break;
                }
            }
            if (!match) continue;
            n += std::snprintf(scan + n, sizeof(scan) - n, " +%d=0x%08x", off, value);
            ++hits;
            ++handleHits;
            if (hits >= 8) break;
        }
        if (hits == 0) {
            n += std::snprintf(scan + n, sizeof(scan) - n, " none");
        }
        ReapLog("%s tag=%s\n", scan, label);
    }
    ReapLog("team manager occupancy found=%d leftoverControllers=%d handleHits=%d tag=%s\n",
            found, leftoverCount, handleHits, label);
}

void MoveManagedBotsToSpectator() {
    if (ChangeTeamVtableIndex() < 0) {
        ReapLog("ChangeTeam skipped: vtable index unset\n");
        return;
    }

    int moved = 0;
    int failed = 0;
    for (int slot = 0; slot < kMaxSlots; ++slot) {
        if (!IdentityMgr().IsManaged(slot)) continue;
        void* client = ResolveClientBySlot(slot);
        if (!client) continue;
        const int entityIndex = GetEntityIndex(client);
        char className[64] = {0};
        void* controller = ResolveEntityInstance(entityIndex, className, sizeof(className));
        if (!controller || std::strcmp(className, "cs_player_controller") != 0) continue;
        if (IsEntityBeingDeleted(controller)) {
            ReapLog("ChangeTeam skipped slot=%d entIdx=%d: already deleting\n",
                    slot, entityIndex);
            continue;
        }
        const uint8_t team = ReadControllerTeam(controller);
        if (team != 2 && team != 3) continue;
        const bool ok = CallControllerChangeTeam(controller, kSpectatorTeam);
        const uint8_t after = ReadControllerTeam(controller);
        ReapLog("ChangeTeam slot=%d entIdx=%d team=%u -> 1 ok=%d after=%u\n",
                slot,
                entityIndex,
                static_cast<unsigned int>(team),
                ok ? 1 : 0,
                static_cast<unsigned int>(after));
        if (ok) ++moved;
        else ++failed;
    }
    if (moved != 0 || failed != 0) {
        ReapLog("ChangeTeam spectator moved=%d failed=%d\n", moved, failed);
    }
}

void ReapOrphanControllers() {
    if (!GameServerAlive() || !UtilRemoveTarget()) return;

    int detached = 0;
    int removed = 0;
    for (int entityIndex = 1; entityIndex <= kMaxSlots; ++entityIndex) {
        char className[64] = {0};
        void* instance = ResolveEntityInstance(entityIndex, className, sizeof(className));
        if (!instance) continue;
        if (std::strcmp(className, "cs_player_controller") != 0) continue;

        const int slot = entityIndex - 1;
        if (!ClientLooksLeftoverOrEmpty(slot)) continue;

        const uint8_t team = ReadControllerTeam(instance);
        if (team != 2 && team != 3) continue;

        const uint8_t previous = DetachControllerFromTeam(instance);
        ++detached;
        if (!IsEntityBeingDeleted(instance)) {
            RemoveEntity(instance);
            ++removed;
        }
        ReapLog("orphan controller slot=%d entIdx=%d team=%u -> 0 removed=%d\n",
                slot,
                entityIndex,
                static_cast<unsigned int>(previous),
                IsEntityBeingDeleted(instance) ? 1 : 0);
    }
    if (detached != 0 || removed != 0) {
        ReapLog("orphan controller reap detached=%d removed=%d\n", detached, removed);
    }
}

void DumpOccupiedClients(const char* tag) {
    const char* label = tag && tag[0] ? tag : "?";
    int occupied = 0;
    for (int slot = 0; slot < kMaxSlots; ++slot) {
        void* client = ResolveClientBySlot(slot);
        if (!client) continue;
        ++occupied;
        ReapLog(
            "client slot=%d ptr=%p userid=%u signon=%d fake=%u conn=0x%02x "
            "entIdx=%d steamid=%llu netch=%p cslot=%d managed=%d tag=%s\n",
            slot,
            client,
            static_cast<unsigned int>(ReadUserId(client)),
            ReadSignonState(client),
            IsFakePlayerSet(client) ? 1u : 0u,
            static_cast<unsigned int>(ReadConnectionTypeFlags(client)),
            GetEntityIndex(client),
            static_cast<unsigned long long>(ReadSteamId(client)),
            ReadNetChannel(client),
            ReadClientSlot(client),
            IdentityMgr().IsManaged(slot) ? 1 : 0,
            label);
    }
    ReapLog("client occupancy count=%d tag=%s\n", occupied, label);
}

}  // namespace botid
