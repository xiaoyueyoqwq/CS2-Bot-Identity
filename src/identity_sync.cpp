#include "identity_sync.h"

#include "bot_info.h"
#include "entity_access.h"
#include "plugin.h"
#include "ssc_ops.h"
#include "vote_transaction.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include <entity2/entityinstance.h>

namespace botid {
namespace {

void SyncLog(const char* fmt, ...) {
    if (!g_BotIdentityPlugin.ismm_) return;
    char buffer[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    g_BotIdentityPlugin.ismm_->ConPrintf("[BotIdentity] %s", buffer);
}

constexpr int kControllerScanBytes = 4096;
constexpr int kMaxXuidOffsets = 8;

struct XuidSyncState {
    void* controller = nullptr;
    uint32_t controllerHandle = 0;
    // SSC SteamID captured at connect, before the disguise overwrote it.
    // Stale copies of this value on the controller are what clients show
    // as "?" / wrong avatars while the engine defers its own sync.
    uint64_t rawXuid = 0;
    int offsets[kMaxXuidOffsets] = {};
    int offsetCount = 0;
    bool logged = false;
};

XuidSyncState g_XuidSync[kMaxSlots];

// SteamID64s (real profiles 0x0110..., raw bot ids like 0x0140...) sit
// in the 0x0100..0x0200 <<48 band. Heap/code pointers on x64 Linux never
// reach it — a value failing this check is pointer-shaped and must
// never be overwritten.
bool LooksLikeSteamId64(uint64_t v) {
    const uint64_t hi = v >> 48;
    return hi >= 0x0100 && hi <= 0x0200;
}

bool SafeIdentityValue(uint64_t v) {
    return v == 0 || LooksLikeSteamId64(v);
}

// Same byte scan the vote window uses: every qword == value inside the
// first nbytes of base. Copies are not required to be 8-aligned.
int FindValueOffsets(const void* base, int nbytes, uint64_t value, int* out, int cap) {
    auto* raw = reinterpret_cast<const unsigned char*>(base);
    int n = 0;
    for (int off = 0; off + 8 <= nbytes && n < cap; ++off) {
        uint64_t v = 0;
        std::memcpy(&v, raw + off, sizeof(v));
        if (v != value) continue;
        bool overlap = false;
        for (int i = 0; i < n; ++i) {
            const int delta = out[i] - off;
            if (delta > -8 && delta < 8) { overlap = true; break; }
        }
        if (!overlap) out[n++] = off;
        off += 7;
    }
    return n;
}

void AddOffset(XuidSyncState& st, int off) {
    for (int i = 0; i < st.offsetCount; ++i) {
        const int delta = st.offsets[i] - off;
        if (delta > -8 && delta < 8) return;
    }
    if (st.offsetCount < kMaxXuidOffsets) st.offsets[st.offsetCount++] = off;
}

void* ResolveManagedController(void* client, uint32_t* outHandle) {
    *outHandle = 0;
    const int entityIndex = GetEntityIndex(client);
    if (entityIndex <= 0 || entityIndex >= 0x8000) return nullptr;
    char className[64] = {0};
    void* controller = ResolveEntityInstance(entityIndex, className, sizeof(className));
    if (!controller || std::strcmp(className, "cs_player_controller") != 0)
        return nullptr;
    auto* entity = reinterpret_cast<CEntityInstance*>(controller);
    *outHandle = static_cast<uint32_t>(entity->GetRefEHandle().ToInt());
    return controller;
}

void LearnControllerOffsets(int slot, XuidSyncState& st, void* controller, uint64_t steamId) {
    int hits[kMaxXuidOffsets];

    // Engine already propagated the disguise somewhere — record where.
    int n = FindValueOffsets(controller, kControllerScanBytes, steamId, hits, kMaxXuidOffsets);
    for (int i = 0; i < n; ++i) AddOffset(st, hits[i]);

    // Raw client xuid copies are stale visible identities — record so
    // the verify pass overwrites them with the disguise.
    if (st.rawXuid != 0 && st.rawXuid != steamId) {
        n = FindValueOffsets(controller, kControllerScanBytes, st.rawXuid, hits, kMaxXuidOffsets);
        for (int i = 0; i < n; ++i) AddOffset(st, hits[i]);
    }

    // Same class → same layout: borrow offsets learned on another bot.
    if (st.offsetCount == 0) {
        for (int s = 0; s < kMaxSlots && st.offsetCount == 0; ++s) {
            const XuidSyncState& donor = g_XuidSync[s];
            if (donor.offsetCount == 0 || donor.controller == controller) continue;
            for (int i = 0; i < donor.offsetCount; ++i) {
                const int off = donor.offsets[i];
                uint64_t v = 0;
                std::memcpy(&v, reinterpret_cast<unsigned char*>(controller) + off, sizeof(v));
                if (SafeIdentityValue(v)) AddOffset(st, off);
            }
            if (st.offsetCount > 0) {
                SyncLog("identity sync: borrowed %d offset(s) from slot=%d for slot=%d\n",
                        st.offsetCount, s, slot);
            }
        }
    }

    // Documented schema offset as the last resort — only if the field
    // currently holds an empty/steamid-shaped value (a heap pointer at
    // +1800 on an old build proved this offset can drift).
    if (st.offsetCount == 0) {
        uint64_t v = 0;
        std::memcpy(&v, reinterpret_cast<unsigned char*>(controller) + OFF_Controller_SteamID, sizeof(v));
        if (SafeIdentityValue(v)) {
            AddOffset(st, OFF_Controller_SteamID);
            SyncLog("identity sync: fallback schema offset %d slot=%d\n",
                    OFF_Controller_SteamID, slot);
        }
    }
}

// Returns true if this pass wrote anything to the controller.
bool SyncOne(int slot, BotIdentity* identity) {
    XuidSyncState& st = g_XuidSync[slot];
    void* client = ResolveClientBySlot(slot);
    if (!client) return false;

    // Late capture before the belt rewrite: when the disguise landed
    // late or the SSC got stomped back, the raw value is still (or
    // again) what the client carries.
    if (st.rawXuid == 0) st.rawXuid = ReadSteamId(client);

    // Belt: engine stomps on the SSC pair get rewritten immediately.
    if (ReadSteamId(client) != identity->steamId)
        WriteSteamId(client, identity->steamId);

    uint32_t handle = 0;
    void* controller = ResolveManagedController(client, &handle);
    if (!controller) {
        st.controller = nullptr;
        st.controllerHandle = 0;
        return false;
    }
    if (controller != st.controller || handle != st.controllerHandle) {
        st.controller = controller;
        st.controllerHandle = handle;
        st.offsetCount = 0;  // recreated object → relearn
        st.logged = false;
    }
    if (st.offsetCount == 0)
        LearnControllerOffsets(slot, st, controller, identity->steamId);

    bool dirty = false;
    for (int i = 0; i < st.offsetCount; ++i) {
        unsigned char* p = reinterpret_cast<unsigned char*>(controller) + st.offsets[i];
        uint64_t v = 0;
        std::memcpy(&v, p, sizeof(v));
        if (v == identity->steamId) continue;
        if (!SafeIdentityValue(v)) {
            // Mis-learned, or the layout shifted under us — drop it.
            st.offsets[i] = st.offsets[--st.offsetCount];
            --i;
            continue;
        }
        std::memcpy(p, &identity->steamId, sizeof(v));
        dirty = true;
    }
    if (!dirty) return false;

    MarkEntityStateChanged(controller);
    if (!st.logged) {
        st.logged = true;
        SyncLog("identity sync: pinned xuid slot=%d steamid=%llu offsets=%d\n",
                slot, static_cast<unsigned long long>(identity->steamId), st.offsetCount);
    }
    return true;
}

}  // namespace

void NotifyIdentityBound(int slot) {
    if (slot < 0 || slot >= kMaxSlots) return;
    XuidSyncState& st = g_XuidSync[slot];
    st = XuidSyncState{};
    void* client = ResolveClientBySlot(slot);
    if (client) st.rawXuid = ReadSteamId(client);
}

void NotifyIdentityDisconnected(int slot) {
    if (slot < 0 || slot >= kMaxSlots) return;
    g_XuidSync[slot] = XuidSyncState{};
}

void SyncControllerIdentity(int slot, BotIdentity* identity) {
    if (slot < 0 || slot >= kMaxSlots || !identity) return;
    if (VoteTransactionActive()) return;
    SyncOne(slot, identity);
}

void TickIdentitySync() {
    if (VoteTransactionActive()) return;
    for (int slot = 0; slot < kMaxSlots; ++slot) {
        if (!IdentityMgr().IsManaged(slot)) continue;
        BotIdentity* identity = IdentityMgr().GetIdentity(slot);
        if (!identity || !identity->applied) continue;
        SyncOne(slot, identity);
    }
}

void ResetIdentitySync() {
    for (auto& st : g_XuidSync) st = XuidSyncState{};
}

}  // namespace botid
