#include "controller_reap.h"

#include "bot_info.h"
#include "entity_access.h"
#include "identity_sync.h"
#include "plugin.h"
#include "shm_pub.h"
#include "ssc_ops.h"
#include "vote_transaction.h"

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include <eiface.h>
#include <entity2/entityidentity.h>
#include <entity2/entityinstance.h>
#include <entityhandle.h>
#include <interfaces/interfaces.h>
#include <iserver.h>
#include <networkstringtabledefs.h>
#include <playerslot.h>

class INetworkServerService;
extern INetworkServerService* g_pNetworkServerService;
extern IVEngineServer* engine;

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

// bot_kick can strand "challenging" client records inside m_Clients
// (netchan=nil, userid 65535, signon 0, steamid 0, conn state ~0x08).
// Once live clients occupy higher slots the dead record becomes an
// interior hole that tail shrink can never reach, and the engine keeps
// listing it to clients: each one renders as a ghost standin model on
// the team-select screen. Zeroing m_Clients[slot] segfaults libengine2
// (0.1.19), so release through INetworkGameServer::DisconnectClient.
// A brand-new record can match the fingerprint for a few frames while
// connecting, so require persistence across ~2s of wall clock first —
// hibernation drops GameFrame to ~2fps and a pass-count dwell stretches
// into a minute.
constexpr double kLeftoverDwellSeconds = 2.0;
constexpr double kLeftoverRetrySeconds = 4.0;
constexpr int kLeftoverMaxAttempts = 3;
// During add/kick churn a corpse's m_nEntityIndex can briefly read
// unbound or resolve to a recycled live controller; a single
// alive-looking pass must not disarm the dead watch or the dwell never
// matures. Require sustained "alive" before resetting.
constexpr double kLeftoverAliveResetSeconds = 3.0;

struct LeftoverClientWatch {
    double firstSeenTime = 0;
    int attempts = 0;
    double lastAttemptTime = 0;
    double aliveSince = 0;
    int controllerAttempts = 0;
    double lastControllerAttempt = 0;
};

LeftoverClientWatch g_LeftoverWatch[kMaxSlots];
int g_DrainPass = 0;

double NowSeconds() {
    return std::chrono::duration<double>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void ReapLeftoverClients() {
    if (!GameServerAlive() || !engine) return;
    const int count = ClientListCount();
    for (int slot = 0; slot < count && slot < kMaxSlots; ++slot) {
        LeftoverClientWatch& w = g_LeftoverWatch[slot];
        void* client = ResolveClientBySlot(slot);
        // Dead = netchannel gone + conn&0x08 (engine teardown flag). Live
        // records never show conn&0x08: managed bots are 0x01, humans 0x04,
        // SourceTV 0x00. Managed/steamid no longer gate the check: a stalled
        // teardown can still carry managed=1, and a vote-transaction restore
        // that fired on a corpse re-writes steamid onto it — both variants
        // are dead records that must not linger in CMsgPlayerInfo (signon>1
        // => client renders a standin model that persists until its scene
        // is rebuilt).
        const int signon = client ? ReadSignonState(client) : 0;
        const bool dead = client && RecordLooksDead(client);
        const bool ghost = dead && signon > 1;
        const bool invisible = dead && signon <= 1;
        const double now = NowSeconds();
        if (!ghost && !invisible) {
            if (w.firstSeenTime != 0) {
                if (w.aliveSince == 0) w.aliveSince = now;
                if (now - w.aliveSince >= kLeftoverAliveResetSeconds) {
                    ReapLog("leftover watch reset slot=%d alive=%.1fs\n",
                            slot, now - w.aliveSince);
                    w.firstSeenTime = 0;
                    w.attempts = 0;
                    w.lastAttemptTime = 0;
                    w.aliveSince = 0;
                    w.controllerAttempts = 0;
                    w.lastControllerAttempt = 0;
                }
            }
            continue;
        }
        w.aliveSince = 0;
        if (w.firstSeenTime == 0) {
            w.firstSeenTime = now;
            ReapLog("leftover watch arm slot=%d signon=%d entIdx=%d\n",
                    slot, signon, client ? GetEntityIndex(client) : -1);
            continue;
        }
        if (now - w.firstSeenTime < kLeftoverDwellSeconds) continue;
        // A teardown stalled before OnClientDisconnect leaves managed=1 and
        // a live vote-transaction snapshot on a dead record: the snapshot's
        // restore can resurrect the disguise SteamID onto the corpse and the
        // managed mark blocks tail shrink forever. The record is dead past
        // dwell either way, so release the bookkeeping exactly as the
        // disconnect path would have.
        if (IdentityMgr().IsManaged(slot)) {
            ReapLog("leftover managed bookkeeping release slot=%d signon=%d\n",
                    slot, signon);
            ResetVoteTransactionSlot(slot);
            NotifyIdentityDisconnected(slot);
            IdentityMgr().Unmark(slot);
            PublishRelease(slot);
        }
        // A corpse can still bind a live orphan controller (teardown stalled
        // before the entity was destroyed). The controller is networked with
        // its team/SteamID fields, so clients keep rendering its preview
        // model even after the record drops out of CMsgPlayerInfo — that is
        // the ghost that survives until a client-side rebuild. Route it
        // through the verified deferred-destroy path so the entity delete
        // reaches every connected client. The queue is retried on a slow
        // cadence because a mid-recycle entIdx can fail transiently — but a
        // recycled index never resolves again, so give up after a few tries
        // instead of logging every drain pass forever.
        if (w.controllerAttempts < kLeftoverMaxAttempts &&
            (w.lastControllerAttempt == 0 ||
             now - w.lastControllerAttempt >= kLeftoverRetrySeconds)) {
            w.lastControllerAttempt = now;
            ++w.controllerAttempts;
            QueueControllerRemovalForClient(client, slot);
        }
        if (!ghost) continue;  // signon<=1 records stay; tail shrink owns them
        if (w.attempts < kLeftoverMaxAttempts &&
            (w.lastAttemptTime == 0 ||
             now - w.lastAttemptTime >= kLeftoverRetrySeconds)) {
            w.lastAttemptTime = now;
            ++w.attempts;
            ReapLog("leftover ghost reap slot=%d attempt=%d signon=%d\n",
                    slot, w.attempts, signon);
            // client->Disconnect (vtbl[8]) gates on signon!=0 — for signon>1
            // records it runs the real path: resets signon, fires the
            // client_disconnected event, and notifies the server so
            // connected clients also drop the stale playerinfo entry.
            auto** vtbl = *reinterpret_cast<void***>(client);
            auto* disconnect =
                reinterpret_cast<void (*)(void*, int, const char*)>(vtbl[8]);
            disconnect(client, 3, "BotIdentity leftover ghost reap");
        }
        // Belt & suspenders, applied unconditionally: a record that survives
        // maxAttempts still gets forced into the canonical "challenging"
        // shape every pass (signon<=1 drops it from CMsgPlayerInfo;
        // userid=65535 + steamid=0 lets the trailing-slot shrink own it).
        if (ReadSignonState(client) > 1) {
            const int zero = 0;
            std::memcpy(reinterpret_cast<unsigned char*>(client) + OFF_m_nSignonState,
                        &zero, sizeof(zero));
        }
        if (ReadUserId(client) != 65535) {
            WriteUserId(client, 65535);
        }
        // Resurrected corpses can still carry a disguise SteamID; the
        // challenging shape requires steamid=0 for tail shrink to own it.
        if (ReadSteamId(client) != 0) {
            WriteSteamId(client, 0);
        }
    }

    // Out-of-count corpse scanning is disabled: elements past Count() are
    // stale pointers to records that may be freed or mid-rebind for a new
    // connection (a connecting record has netch=nil + unset conn flags and
    // matches the dead fingerprint), so reading them can segfault and
    // normalizing them races the engine's own fill-in — it crashed the
    // server on bot_add. NormalizeCorpseNow covers the disconnect path.
}

// ---------------------------------------------------------------------------
// Team member vector sweep.
//
// CTeam::m_aPlayerControllers (+1936) and CTeam::m_aPlayers (+1960) are
// C_NetworkUtlVectorBase<CHandle<>> member lists that replicate to clients;
// the client builds the team-select roster by resolving each element. Bot
// teardown stalls leave dead/orphaned handles in these vectors — the client
// renders a preview model for every entry it cannot resolve (falling back to
// the local player's loadout, which is why ghosts wear the human's skin).
// An INVALID_EHANDLE placeholder still resolves to a fallback model, so
// reaped entries are compacted out of the vector entirely and the manager
// gets MarkEntityStateChanged so connected clients receive the delta.
// New elements must stay stale across kLeftoverDwellPasses before writing,
// same race guard as the client reap.
constexpr int kTeamMemberVecOffsets[] = {1936, 1960};
constexpr uint32_t kInvalidMemberHandle = 0xFFFFFFFFu;

struct StaleMemberWatch {
    uint64_t key = 0;
    uint32_t handle = 0;
    double firstSeenTime = 0;
};

std::vector<StaleMemberWatch> g_StaleMemberWatch;

uint64_t MemberWatchKey(int entityIndex, int fieldOff, int elemIdx) {
    return (static_cast<uint64_t>(entityIndex) << 32) |
           (static_cast<uint64_t>(fieldOff) << 16) |
           static_cast<uint64_t>(elemIdx);
}

bool ClientRecordAlive(void* client) {
    return !RecordLooksDead(client);
}

// A controller with no live owning client record is an orphan (its record
// died mid-teardown). Such controllers keep rendering team-select ghosts.
bool IsOrphanControllerInstance(void* controller) {
    for (int slot = 0; slot < kMaxSlots; ++slot) {
        void* client = ResolveClientBySlot(slot);
        if (!client || !ClientRecordAlive(client)) continue;
        const int entityIndex = GetEntityIndex(client);
        char className[64] = {0};
        void* current = ResolveEntityInstance(entityIndex, className, sizeof(className));
        if (current == controller &&
            std::strcmp(className, "cs_player_controller") == 0) {
            return false;
        }
    }
    return true;
}

// Returns how many elements were swept in this vector.
int SweepMemberVector(unsigned char* managerRaw, int fieldOff, int managerIdx) {
    void* mem = nullptr;
    int32_t count = 0;
    std::memcpy(&mem, managerRaw + fieldOff, sizeof(mem));
    std::memcpy(&count, managerRaw + fieldOff + 8, sizeof(count));
    if (!mem || count <= 0 || count > 64) return 0;

    auto* handles = reinterpret_cast<uint32_t*>(mem);
    bool sweptFlag[64] = {};
    int swept = 0;
    for (int i = 0; i < count; ++i) {
        const uint32_t h = handles[i];
        const uint64_t key = MemberWatchKey(managerIdx, fieldOff, i);
        auto findWatch = [&]() -> StaleMemberWatch* {
            for (auto& w : g_StaleMemberWatch) {
                if (w.key == key) return &w;
            }
            return nullptr;
        };
        if (h == 0 || h == kInvalidMemberHandle) {
            if (StaleMemberWatch* w = findWatch()) w->firstSeenTime = 0;
            continue;
        }
        // The vector's capacity tail contains uninitialized memory, not
        // handles: values like 0x00000272 or 0x1b4c5000 have no serial bits
        // or out-of-range indices. Skip anything that cannot be a real
        // CHandle<CEntityInstance> — writing over it would dirty heap data
        // the engine still owns.
        const int idx = static_cast<int>(h & 0x3FFFu);
        if (idx <= 0 || idx >= 4096 || (h >> 14) == 0) {
            if (StaleMemberWatch* w = findWatch()) w->firstSeenTime = 0;
            continue;
        }
        char className[64] = {0};
        void* target =
            idx > 0 ? ResolveEntityInstance(idx, className, sizeof(className)) : nullptr;
        const char* why = nullptr;
        if (!target) {
            why = "dead";
        } else {
            auto* te = reinterpret_cast<CEntityInstance*>(target);
            const uint32_t liveHandle =
                static_cast<uint32_t>(te->GetRefEHandle().ToInt());
            if (liveHandle != h) {
                why = "serial";
            } else if (IsEntityBeingDeleted(target)) {
                why = "deleting";
            } else if (std::strcmp(className, "cs_player_controller") == 0 &&
                       IsOrphanControllerInstance(target)) {
                why = "orphan";
            }
        }
        if (!why) {
            if (StaleMemberWatch* w = findWatch()) w->firstSeenTime = 0;
            continue;
        }
        const double now = NowSeconds();
        StaleMemberWatch* w = findWatch();
        if (!w) {
            g_StaleMemberWatch.push_back({key, h, now});
            continue;
        }
        if (w->handle != h) {
            w->handle = h;
            w->firstSeenTime = now;
            continue;
        }
        if (now - w->firstSeenTime < kLeftoverDwellSeconds) continue;
        ReapLog("team member sweep manager=%d +%d[%d]=0x%08x idx=%d %s\n",
                managerIdx, fieldOff, i, h, idx, why);
        sweptFlag[i] = true;
        w->firstSeenTime = 0;
        ++swept;
    }
    if (swept > 0) {
        // Compact: clients appear to render a preview model for every
        // element in these vectors — an INVALID handle still draws the
        // local-loadout fallback, so swept entries must leave the array,
        // not just be invalidated.
        int write = 0;
        for (int i = 0; i < count; ++i) {
            if (sweptFlag[i]) continue;
            handles[write++] = handles[i];
        }
        for (int i = write; i < count; ++i) handles[i] = kInvalidMemberHandle;
        std::memcpy(managerRaw + fieldOff + 8, &write, sizeof(write));
        ReapLog("team member sweep compact manager=%d +%d count=%d->%d\n",
                managerIdx, fieldOff, count, write);
    }
    return swept;
}

void SweepTeamManagerMembers() {
    if (!GameServerAlive()) {
        g_StaleMemberWatch.clear();
        return;
    }
    for (int entityIndex = 1; entityIndex <= 2048; ++entityIndex) {
        char className[64] = {0};
        void* instance =
            ResolveEntityInstance(entityIndex, className, sizeof(className));
        if (!instance) continue;
        if (std::strcmp(className, "cs_team_manager") != 0) continue;
        auto* raw = reinterpret_cast<unsigned char*>(instance);
        int swept = 0;
        for (int fieldOff : kTeamMemberVecOffsets) {
            swept += SweepMemberVector(raw, fieldOff, entityIndex);
        }
        if (swept > 0) {
            MarkEntityStateChanged(instance);
            ReapLog("team member sweep entIdx=%d swept=%d\n", entityIndex, swept);
        }
    }
    // Drop watches for slots that no longer exist (vector shrank or manager
    // entity was recycled) — a stale key with firstSeenPass=-1 is dormant,
    // but entries whose element index fell out of range are pure noise.
    if (g_StaleMemberWatch.size() > 256) {
        g_StaleMemberWatch.erase(
            std::remove_if(g_StaleMemberWatch.begin(), g_StaleMemberWatch.end(),
                           [](const StaleMemberWatch& w) {
                               return w.firstSeenTime == 0;
                           }),
            g_StaleMemberWatch.end());
    }
}

bool ClientLooksLeftoverOrEmpty(int slot) {
    // Check the native-owned mark before client resolution: a managed bot
    // must never be reaped, even if m_Clients resolution is broken.
    if (IdentityMgr().IsManaged(slot)) return false;
    void* client = ResolveClientBySlot(slot);
    if (!client) return true;
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
    // leftover path after kick already marked delete. A stale vtable index
    // can return success without moving the team (1.41.9.7 shifted
    // ChangeTeam 102 -> 104); verify the field changed before trusting it.
    if (!IsEntityBeingDeleted(controller) &&
        CallControllerChangeTeam(controller, kSpectatorTeam) &&
        ReadControllerTeam(controller) != team) {
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

// A record is dead when it carries the marked-teardown fingerprint
// (netch=nil && conn&0x08, set by our own fake-player mark surviving on a
// corpse) OR its entity binding resolves to a dead/recycled entity. The
// entity check catches corpses that stall before conn ever reaches 0x08:
// they keep conn=0x01 + managed=1 + a disguise SteamID and look identical
// to a live managed bot on every field — except m_nEntityIndex, which a
// live client always points at its live cs_player_controller.
bool RecordLooksDead(void* client) {
    if (!client) return true;
    if (ReadNetChannel(client) == nullptr &&
        (ReadConnectionTypeFlags(client) & 0x08) != 0) {
        return true;
    }
    const int idx = GetEntityIndex(client);
    if (idx <= 0) return false;  // unbound — cannot judge
    char className[64] = {0};
    void* instance = ResolveEntityInstance(idx, className, sizeof(className));
    if (!instance) return true;
    if (std::strcmp(className, "cs_player_controller") != 0) return true;
    return IsEntityBeingDeleted(instance);
}

void TestLeftoverRelease(int slot, int mode, int reason) {
    if (mode == 20) {
        // Enumerate server string tables and dump the userinfo table —
        // ghost roster entries should persist there if the client-side
        // preview models are driven by CMsgPlayerInfo string replication.
        auto* ismm = g_BotIdentityPlugin.ismm_;
        if (!ismm) { ReapLog("stringtables: no ismm\n"); return; }
        auto factory = ismm->GetEngineFactory(false);
        if (!factory) { ReapLog("stringtables: no factory\n"); return; }
        auto* container = reinterpret_cast<INetworkStringTableContainer*>(
            factory(INTERFACENAME_NETWORKSTRINGTABLESERVER, nullptr));
        if (!container) { ReapLog("stringtables: container null\n"); return; }
        const int nTables = container->GetNumTables();
        ReapLog("stringtables: %d tables\n", nTables);
        for (int t = 0; t < nTables; ++t) {
            auto* tbl = container->GetTable(t);
            if (!tbl) continue;
            ReapLog("  table[%d] id=%d name='%s' strings=%d\n", t,
                    tbl->GetTableId(), tbl->GetTableName(), tbl->GetNumStrings());
        }
        const char* tableNames[] = {"userinfo", "userinfo2", "player_info"};
        for (const char* tn : tableNames) {
            auto* tbl = container->FindTable(tn);
            if (!tbl) continue;
            ReapLog("stringtables: '%s' entries:\n", tn);
            for (int i = 0; i < tbl->GetNumStrings(); ++i) {
                const char* s = tbl->GetString(i);
                auto* ud = tbl->GetStringUserData(i);
                ReapLog("  entry[%d] '%s' udlen=%u\n", i, s ? s : "?",
                        ud ? ud->m_cbDataSize : 0u);
            }
        }
        return;
    }
    if (mode == 13) {
        // Entity census: count live instances per class name, then list every
        // entity whose class suggests a player/preview body (controller, pawn,
        // character, mannequin, team_select) with index/team/handle.
        int census[2048][2] = {};
        char names[2048][64];
        int classCount = 0;
        for (int entityIndex = 1; entityIndex <= 8192; ++entityIndex) {
            char className[64] = {0};
            void* instance =
                ResolveEntityInstance(entityIndex, className, sizeof(className));
            if (!instance) continue;
            int ci = -1;
            for (int i = 0; i < classCount; ++i) {
                if (std::strcmp(names[i], className) == 0) { ci = i; break; }
            }
            if (ci < 0 && classCount < 2048) {
                std::strncpy(names[classCount], className, 63);
                ci = classCount++;
            }
            if (ci >= 0) census[ci][0] += 1;
            if (std::strstr(className, "player") ||
                std::strstr(className, "character") ||
                std::strstr(className, "preview") ||
                std::strstr(className, "mannequin") ||
                std::strstr(className, "team_select") ||
                std::strstr(className, "pawn")) {
                auto* entity = reinterpret_cast<CEntityInstance*>(instance);
                ReapLog("census-entity entIdx=%d cls='%s' team=%u handle=0x%08x deleting=%d\n",
                        entityIndex, className,
                        static_cast<unsigned int>(ReadControllerTeam(instance)),
                        static_cast<uint32_t>(entity->GetRefEHandle().ToInt()),
                        IsEntityBeingDeleted(instance) ? 1 : 0);
            }
        }
        for (int i = 0; i < classCount; ++i) {
            ReapLog("census cls='%s' count=%d\n", names[i], census[i][0]);
        }
        return;
    }
    if (mode == 12) {
        // Raw entity hex dump: <entIdx> 12 — dump non-zero qwords of the
        // resolved instance to spot fields the structured probe misses.
        const int entityIndex = slot;
        char className[64] = {0};
        void* instance = ResolveEntityInstance(entityIndex, className, sizeof(className));
        if (!instance) {
            ReapLog("entity dump entIdx=%d: resolve failed\n", entityIndex);
            return;
        }
        ReapLog("entity dump entIdx=%d cls='%s' inst=%p\n", entityIndex, className, instance);
        const auto* raw = reinterpret_cast<const unsigned char*>(instance);
        for (int off = 8; off + 8 <= 3136; off += 8) {
            uint64_t v = 0;
            std::memcpy(&v, raw + off, 8);
            if (v != 0) {
                ReapLog("  +%d=0x%llx\n", off,
                        static_cast<unsigned long long>(v));
            }
        }
        return;
    }
    void* client = ResolveClientBySlot(slot);
    if (!client) {
        ReapLog("leftover test slot=%d: no record\n", slot);
        return;
    }
    ReapLog("leftover test slot=%d mode=%d reason=%d pre: userid=%u signon=%d steam=%llu netch=%p fake=%u conn=0x%02x\n",
            slot, mode, reason,
            static_cast<unsigned int>(ReadUserId(client)),
            ReadSignonState(client),
            static_cast<unsigned long long>(ReadSteamId(client)),
            ReadNetChannel(client),
            IsFakePlayerSet(client) ? 1u : 0u,
            static_cast<unsigned int>(ReadConnectionTypeFlags(client)));
    if (!engine) {
        ReapLog("leftover test slot=%d: engine null\n", slot);
        return;
    }
    switch (mode) {
    case 0:
        ReapLog("leftover test slot=%d: DisconnectClient r=%d\n", slot, reason);
        engine->DisconnectClient(CPlayerSlot(slot),
                                 static_cast<ENetworkDisconnectionReason>(reason),
                                 "BotIdentity test");
        break;
    case 1:
        ReapLog("leftover test slot=%d: KickClient r=%d\n", slot, reason);
        engine->KickClient(CPlayerSlot(slot), "BotIdentity test",
                           static_cast<ENetworkDisconnectionReason>(reason));
        break;
    case 2:
        SetFakePlayer(client);
        ReapLog("leftover test slot=%d: SetFakePlayer done, now fire bot_kick\n", slot);
        break;
    case 3: {
        // Stage the record as mid-connect: signon=<reason> keeps userid
        // sentinel, then engine DisconnectClient.
        const int signon = reason;
        std::memcpy(reinterpret_cast<unsigned char*>(client) + OFF_m_nSignonState,
                    &signon, sizeof(signon));
        ReapLog("leftover test slot=%d: wrote signon=%d, DisconnectClient r=3\n",
                slot, signon);
        engine->DisconnectClient(CPlayerSlot(slot),
                                 static_cast<ENetworkDisconnectionReason>(3),
                                 "BotIdentity test");
        break;
    }
    case 4: {
        // Stage as fully-connected fake-ish client: signon=6 + userid, then
        // DisconnectClient.
        const int signon = 6;
        const uint16_t uid = static_cast<uint16_t>(reason);
        std::memcpy(reinterpret_cast<unsigned char*>(client) + OFF_m_nSignonState,
                    &signon, sizeof(signon));
        WriteUserId(client, uid);
        ReapLog("leftover test slot=%d: wrote signon=6 userid=%u, DisconnectClient r=39\n",
                slot, static_cast<unsigned int>(uid));
        engine->DisconnectClient(CPlayerSlot(slot),
                                 static_cast<ENetworkDisconnectionReason>(39),
                                 "BotIdentity test");
        break;
    }
    case 5: {
        // Ghost visibility probe: clear the record's entity binding. If the
        // team-select model is driven by record->entity linkage, this hides
        // the ghost even though the record itself can't be freed.
        const int neg = -reason;
        std::memcpy(reinterpret_cast<unsigned char*>(client) + OFF_m_nEntityIndex,
                    &neg, sizeof(neg));
        ReapLog("leftover test slot=%d: wrote entIdx=%d\n", slot, neg);
        break;
    }
    case 6: {
        // Engine's internal removal path used by a normal disconnect:
        // CNetworkGameServerBase::RemoveClientFromGame. The public
        // IVEngineServer2::DisconnectClient early-outs on records that
        // never finished signon; this is one level deeper.
        auto* gameServer = g_pNetworkServerService
                               ? g_pNetworkServerService->GetIGameServer()
                               : nullptr;
        if (!gameServer) {
            ReapLog("leftover test slot=%d: game server null\n", slot);
            break;
        }
        ReapLog("leftover test slot=%d: RemoveClientFromGame r=%d\n", slot, reason);
        gameServer->RemoveClientFromGame(
            reinterpret_cast<CServerSideClientBase*>(client), reason);
        break;
    }
    case 7: {
        // Same removal path after staging the record as fully connected:
        // signon=6 + userid=<reason> so engine-side state checks pass.
        const int signon = 6;
        const uint16_t uid = static_cast<uint16_t>(reason);
        std::memcpy(reinterpret_cast<unsigned char*>(client) + OFF_m_nSignonState,
                    &signon, sizeof(signon));
        WriteUserId(client, uid);
        auto* gameServer = g_pNetworkServerService
                               ? g_pNetworkServerService->GetIGameServer()
                               : nullptr;
        if (!gameServer) {
            ReapLog("leftover test slot=%d: game server null\n", slot);
            break;
        }
        ReapLog("leftover test slot=%d: staged signon=6 userid=%u, RemoveClientFromGame r=3\n",
                slot, static_cast<unsigned int>(uid));
        gameServer->RemoveClientFromGame(
            reinterpret_cast<CServerSideClientBase*>(client), 3);
        break;
    }
    case 8: {
        // Verified from libengine2 disassembly: client vtbl[8] is
        // CServerSideClient::Disconnect(reason, internalReason) - it saves
        // esi/rdx across a call and fires the "client_disconnected" event.
        // Its gate is this+0x64 (m_nSignonState) != 0, which is why every
        // engine-side release path no-ops on these signon=0 leftovers.
        // Stage signon=6 (FULL) so the gate passes, then call it directly.
        const int signon = 6;
        std::memcpy(reinterpret_cast<unsigned char*>(client) + OFF_m_nSignonState,
                    &signon, sizeof(signon));
        auto** vtbl = *reinterpret_cast<void***>(client);
        auto* disconnect =
            reinterpret_cast<void (*)(void*, int, const char*)>(vtbl[8]);
        ReapLog("leftover test slot=%d: staged signon=6, call client->Disconnect r=%d fn=%p\n",
                slot, reason, reinterpret_cast<void*>(disconnect));
        disconnect(client, reason, "BotIdentity test");
        ReapLog("leftover test slot=%d: Disconnect returned\n", slot);
        break;
    }
    case 9: {
        // Same staged Disconnect plus RemoveClientFromGame follow-up in case
        // Disconnect only marks the record for the frame-loop reaper.
        const int signon = 6;
        std::memcpy(reinterpret_cast<unsigned char*>(client) + OFF_m_nSignonState,
                    &signon, sizeof(signon));
        auto** vtbl = *reinterpret_cast<void***>(client);
        auto* disconnect =
            reinterpret_cast<void (*)(void*, int, const char*)>(vtbl[8]);
        ReapLog("leftover test slot=%d: staged signon=6, client->Disconnect r=%d then RemoveClientFromGame\n",
                slot, reason);
        disconnect(client, reason, "BotIdentity test");
        auto* gameServer = g_pNetworkServerService
                               ? g_pNetworkServerService->GetIGameServer()
                               : nullptr;
        if (gameServer) {
            gameServer->RemoveClientFromGame(
                reinterpret_cast<CServerSideClientBase*>(client), 3);
        }
        ReapLog("leftover test slot=%d: mode9 calls returned\n", slot);
        break;
    }
    case 11: {
        // Orphan-controller test: a dead record still binds a live
        // cs_player_controller entity via entIdx (deleting=0, never got
        // destroyed because the client never completed connect). M renders
        // standin models from controllers, so these orphans are the ghosts.
        // Route the bound entity through the proven deferred-destroy path.
        // Guard: only dead records may queue — running this on a live slot
        // destroys the player's own controller.
        if (!RecordLooksDead(client)) {
            ReapLog("leftover test slot=%d: mode 11 refused: record is alive\n", slot);
            break;
        }
        const int entityIndex = GetEntityIndex(client);
        ReapLog("leftover test slot=%d: queue controller destroy entIdx=%d\n",
                slot, entityIndex);
        QueueControllerRemovalForClient(client, slot);
        break;
    }
    case 10: {
        // Dead records carry lifecycle-state byte +0x61 == 2, which routes
        // the deferred-dispatch wrapper (0x52d550) through a pre-cleanup
        // that resets signon before the real disconnect runs; the main
        // disconnect then takes the abbreviated "already gone" path and
        // skips notifying the server. Stage +0x61=0 + signon=6 so the
        // record looks like a normal active client on the full path.
        const int signon = 6;
        std::memcpy(reinterpret_cast<unsigned char*>(client) + OFF_m_nSignonState,
                    &signon, sizeof(signon));
        reinterpret_cast<unsigned char*>(client)[0x61] = 0;
        auto** vtbl = *reinterpret_cast<void***>(client);
        auto* disconnect =
            reinterpret_cast<void (*)(void*, int, const char*)>(vtbl[8]);
        ReapLog("leftover test slot=%d: staged state=0 signon=6, client->Disconnect r=%d\n",
                slot, reason);
        disconnect(client, reason, "BotIdentity test");
        ReapLog("leftover test slot=%d: mode10 Disconnect returned\n", slot);
        break;
    }
    default:
        ReapLog("leftover test slot=%d: unknown mode=%d\n", slot, mode);
        break;
    }
}

bool QueueControllerRemovalForClient(void* client, int slot) {
    if (!g_CleanupEnabled) return false;
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

bool g_CleanupEnabled = true;

void DrainPendingControllerRemovals() {
    if (!g_CleanupEnabled) return;
    ++g_DrainPass;
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
        ReapLeftoverClients();
        SweepTeamManagerMembers();
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
    ReapLeftoverClients();
    SweepTeamManagerMembers();
}

void ClearPendingControllerRemovals() { g_Pending.clear(); }

// Called from the ClientDisconnect POST hook for a managed slot that just
// finished engine-side disconnect handling. The record teardown can stall
// (signon>1 corpse), which keeps the slot's CMsgPlayerInfo entry — and its
// team-select preview model — replicated to every connected client. Strip
// the record to the canonical challenging fingerprint immediately instead
// of waiting for the leftover reap's dwell.
void NormalizeCorpseNow(int slot) {
    if (!GameServerAlive()) return;
    void* client = ResolveClientBySlot(slot);
    if (!client) return;
    if (ReadClientSlot(client) != slot) return;
    if (ReadNetChannel(client) != nullptr) return;  // still live
    if (ReadSignonState(client) <= 1 &&
        ReadUserId(client) == 65535 && ReadSteamId(client) == 0) {
        return;  // engine teardown completed cleanly
    }
    ReapLog("post-disconnect normalize slot=%d signon=%d userid=%u "
            "steamid=%llu\n",
            slot, ReadSignonState(client),
            static_cast<unsigned int>(ReadUserId(client)),
            static_cast<unsigned long long>(ReadSteamId(client)));
    // Zero every copy of the record's SteamID — m_SteamID, mirrors,
    // friends id and the xuid inside m_PlayerInfo all match the same u64.
    const uint64_t target = ReadSteamId(client);
    if (target != 0) {
        auto* raw = reinterpret_cast<unsigned char*>(client);
        for (int off = 0; off + 8 <= 2048; ++off) {
            uint64_t v = 0;
            std::memcpy(&v, raw + off, sizeof(v));
            if (v != target) continue;
            const uint64_t zero = 0;
            std::memcpy(raw + off, &zero, sizeof(zero));
            off += 7;
        }
    }
    if (ReadSignonState(client) > 1) {
        const int zero = 0;
        std::memcpy(reinterpret_cast<unsigned char*>(client) + OFF_m_nSignonState,
                    &zero, sizeof(zero));
    }
    if (ReadUserId(client) != 65535) WriteUserId(client, 65535);
    if (ReadSteamId(client) != 0) WriteSteamId(client, 0);
}

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
    static const char* kManagerClasses[] = {
        "cs_team_manager", "team_manager", "cs_player_manager",
        "team_select_terrorist", "team_select_counterterrorist",
    };
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
    int staleHits = 0;
    for (int entityIndex = 1; entityIndex <= 2048; ++entityIndex) {
        char className[64] = {0};
        void* instance = ResolveEntityInstance(entityIndex, className, sizeof(className));
        if (!instance) continue;
        bool managerClass = false;
        for (const char* mc : kManagerClasses) {
            if (std::strcmp(className, mc) == 0) {
                managerClass = true;
                break;
            }
        }
        if (!managerClass) continue;
        ++found;
        auto* entity = reinterpret_cast<CEntityInstance*>(instance);
        const uint32_t flags =
            entity->m_pEntity ? static_cast<uint32_t>(entity->m_pEntity->m_flags) : 0;
        const uint8_t team = ReadControllerTeam(instance);
        auto* raw = reinterpret_cast<unsigned char*>(instance);
        char scan[384];
        int n = std::snprintf(
            scan, sizeof(scan),
            "teammanager entIdx=%d cls='%s' team=%u flags=0x%x handles",
            entityIndex,
            className,
            static_cast<unsigned int>(team),
            flags);
        int hits = 0;
        for (int off = 0; off + 4 <= 4096; off += 4) {
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

        // CTeam::m_aPlayerControllers @1200 / m_aPlayers @1224 are the
        // networked member vectors — stale handles here keep rendering
        // team-select ghosts client-side.
        for (int fieldOff : {1200, 1224}) {
            char hdr[256];
            int hn = std::snprintf(hdr, sizeof(hdr), "member vec raw +%d:", fieldOff);
            for (int q = 0; q < 8; ++q) {
                uint64_t qw = 0;
                std::memcpy(&qw, raw + fieldOff + q * 8, 8);
                hn += std::snprintf(hdr + hn, sizeof(hdr) - hn, " [%d]=0x%llx", q * 8,
                                    static_cast<unsigned long long>(qw));
            }
            ReapLog("%s tag=%s\n", hdr, label);
            for (int cand = 0; cand <= 16; cand += 8) {
                void* mem = nullptr;
                int size = 0;
                std::memcpy(&mem, raw + fieldOff + cand, 8);
                std::memcpy(&size, raw + fieldOff + cand + 8, 4);
                if (!mem || size <= 0 || size > 64) continue;
                ReapLog("member vec +%d cand=+%d ptr=%p size=%d tag=%s\n",
                        fieldOff, cand, mem, size, label);
                auto* handles = reinterpret_cast<uint32_t*>(mem);
                for (int i = 0; i < size && i < 64; ++i) {
                    const uint32_t h = handles[i];
                    const int idx = static_cast<int>(h & 0x3FFFu);
                    char tc[64] = {0};
                    void* t = idx > 0 ? ResolveEntityInstance(idx, tc, sizeof(tc)) : nullptr;
                    if (!t) {
                        ReapLog("  member[%d]=0x%08x idx=%d DEAD tag=%s\n", i, h, idx, label);
                        ++staleHits;
                        continue;
                    }
                    auto* te = reinterpret_cast<CEntityInstance*>(t);
                    const uint32_t lh = static_cast<uint32_t>(te->GetRefEHandle().ToInt());
                    ReapLog("  member[%d]=0x%08x idx=%d cls='%s' %s deleting=%d tag=%s\n",
                            i, h, idx, tc,
                            lh == h ? "ok" : "SERIAL-MISMATCH",
                            IsEntityBeingDeleted(t) ? 1 : 0, label);
                    if (lh != h) ++staleHits;
                }
            }
        }

        // Handle-shaped scan: any stored CEntityHandle whose target is gone or
        // whose serial no longer matches is a stale member reference — the
        // prime suspect for client-rendered team-select ghosts.
        int liveRefs = 0;
        for (int off = 0; off + 4 <= 4096; off += 4) {
            uint32_t value = 0;
            std::memcpy(&value, raw + off, sizeof(value));
            if (value == 0 || value == 0xFFFFFFFFu) continue;
            if ((value & ~0x3FFFu) == 0) continue;
            const int idx = static_cast<int>(value & 0x3FFFu);
            if (idx < 1 || idx > 4095) continue;
            char targetClass[64] = {0};
            void* target = ResolveEntityInstance(idx, targetClass, sizeof(targetClass));
            if (!target) {
                n = std::snprintf(scan, sizeof(scan),
                                  "stale ref +%d=0x%08x idx=%d (dead) tag=%s\n",
                                  off, value, idx, label);
                ReapLog("%s", scan);
                ++staleHits;
                if (staleHits >= 24) break;
                continue;
            }
            auto* targetEntity = reinterpret_cast<CEntityInstance*>(target);
            const uint32_t liveHandle =
                static_cast<uint32_t>(targetEntity->GetRefEHandle().ToInt());
            if (liveHandle == value) {
                if (liveRefs++ < 40) {
                    ReapLog("live ref +%d=0x%08x idx=%d cls='%s' deleting=%d tag=%s\n",
                            off, value, idx, targetClass,
                            IsEntityBeingDeleted(target) ? 1 : 0, label);
                }
            } else {
                ReapLog("stale ref +%d=0x%08x idx=%d live=0x%08x cls='%s' tag=%s\n",
                        off, value, idx, liveHandle, targetClass, label);
                ++staleHits;
                if (staleHits >= 24) break;
            }
        }
    }
    ReapLog("team manager occupancy found=%d leftoverControllers=%d handleHits=%d staleHits=%d tag=%s\n",
            found, leftoverCount, handleHits, staleHits, label);
}

void DumpGhostEntities(const char* tag) {
    const char* label = tag && tag[0] ? tag : "?";
    static const char* kInteresting[] = {
        "pawn", "player", "preview", "intro", "character", "teamselect", "observer", "dummy",
    };
    struct ClassCount {
        char name[64];
        int count;
    };
    ClassCount histogram[512];
    int histogramCount = 0;
    for (int entityIndex = 1; entityIndex <= 4096; ++entityIndex) {
        char className[64] = {0};
        void* instance = ResolveEntityInstance(entityIndex, className, sizeof(className));
        if (!instance) continue;
        int slot = -1;
        for (int i = 0; i < histogramCount; ++i) {
            if (std::strcmp(histogram[i].name, className) == 0) {
                slot = i;
                break;
            }
        }
        if (slot < 0 && histogramCount < 512) {
            slot = histogramCount++;
            std::snprintf(histogram[slot].name, sizeof(histogram[slot].name), "%s", className);
            histogram[slot].count = 0;
        }
        if (slot >= 0) ++histogram[slot].count;

        bool interesting = false;
        for (const char* needle : kInteresting) {
            if (std::strstr(className, needle)) {
                interesting = true;
                break;
            }
        }
        if (!interesting) continue;
        auto* entity = reinterpret_cast<CEntityInstance*>(instance);
        const uint32_t flags =
            entity->m_pEntity ? static_cast<uint32_t>(entity->m_pEntity->m_flags) : 0;
        const uint32_t handle = static_cast<uint32_t>(entity->GetRefEHandle().ToInt());
        ReapLog(
            "entity entIdx=%d class='%s' team836=%u flags=0x%x handle=0x%08x deleting=%d tag=%s\n",
            entityIndex,
            className,
            static_cast<unsigned int>(ReadControllerTeam(instance)),
            flags,
            handle,
            IsEntityBeingDeleted(instance) ? 1 : 0,
            label);
    }
    for (int i = 0; i < histogramCount; ++i) {
        ReapLog("class '%s' x%d tag=%s\n", histogram[i].name, histogram[i].count, label);
    }
    ReapLog("entity census classes=%d tag=%s\n", histogramCount, label);
}

void DumpTeamSelectPositions(const char* tag) {
    const char* label = tag && tag[0] ? tag : "?";
    int found = 0;
    int occupied = 0;
    for (int entityIndex = 1; entityIndex <= 4096; ++entityIndex) {
        char className[64] = {0};
        void* instance = ResolveEntityInstance(entityIndex, className, sizeof(className));
        if (!instance) continue;
        if (std::strncmp(className, "team_select_", 12) != 0 &&
            std::strncmp(className, "team_intro_", 11) != 0) {
            continue;
        }
        ++found;
        auto* raw = reinterpret_cast<unsigned char*>(instance);
        // CCSGO_TeamPreviewCharacterPosition schema offsets:
        // m_nVariant +0x4A8, m_nRandom +0x4AC, m_nOrdinal +0x4B0,
        // m_sWeaponName +0x4B8, m_xuid +0x4C0, m_agentItem +0x4C8.
        uint32_t variant = 0, random = 0, ordinal = 0;
        uint64_t xuid = 0;
        uint16_t agentDefIdx = 0;
        std::memcpy(&variant, raw + 0x4A8, 4);
        std::memcpy(&random, raw + 0x4AC, 4);
        std::memcpy(&ordinal, raw + 0x4B0, 4);
        std::memcpy(&xuid, raw + 0x4C0, 8);
        std::memcpy(&agentDefIdx, raw + 0x4C8, 2);
        if (xuid != 0 || agentDefIdx != 0) ++occupied;
        auto* entity = reinterpret_cast<CEntityInstance*>(instance);
        const uint32_t flags =
            entity->m_pEntity ? static_cast<uint32_t>(entity->m_pEntity->m_flags) : 0;
        ReapLog(
            "pos entIdx=%d cls='%s' variant=%u random=%u ordinal=%u xuid=%llu agentDef=%u flags=0x%x tag=%s\n",
            entityIndex,
            className,
            variant,
            random,
            ordinal,
            static_cast<unsigned long long>(xuid),
            static_cast<unsigned int>(agentDefIdx),
            flags,
            label);
    }
    ReapLog("position census found=%d occupied=%d tag=%s\n", found, occupied, label);
}

void ScanEntitiesForU64(uint64_t needle, const char* tag) {
    const char* label = tag && tag[0] ? tag : "?";
    if (needle == 0) {
        ReapLog("scanxuid: needle=0 refused tag=%s\n", label);
        return;
    }
    int hits = 0;
    constexpr size_t kScanBytes = 0x1500;
    for (int entityIndex = 1; entityIndex <= 4096; ++entityIndex) {
        char className[64] = {0};
        void* instance = ResolveEntityInstance(entityIndex, className, sizeof(className));
        if (!instance) continue;
        auto* raw = reinterpret_cast<unsigned char*>(instance);
        for (size_t off = 0; off + sizeof(needle) <= kScanBytes; off += 4) {
            uint64_t value = 0;
            std::memcpy(&value, raw + off, sizeof(value));
            if (value != needle) continue;
            ++hits;
            ReapLog("scanxuid hit entIdx=%d cls='%s' off=+0x%zx tag=%s\n",
                    entityIndex, className, off, label);
        }
    }
    ReapLog("scanxuid needle=%llu hits=%d tag=%s\n",
            static_cast<unsigned long long>(needle), hits, label);
}

// The team-select preview position entity binds its occupant via a u64 at
// +0x7a0 (a live dump put the local player's SteamID64 there). Poke it to
// test whether the client's preview model re-reads the binding.
void PokeTeamSelectXuid(const char* matchArg, uint64_t newValue) {
    const bool matchAll = matchArg && std::strcmp(matchArg, "all") == 0;
    uint64_t match = 0;
    if (!matchAll && matchArg) match = std::strtoull(matchArg, nullptr, 10);
    int writes = 0;
    for (int entityIndex = 1; entityIndex <= 4096; ++entityIndex) {
        char className[64] = {0};
        void* instance = ResolveEntityInstance(entityIndex, className, sizeof(className));
        if (!instance) continue;
        if (!std::strstr(className, "team_select") &&
            !std::strstr(className, "team_intro"))
            continue;
        auto* raw = reinterpret_cast<unsigned char*>(instance);
        uint64_t cur = 0;
        std::memcpy(&cur, raw + 0x7a0, sizeof(cur));
        if (!matchAll && cur != match) continue;
        std::memcpy(raw + 0x7a0, &newValue, sizeof(newValue));
        MarkEntityStateChanged(instance);
        ++writes;
        ReapLog("pospoke entIdx=%d cls='%s' %llu->%llu\n",
                entityIndex, className,
                static_cast<unsigned long long>(cur),
                static_cast<unsigned long long>(newValue));
    }
    ReapLog("pospoke done writes=%d\n", writes);
}

int NeutralizeCollidingLeftoverUserIds(int keepSlot, uint16_t userId) {
    if (keepSlot < 0 || keepSlot >= kMaxSlots) return 0;
    if (userId == 65535) return 0;

    int rewritten = 0;
    for (int slot = 0; slot < kMaxSlots; ++slot) {
        if (slot == keepSlot) continue;
        void* client = ResolveClientBySlot(slot);
        if (!client) continue;
        if (IdentityMgr().IsManaged(slot)) continue;
        if (ReadNetChannel(client) != nullptr) continue;
        if (ReadSteamId(client) != 0) continue;
        if (ReadUserId(client) != userId) continue;

        WriteUserId(client, 65535);
        ++rewritten;
        ReapLog("userid collision neutralized slot=%d keep=%d userid=%u signon=%d\n",
                slot,
                keepSlot,
                static_cast<unsigned int>(userId),
                ReadSignonState(client));
    }
    return rewritten;
}

bool MoveManagedBotToSpectator(int slot) {
    if (slot < 0 || slot >= kMaxSlots) return false;
    if (ChangeTeamVtableIndex() < 0) {
        ReapLog("ChangeTeam skipped: vtable index unset\n");
        return false;
    }
    if (!IdentityMgr().IsManaged(slot)) return false;

    void* client = ResolveClientBySlot(slot);
    if (!client) return false;
    const int entityIndex = GetEntityIndex(client);
    char className[64] = {0};
    void* controller = ResolveEntityInstance(entityIndex, className, sizeof(className));
    if (!controller || std::strcmp(className, "cs_player_controller") != 0) return false;
    if (IsEntityBeingDeleted(controller)) {
        ReapLog("ChangeTeam skipped slot=%d entIdx=%d: already deleting\n",
                slot, entityIndex);
        return false;
    }
    const uint8_t team = ReadControllerTeam(controller);
    if (team != 2 && team != 3) return false;
    const bool ok = CallControllerChangeTeam(controller, kSpectatorTeam);
    const uint8_t after = ReadControllerTeam(controller);
    ReapLog("ChangeTeam slot=%d entIdx=%d team=%u -> 1 ok=%d after=%u\n",
            slot,
            entityIndex,
            static_cast<unsigned int>(team),
            ok ? 1 : 0,
            static_cast<unsigned int>(after));
    if (ok && after == team) {
        ReapLog("ChangeTeam slot=%d entIdx=%d warn: ok but team unchanged; vtable index may be stale\n",
                slot, entityIndex);
    }
    return ok;
}

void ReapOrphanControllers() {
    if (!g_CleanupEnabled) return;
    if (!GameServerAlive() || !UtilRemoveTarget()) return;
    // If the client vector can't be read at all (stale m_Clients offset),
    // every slot reports "leftover" and the reap would nuke live players.
    // With zero clients there is nothing legitimate to reap anyway.
    if (ClientListCount() <= 0) return;

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
