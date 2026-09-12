#pragma once

#include <cstddef>
#include <cstdint>

// Forward decls to avoid pulling ISmmPlugin.h into every translation unit
class CEntityInstance;
struct NetworkStateChangedData;

namespace botid {

// Load gamedata overrides (call once at plugin load)
void LoadGamedata(const char* path);

// Resolve client by slot (returns CServerSideClient*)
void* ResolveClientBySlot(int slot);

// m_Clients.Count() after the same bounds check ResolveClientBySlot uses.
int ClientListCount();

// Shrink m_Clients.Count() so leaked leftover pointers fall outside
// 0..Count-1. Never inserts, never writes nullptr, never SetCount /
// FastRemove. False if newCount is not strictly smaller than Count().
bool ShrinkClientListTo(int newCount);

// Resolve entity instance by entity index, optionally fetch class name
// Returns entity instance pointer (e.g. cs_player_controller), or nullptr.
void* ResolveEntityInstance(int entityIndex, char* classnameOut = nullptr,
                           size_t classnameCap = 0, bool debug = false);

// Trigger network state sync for one entity
void MarkEntityStateChanged(void* instance);
// Mark one flattened field dirty (e.g. CBaseEntity::m_iTeamNum).
void MarkEntityFieldChanged(void* instance, uint32_t localOffset);

// Set the global GameResourceService pointer (called once at plugin load)
void SetGameResourceServicePtr(void* p);
// Returns the current GameResourceService pointer
void* GetGameResourceServicePtr();

// Safe pointer read (returns false on null)
bool SafeReadPtr(const void* address, void** output);

// Resolve UTIL_Remove from gamedata. Prefer the module that hosts
// serverInterface (IServerGameClients); Metamod also ships libserver.so.
void ResolveUtilRemove(void* serverInterface = nullptr);
// Path of the module the signature hit, or empty if unresolved.
const char* UtilRemoveModulePath();
// Resolved UTIL_Remove function pointer, or nullptr if the signature missed.
void* UtilRemoveTarget();
// Destroy one entity through UTIL_Remove. False if unresolved or null.
bool RemoveEntity(void* instance);
// True if the instance is missing or already entering deletion.
bool IsEntityBeingDeleted(void* instance);

// CCSPlayerController::ChangeTeam vtable index from gamedata, or -1 if unset.
int ChangeTeamVtableIndex();
// Call CCSPlayerController::ChangeTeam(team) through the object vtable.
// Spectator=1. Does not hook JoinTeam. False if the vtable slot is missing.
bool CallControllerChangeTeam(void* controller, int team);

}  // namespace botid
