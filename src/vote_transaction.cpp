#include "vote_transaction.h"

#include "bot_info.h"
#include "entity_access.h"
#include "plugin.h"
#include "shm_pub.h"
#include "ssc_ops.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include <entity2/entityinstance.h>

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

struct VoteSlotSnapshot {
    bool captured = false;
    int slot = -1;
    void* client = nullptr;
    int entityIndex = -1;
    unsigned char connectionFlags = 0;
    unsigned char fakePlayer = 0;
    bool hasController = false;
    void* controller = nullptr;
    uint32_t controllerFlags = 0;
    uint32_t controllerHandle = 0;
};

constexpr int kMaxVoteSlots = 64;

unsigned int g_VoteDepth = 0;
VoteSlotSnapshot g_VoteSnapshots[kMaxVoteSlots];
bool g_VoteSnapshotValid = false;

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

        // Restore Valve's native bot markers so the voter pool excludes us.
        SetFakePlayer(client);

        void* controller = nullptr;
        uint32_t handle = 0;
        if (ResolveController(entityIndex, &controller, &handle)) {
            snapshot.hasController = true;
            snapshot.controller = controller;
            snapshot.controllerHandle = handle;
            snapshot.controllerFlags = *reinterpret_cast<uint32_t*>(
                reinterpret_cast<unsigned char*>(controller) + OFF_Controller_FakeClientFlags);
            SetControllerFakeClientFlag(controller);
            MarkEntityStateChanged(controller);
        }

        VoteLog("[BotIdentity] vote transaction: native bot identity restored slot=%d controller=%s\n",
                       slot, snapshot.hasController ? "ok" : "missing");
    }
    g_VoteSnapshotValid = true;
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
        // ApplyDisguise performs on connect); engine-owned flag state is
        // preserved.
        ClearFakePlayer(currentClient);

        if (snapshot.hasController) {
            const int entityIndex = GetEntityIndex(currentClient);
            void* controller = nullptr;
            uint32_t handle = 0;
            if (ResolveController(entityIndex, &controller, &handle) &&
                controller == snapshot.controller && handle == snapshot.controllerHandle) {
                ApplyControllerBit(controller, false);
            } else {
                VoteLog("[BotIdentity] vote transaction: controller rebound slot=%d; redisguise deferred\n",
                               snapshot.slot);
            }
        }
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
    if (--g_VoteDepth == 0) {
        RestorePlayerIdentity();
        VoteLog("[BotIdentity] vote transaction end\n");
    }
}

bool VoteTransactionActive() { return g_VoteDepth != 0; }

void ResetVoteTransaction() {
    g_VoteDepth = 0;
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
