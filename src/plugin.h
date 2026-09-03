#pragma once

#include <ISmmPlugin.h>
#include <playerslot.h>
#include <string>

class BotIdentityPlugin : public ISmmPlugin, public IMetamodListener {
public:
    bool Load(PluginId id, ISmmAPI* ismm, char* error, size_t maxlen, bool late) override;
    bool Unload(char* error, size_t maxlen) override;
    bool Pause(char*, size_t) override { return true; }
    bool Unpause(char*, size_t) override { return true; }
    void AllPluginsLoaded() override;

    const char* GetAuthor() override { return "xiaoyueyoqwq"; }
    const char* GetName() override { return "BotIdentity"; }
    const char* GetDescription() override { return "Bot identity management — minimal implementation"; }
    const char* GetURL() override { return "https://github.com/xiaoyueyoqwq/CS2-Bot-Identity"; }
    const char* GetLicense() override { return "MIT"; }
    const char* GetVersion() override { return "0.1.0"; }
    const char* GetDate() override { return __DATE__; }
    const char* GetLogTag() override { return "BOTIDENTITY"; }

private:
    void Hook_OnClientConnected_Post(
        CPlayerSlot slot, const char* pszName, uint64 xuid,
        const char* pszNetworkID, const char* pszAddress, bool bFakePlayer);
    void Hook_ClientPutInServer_Post(
        CPlayerSlot slot, const char* pszName, int type, uint64 xuid);
    void Hook_ClientDisconnect_Pre(
        CPlayerSlot slot, ENetworkDisconnectionReason reason, const char* pszName, uint64 xuid, const char* pszNetworkID);
    void Hook_GameFrame_Post(bool simulating, bool bFirstTick, bool bLastTick);

    // Last time we jittered pings, in seconds since epoch (wall-clock)
    double m_LastJitterTime = 0.0;

    ISmmAPI* ismm_ = nullptr;
};

enum ENetworkDisconnectionReason : int;

extern BotIdentityPlugin g_BotIdentityPlugin;
