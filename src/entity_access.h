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

// Resolve entity instance by entity index, optionally fetch class name
// Returns entity instance pointer (e.g. cs_player_controller), or nullptr.
void* ResolveEntityInstance(int entityIndex, char* classnameOut = nullptr,
                           size_t classnameCap = 0, bool debug = false);

// Trigger network state sync for one entity
void MarkEntityStateChanged(void* instance);

// Set the global GameResourceService pointer (called once at plugin load)
void SetGameResourceServicePtr(void* p);
// Returns the current GameResourceService pointer
void* GetGameResourceServicePtr();

// Safe pointer read (returns false on null)
bool SafeReadPtr(const void* address, void** output);

}  // namespace botid
