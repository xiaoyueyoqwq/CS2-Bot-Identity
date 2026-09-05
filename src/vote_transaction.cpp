#include "vote_transaction.h"

#include "bot_info.h"
#include "entity_access.h"
#include "plugin.h"
#include "shm_pub.h"
#include "ssc_ops.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include <eiface.h>
#include <entity2/entityinstance.h>
#include <interfaces/interfaces.h>
#include <iserver.h>
#include <playerslot.h>
#include <steam/steamclientpublic.h>

class INetworkServerService;
extern INetworkServerService* g_pNetworkServerService;
extern IVEngineServer* engine;

namespace botid {

// Console sink via the plugin's ISmmAPI pointer (avoids g_SMAPI linkage).
static void VoteLog(const char* fmt, ...) {
    if (!g_BotIdentityPlugin.ismm_) return;
    char buffer[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    g_BotIdentityPlugin.ismm_->ConPrintf("[BotIdentity] %s", buffer);
}

namespace {

constexpr int kMaxVoteSlots = 64;
constexpr int kMaxSteamIdCopies = 8;
constexpr int kSscScanBytes = 2048;
constexpr int kControllerScanBytes = 4096;

struct SteamIdCopy {
    int offset = -1;
    uint64_t value = 0;
    bool onController = false;
};

struct VoteSlotSnapshot {
    bool captured = false;
    int slot = -1;
    void* client = nullptr;
    int entityIndex = -1;
    unsigned char connectionFlags = 0;
    unsigned char fakePlayer = 0;
    uint64_t sscSteamId = 0;
    bool hasController = false;
    void* controller = nullptr;
    uint32_t controllerFlags = 0;
    uint32_t controllerHandle = 0;
    uint64_t controllerOff1800 = 0;
    SteamIdCopy copies[kMaxSteamIdCopies];
    int copyCount = 0;
    bool holdProbed = false;
};

unsigned int g_VoteDepth = 0;
VoteSlotSnapshot g_VoteSnapshots[kMaxVoteSlots];
bool g_VoteSnapshotValid = false;
bool g_EndScheduled = false;
int g_HoldFramesRemaining = 0;

bool ResolveController(int entityIndex, void** outController, uint32_t* outHandle) {
    *outController = nullptr;
    *outHandle = 0;
    if (entityIndex <= 0 || entityIndex >= 0x8000) return false;

    char className[64] = {0};
    void* controller = ResolveEntityInstance(entityIndex, className, sizeof(className));
    if (!controller || std::strcmp(className, "cs_player_controller") != 0) return false;

    auto* entity = reinterpret_cast<CEntityInstance*>(controller);
    *outController = controller;
    *outHandle = static_cast<uint32_t>(entity->GetRefEHandle().ToInt());
    return true;
}

bool SnapshotControllerLive(const VoteSlotSnapshot& snapshot, void* currentClient, void** outController) {
    *outController = nullptr;
    if (!snapshot.hasController || !currentClient) return false;
    void* controller = nullptr;
    uint32_t handle = 0;
    if (!ResolveController(GetEntityIndex(currentClient), &controller, &handle)) return false;
    if (controller != snapshot.controller || handle != snapshot.controllerHandle) return false;
    *outController = controller;
    return true;
}

bool CopyOverlaps(const VoteSlotSnapshot& snapshot, int offset, bool onController) {
    for (int i = 0; i < snapshot.copyCount; ++i) {
        const auto& copy = snapshot.copies[i];
        if (copy.onController != onController) continue;
        const int delta = copy.offset - offset;
        if (delta > -8 && delta < 8) return true;
    }
    return false;
}

void FormatCopyOffsets(const VoteSlotSnapshot& snapshot, char* out, size_t cap) {
    if (!out || cap == 0) return;
    out[0] = '\0';
    size_t used = 0;
    for (int i = 0; i < snapshot.copyCount; ++i) {
        const auto& copy = snapshot.copies[i];
        char piece[24];
        const int n = snprintf(piece, sizeof(piece), "%s%s%d",
                               i == 0 ? "" : ",",
                               copy.onController ? "c" : "s",
                               copy.offset);
        if (n <= 0) break;
        if (used + static_cast<size_t>(n) + 1 > cap) break;
        std::memcpy(out + used, piece, static_cast<size_t>(n));
        used += static_cast<size_t>(n);
        out[used] = '\0';
    }
    if (snapshot.copyCount == 0 && cap > 1) {
        std::memcpy(out, "-", 2);
    }
}

void ScanAndZeroSteamIdCopies(VoteSlotSnapshot& snapshot, void* client, void* controller) {
    const uint64_t target = snapshot.sscSteamId;
    if (target == 0) return;

    auto scanRegion = [&](void* base, int nbytes, bool onController) {
        if (!base || nbytes < 8) return;
        auto* raw = reinterpret_cast<unsigned char*>(base);
        for (int off = 0; off + 8 <= nbytes; ++off) {
            uint64_t val = 0;
            std::memcpy(&val, raw + off, sizeof(val));
            if (val != target) continue;

            if (!CopyOverlaps(snapshot, off, onController) &&
                snapshot.copyCount < kMaxSteamIdCopies) {
                snapshot.copies[snapshot.copyCount] = SteamIdCopy{off, val, onController};
                snapshot.copyCount++;
            }

            uint64_t zero = 0;
            std::memcpy(raw + off, &zero, sizeof(zero));
            off += 7;
        }
    };

    scanRegion(client, kSscScanBytes, false);
    scanRegion(controller, kControllerScanBytes, true);
}

void RestoreSteamIdCopies(const VoteSlotSnapshot& snapshot, void* client, void* controller) {
    for (int i = 0; i < snapshot.copyCount; ++i) {
        const auto& copy = snapshot.copies[i];
        void* base = copy.onController ? controller : client;
        if (!base || copy.offset < 0) continue;
        const int limit = copy.onController ? kControllerScanBytes : kSscScanBytes;
        if (copy.offset + 8 > limit) continue;
        auto* raw = reinterpret_cast<unsigned char*>(base);
        std::memcpy(raw + copy.offset, &copy.value, sizeof(copy.value));
    }
}

bool CopiesReappeared(const VoteSlotSnapshot& snapshot, void* client, void* controller) {
    if (snapshot.sscSteamId == 0) return false;
    for (int i = 0; i < snapshot.copyCount; ++i) {
        const auto& copy = snapshot.copies[i];
        void* base = copy.onController ? controller : client;
        if (!base || copy.offset < 0) continue;
        const int limit = copy.onController ? kControllerScanBytes : kSscScanBytes;
        if (copy.offset + 8 > limit) continue;
        uint64_t val = 0;
        std::memcpy(&val, reinterpret_cast<unsigned char*>(base) + copy.offset, sizeof(val));
        if (val == snapshot.sscSteamId) return true;
    }
    return false;
}

void LogIdentityProbe(const VoteSlotSnapshot& snapshot, const char* when) {
    uint64_t xuid = 0;
    uint64_t engineSid = 0;
    const char* netid = "";
    if (engine && snapshot.slot >= 0) {
        CPlayerSlot playerSlot(snapshot.slot);
        xuid = engine->GetClientXUID(playerSlot);
        if (const CSteamID* sid = engine->GetClientSteamID(playerSlot)) {
            engineSid = sid->ConvertToUint64();
        }
        if (const char* id = engine->GetPlayerNetworkIDString(playerSlot)) {
            netid = id;
        }
    }
    VoteLog("[BotIdentity] vote transaction: identity probe when=%s slot=%d xuid=%llu engine_sid=%llu netid='%s' controller=%p\n",
            when,
            snapshot.slot,
            static_cast<unsigned long long>(xuid),
            static_cast<unsigned long long>(engineSid),
            netid ? netid : "",
            snapshot.controller);
}

// Native bot markers Valve's voter-pool construction is known to consult.
// Do not MarkEntityStateChanged: the scoreboard must not flash BOT / empty SteamID.
// Do not unconditionally write controller +1800: 0.1.2 live reads were a heap pointer.
void ApplyNativeVoteMarkers(VoteSlotSnapshot& snapshot, void* client, void* controller) {
    SetFakePlayer(client);
    if (controller) SetControllerFakeClientFlag(controller);
    ScanAndZeroSteamIdCopies(snapshot, client, controller);
    WriteSteamId(client, 0);
}

void CaptureAndRestoreNativeBotIdentity() {
    for (int slot = 0; slot < kMaxVoteSlots && slot < kMaxSlots; ++slot) {
        auto& snapshot = g_VoteSnapshots[slot];
        snapshot = VoteSlotSnapshot{};
        if (!IdentityMgr().IsManaged(slot)) continue;

        void* client = ResolveClientBySlot(slot);
        if (!client) {
            VoteLog("[BotIdentity] vote transaction: client resolve failed slot=%d\n", slot);
            continue;
        }

        auto* raw = reinterpret_cast<unsigned char*>(client);
        const int entityIndex = GetEntityIndex(client);

        snapshot.captured = true;
        snapshot.slot = slot;
        snapshot.client = client;
        snapshot.entityIndex = entityIndex;
        snapshot.connectionFlags = raw[OFF_m_nConnectionTypeFlags];
        snapshot.fakePlayer = raw[OFF_m_bFakePlayer];
        snapshot.sscSteamId = ReadSteamId(client);

        void* controller = nullptr;
        uint32_t handle = 0;
        if (ResolveController(entityIndex, &controller, &handle)) {
            snapshot.hasController = true;
            snapshot.controller = controller;
            snapshot.controllerHandle = handle;
            snapshot.controllerFlags = *reinterpret_cast<uint32_t*>(
                reinterpret_cast<unsigned char*>(controller) + OFF_Controller_FakeClientFlags);
            snapshot.controllerOff1800 = ReadControllerSteamId(controller);
        }

        ApplyNativeVoteMarkers(snapshot, client, snapshot.hasController ? controller : nullptr);

        char offs[96];
        FormatCopyOffsets(snapshot, offs, sizeof(offs));
        VoteLog("[BotIdentity] vote transaction: native bot identity restored slot=%d ssc_sid=%llu->0 ctrl1800=%llu copies=%d offs=%s controller=%s\n",
                slot,
                static_cast<unsigned long long>(snapshot.sscSteamId),
                static_cast<unsigned long long>(snapshot.controllerOff1800),
                snapshot.copyCount,
                offs,
                snapshot.hasController ? "ok" : "missing");
        LogIdentityProbe(snapshot, "begin");
    }
    g_VoteSnapshotValid = true;
}

void ReassertNativeVoteMarkers() {
    if (!g_VoteSnapshotValid) return;
    for (auto& snapshot : g_VoteSnapshots) {
        if (!snapshot.captured) continue;
        if (!IdentityMgr().IsManaged(snapshot.slot)) continue;

        void* currentClient = ResolveClientBySlot(snapshot.slot);
        if (currentClient != snapshot.client) continue;

        const uint64_t sscNow = ReadSteamId(currentClient);
        void* controller = nullptr;
        const bool controllerLive = SnapshotControllerLive(snapshot, currentClient, &controller);
        const bool copiesBack = CopiesReappeared(snapshot, currentClient, controllerLive ? controller : nullptr);

        if (sscNow != 0 || copiesBack) {
            VoteLog("[BotIdentity] vote transaction: steamid reappeared slot=%d ssc=%llu copies_back=%d; re-zeroed\n",
                    snapshot.slot,
                    static_cast<unsigned long long>(sscNow),
                    copiesBack ? 1 : 0);
        }

        ApplyNativeVoteMarkers(snapshot, currentClient, controllerLive ? controller : nullptr);
        if (!snapshot.holdProbed) {
            LogIdentityProbe(snapshot, "hold");
            snapshot.holdProbed = true;
        }
    }
}

void ApplyControllerBit(void* controller, bool setBit) {
    if (setBit)
        SetControllerFakeClientFlag(controller);
    else
        ClearControllerFakeClientFlag(controller);
    MarkEntityStateChanged(controller);
}

void RestorePlayerIdentity() {
    if (!g_VoteSnapshotValid) return;
    g_VoteSnapshotValid = false;

    int restored = 0;
    for (auto& snapshot : g_VoteSnapshots) {
        if (!snapshot.captured) continue;
        snapshot.captured = false;
        if (!IdentityMgr().IsManaged(snapshot.slot)) continue;

        void* currentClient = ResolveClientBySlot(snapshot.slot);
        if (currentClient != snapshot.client) {
            VoteLog("[BotIdentity] vote transaction: restore skipped slot=%d: client rebound\n", snapshot.slot);
            continue;
        }

        // Replay BotIdentity's own disguise writes (the same transforms that
        // ApplyDisguise performs on connect), then put back the SteamID copies
        // this window zeroed. Engine-owned flag state outside those writes is
        // preserved. Do not MarkEntityStateChanged for SteamID: clients never
        // saw the transient zero.
        ClearFakePlayer(currentClient);
        WriteSteamId(currentClient, snapshot.sscSteamId);

        void* controller = nullptr;
        const bool controllerLive = SnapshotControllerLive(snapshot, currentClient, &controller);
        if (controllerLive) {
            ApplyControllerBit(controller, false);
        } else if (snapshot.hasController) {
            VoteLog("[BotIdentity] vote transaction: controller rebound slot=%d; redisguise deferred\n",
                    snapshot.slot);
        }
        RestoreSteamIdCopies(snapshot, currentClient, controllerLive ? controller : nullptr);
        ++restored;
    }

    VoteLog("[BotIdentity] vote transaction: player identity restored on %d slots\n", restored);
}

}  // namespace

void BeginVoteTransaction() {
    if (g_VoteDepth++ == 0) {
        CaptureAndRestoreNativeBotIdentity();
        VoteLog("[BotIdentity] vote transaction begin\n");
    }
}

void EndVoteTransaction() {
    if (g_VoteDepth == 0) {
        VoteLog("[BotIdentity] warning: vote transaction end without begin\n");
        return;
    }
    g_EndScheduled = false;
    g_HoldFramesRemaining = 0;
    if (--g_VoteDepth == 0) {
        RestorePlayerIdentity();
        VoteLog("[BotIdentity] vote transaction end\n");
    }
}

void ScheduleVoteTransactionEnd(int frames) {
    if (g_VoteDepth == 0) {
        VoteLog("[BotIdentity] vote transaction: schedule ignored, not active\n");
        return;
    }
    if (frames < 1) frames = 1;
    if (frames > 32) frames = 32;
    g_HoldFramesRemaining = frames;
    g_EndScheduled = true;
    VoteLog("[BotIdentity] vote transaction: holding native identity for %d frames\n", frames);
}

void TickVoteTransaction() {
    if (g_VoteDepth == 0 && !g_EndScheduled) return;

    bool serverAlive = false;
    if (g_pNetworkServerService)
        serverAlive = g_pNetworkServerService->GetIGameServer() != nullptr;
    if (!serverAlive) {
        VoteLog("[BotIdentity] vote transaction: game server gone, dropping snapshots\n");
        ResetVoteTransaction();
        return;
    }

    if (!g_EndScheduled) return;
    if (g_HoldFramesRemaining > 1) {
        ReassertNativeVoteMarkers();
        --g_HoldFramesRemaining;
        VoteLog("[BotIdentity] vote transaction: hold tick remaining=%d\n", g_HoldFramesRemaining);
        return;
    }

    g_HoldFramesRemaining = 0;
    g_EndScheduled = false;
    g_VoteDepth = 0;
    RestorePlayerIdentity();
    VoteLog("[BotIdentity] vote transaction end\n");
}

bool VoteTransactionActive() { return g_VoteDepth != 0; }

void ResetVoteTransaction() {
    g_VoteDepth = 0;
    g_EndScheduled = false;
    g_HoldFramesRemaining = 0;
    g_VoteSnapshotValid = false;
    for (auto& snapshot : g_VoteSnapshots) snapshot = VoteSlotSnapshot{};
}

void ResetVoteTransactionSlot(int slot) {
    if (slot < 0 || slot >= kMaxVoteSlots) return;
    auto& snapshot = g_VoteSnapshots[slot];
    if (!snapshot.captured) return;
    // The slot is leaving our control while native bot markers are applied;
    // the disconnect path's RestoreIdentity will bring the engine back in sync.
    snapshot = VoteSlotSnapshot{};
    VoteLog("[BotIdentity] vote transaction: snapshot dropped for slot=%d\n", slot);
}

}  // namespace botid
