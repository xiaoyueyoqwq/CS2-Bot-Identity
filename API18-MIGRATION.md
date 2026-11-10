# BotIdentity API18 迁移笔记（2026-11-11）

## 背景

metamod build ~1460+ 把插件接口升到 API18：`core/sourcehook/` **整个删除**，老 SH 二进制被拒载（`old SourceHook build (17 < 18)`）。API18 构建 = **SourceHook → KHook 源码迁移**，不是换头文件。

## 新构建环境（已备好）

- MM 头：`/home/xiaoyueyoqwq/project/CS2-Server/.codex/deps/metamod-source-18`（origin/master `ea513d8` worktree）
- khook：`metamod-source-18/third_party/khook`（已 `git submodule update --init`）
- `metamod-source-18/core/metamod_khook.h` 提供 `KHookImpl`（`KHook::SetupHook`/`SetupVirtualHook`），插件侧要求 `KHOOK_STANDALONE` 宏 + `KHook::__exported__khook` 非空检查

## 上游参考实现（XBribo/CS2-Bot-Controller main，已 khook）

```cpp
using DisconnectHookBase =
    KHook::Virtual<ISource2GameClients, void, CPlayerSlot, ENetworkDisconnectionReason, const char*, uint64, const char*>;

class DisconnectHook : public DisconnectHookBase { ... };

KHook::Return<void> OnClientDisconnect(ISource2GameClients*, CPlayerSlot, ...) {
    ...
    return { KHook::Action::Ignore };
}

// Load():
if (!KHook::__exported__khook) fail("KHook support required");
g_hook = std::make_unique<DisconnectHook>(&ISource2GameClients::ClientDisconnect, &OnClientDisconnect, nullptr);
if (!g_hook->AddChecked(clients)) fail;
```

- `KHook::Virtual<IFace, Ret, Args...>` 模板：`(成员函数指针, pre回调, post回调)`，`Add(iface)` 装
- 回调返回 `KHook::Return<Ret>`，`KHook::Action::Ignore` 等价 SH 的 `MRES_IGNORED`；`Action::Handled/Supercede` 挡掉原调用

## BotIdentity 待迁移清单（src/plugin.cpp）

| # | SH 声明 | SH 时机 | KHook 形态 |
|---|---|---|---|
| 1 | `SH_DECL_HOOK6_void(IServerGameClients, OnClientConnected, ..., CPlayerSlot, const char*, const char*, int, const char*, bool)` | post | `KHook::Virtual<ISource2GameClients, void, 6 args>` 传 post cb |
| 2 | `SH_DECL_HOOK4_void(IServerGameClients, ClientPutInServer, ..., CPlayerSlot, const char*, int, uint64)` | post | 同上 |
| 3 | `SH_DECL_HOOK5_void(IServerGameClients, ClientDisconnect, ..., CPlayerSlot, ENetworkDisconnectionReason, const char*, uint64, const char*)` | pre | 传 pre cb |
| 4 | `SH_DECL_HOOK3_void(IServerGameDLL, GameFrame, ..., bool, bool, bool)` | post | `KHook::Virtual<IServerGameDLL, void,bool,bool,bool>` |
| 5/6 | `SH_DECL_HOOK3_void(ICvar, DispatchConCommand, ..., ConCommandRef, const CCommandContext&, const CCommand&)` | pre + post | 同函数 pre+post 两个 KHook::Virtual 或一个带两 cb |

⚠️ 槽位类型：`ISource2GameClients`（S2 新名，上游代码用此）vs `IServerGameClients`（S1 老名）——核对本地 include 里实际的类名再写模板参数。

## 构建改动点（CMakeLists.txt）

- `SDK_SOURCES` 里 `sourcehook*.cpp` 全部没了 → 换成 khook 需要的源（查 metamod-source-18/core 下有没有给插件编译的 khook 配套源文件；上游走 `cs2_sdk::sdk` 封装，我们没有，直接引 `khook/src` 或不需要源）
- `target_include_directories` 加 `${MMS}/third_party/khook/include`
- `target_compile_definitions` 加 `KHOOK_STANDALONE`
- `MMSOURCE_DEV` 指 `metamod-source-18`（API18 构建）或老 deps/metamod-source（API17 构建）二选一
- glibc：构建机 2.43 > 服务器 2.39——若链出 `acosf/asinf/atan2f/sqrtf` 等符号，在 plugin.cpp 加 extern C 本地实现（RayTraceRemastered 同款解法）

## 验证顺序

1. API18 构建成功 → `readelf -V` 无 >2.39 引用 → 部署到 **MM1490 环境**（等 CSS 就绪后一起上）或先放 staged
2. 加载验 `meta list` BotIdentity 起来 + `botidentity:api` 注册 + 6 hook 生效（bot 伪装/投票照常）

## 风险提示

- `DispatchConCommand` 的 pre-hook 如果能 return-handled 挡命令——SH `MRES_SUPERCEDE` 语义对应 KHook `Action::Handled`/`Supercede`，迁移时逐条对语义
- `ClientDisconnect` pre 里有状态清理（ForgetSlotLifecycle 类）——迁移成 pre cb 时保持同样先清再放
EOF
echo written
## 回调语义盘点（已核，全 IGNORED）

全部 6 个钩子回调里只出现 `RETURN_META(MRES_IGNORED)` / `return`——**纯观察型**，没有 SUPERCEDE/HANDLED 拦截。
⇒ KHook 迁移统一：回调返回 `{ KHook::Action::Ignore }`（pre）或观察型 post。无语义坑。

- post 类：OnClientConnected / ClientPutInServer / GameFrame / DispatchConCommand(post)
- pre 类：ClientDisconnect / DispatchConCommand(pre)
- 函数指针：`&IServerGameClients::OnClientConnected` 这类——注意本地 sdk 里接口名是 `IServerGameClients`（S1 老名）还是 `ISource2GameClients`（上游 main 用 S2 名）——**写代码前先 grep 确认**

## 完整迁移配方（已核定，可直接套）

### plugin.cpp 顶部替换（SH_DECL_HOOK 段 → KHook）

```cpp
// 原 SH_DECL_HOOK×6 全删，换：
#include <khook.hpp>
#include <memory>

using HookConnectedBase =
    KHook::Virtual<IServerGameClients, void, CPlayerSlot, const char*, uint64, const char*, const char*, bool>;
using HookPutInBase =
    KHook::Virtual<IServerGameClients, void, CPlayerSlot, const char*, int, uint64>;
using HookDisconnectBase =
    KHook::Virtual<IServerGameClients, void, CPlayerSlot, ENetworkDisconnectionReason, const char*, uint64, const char*>;
using HookGameFrameBase =
    KHook::Virtual<IServerGameDLL, void, bool, bool, bool>;
using HookConCmdBase =
    KHook::Virtual<ICvar, void, ConCommandRef, const CCommandContext&, const CCommand&>;

static std::unique_ptr<HookConnectedBase>   g_hkConnected;
static std::unique_ptr<HookPutInBase>       g_hkPutIn;
static std::unique_ptr<HookDisconnectBase>  g_hkDisconnect;
static std::unique_ptr<HookGameFrameBase>   g_hkGameFrame;
static std::unique_ptr<HookConCmdBase>      g_hkConCmdPre;
static std::unique_ptr<HookConCmdBase>      g_hkConCmdPost;

// 回调（KHook 用自由函数，内部转调成员函数）
KHook::Return<void> K_OnClientConnected(IServerGameClients*, CPlayerSlot s, const char* n, uint64 x, const char* nid, const char* addr, bool fake) {
    g_BotIdentityPlugin.Hook_OnClientConnected_Post(s, n, x, nid, addr, fake);
    return { KHook::Action::Ignore };
}
KHook::Return<void> K_ClientPutInServer(IServerGameClients*, CPlayerSlot s, const char* n, int t, uint64 x) {
    g_BotIdentityPlugin.Hook_ClientPutInServer_Post(s, n, t, x);
    return { KHook::Action::Ignore };
}
KHook::Return<void> K_ClientDisconnect(IServerGameClients*, CPlayerSlot s, ENetworkDisconnectionReason r, const char* n, uint64 x, const char* nid) {
    g_BotIdentityPlugin.Hook_ClientDisconnect_Pre(s, r, n, x, nid);
    return { KHook::Action::Ignore };
}
KHook::Return<void> K_GameFrame(IServerGameDLL*, bool sim, bool first, bool last) {
    g_BotIdentityPlugin.Hook_GameFrame_Post(sim, first, last);
    return { KHook::Action::Ignore };
}
KHook::Return<void> K_ConCmdPre(ICvar*, ConCommandRef c, const CCommandContext& x, const CCommand& a) {
    g_BotIdentityPlugin.Hook_DispatchConCommand_Pre(c, x, a);
    return { KHook::Action::Ignore };
}
KHook::Return<void> K_ConCmdPost(ICvar*, ConCommandRef c, const CCommandContext& x, const CCommand& a) {
    g_BotIdentityPlugin.Hook_DispatchConCommand_Post(c, x, a);
    return { KHook::Action::Ignore };
}
```

### 回调函数体内（25 处）

`sed -i 's/RETURN_META(MRES_IGNORED)/return/g' src/plugin.cpp` — 全部是 IGNORED，等价替换。

### Load() 里 SH_ADD_HOOK 段替换

```cpp
if (!KHook::__exported__khook) { snprintf(error,maxlen,"KHook support required"); return false; }
g_hkConnected  = std::make_unique<HookConnectedBase>(&IServerGameClients::OnClientConnected, nullptr, &K_OnClientConnected);
g_hkPutIn      = std::make_unique<HookPutInBase>(&IServerGameClients::ClientPutInServer, nullptr, &K_ClientPutInServer);
g_hkDisconnect = std::make_unique<HookDisconnectBase>(&IServerGameClients::ClientDisconnect, &K_ClientDisconnect, nullptr);
g_hkGameFrame  = std::make_unique<HookGameFrameBase>(&IServerGameDLL::GameFrame, nullptr, &K_GameFrame);
g_hkConCmdPre  = std::make_unique<HookConCmdBase>(&ICvar::DispatchConCommand, &K_ConCmdPre, nullptr);
g_hkConCmdPost = std::make_unique<HookConCmdBase>(&ICvar::DispatchConCommand, nullptr, &K_ConCmdPost);
g_hkConnected->Add(gameclients);  g_hkPutIn->Add(gameclients);  g_hkDisconnect->Add(gameclients);
g_hkGameFrame->Add(gameserver);   g_hkConCmdPre->Add(icvar);      g_hkConCmdPost->Add(icvar);
```
（KHook::Virtual ctor 参数序=(method, pre, post)——post 钩子传 `nullptr, &cb`）

### Unload() 里 SH_REMOVE_HOOK 段替换

```cpp
g_hkConnected.reset(); g_hkPutIn.reset(); g_hkDisconnect.reset();
g_hkGameFrame.reset(); g_hkConCmdPre.reset(); g_hkConCmdPost.reset();
```

### CMakeLists 改动

- `SDK_SOURCES`：删 sourcehook*.cpp（mm-18 无此目录）——api18 下插件不需要这些源
- include 加 `${MMS}/third_party/khook/include`
- defines 加 `KHOOK_STANDALONE`
- `MMSOURCE_DEV` 指向 `metamod-source-18`
