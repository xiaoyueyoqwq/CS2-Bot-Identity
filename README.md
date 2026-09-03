# BotIdentity

A Counter-Strike 2 Metamod plugin for managing bot identities.

## Overview

BotIdentity allows you to customize bot identities including:
- Custom SteamID64
- Custom player names
- Fake ping (10-60ms)
- Scoreboard flair (with probability)
- Custom crosshair codes
- Custom avatars

## Key Difference from Schema API Approach

**Critical Discovery**: The original Schema API approach failed because it tried to modify Native bots directly. However, Schema API can only modify networked fields, which don't affect client display for Native bots.

**Solution**: BotIdentity converts Native bots to Fake-players first by setting `m_bFakePlayer = 1` in the `CServerSideClient` structure. This allows subsequent modifications to work correctly.

## Building

### Prerequisites

1. **Metamod:Source SDK**
   - Download from: https://github.com/alliedmodders/metamod-source
   - Extract to a known location

2. **HL2SDK for CS2**
   - Download from: https://github.com/alliedmodders/hl2sdk
   - Branch: `cs2` or appropriate branch
   - Extract to a known location

3. **CMake 3.10+**

### Build Instructions

#### Linux

```bash
cd CS2-Bot-Identity
mkdir build && cd build
cmake .. \
  -DCMAKE_BUILD_TYPE=Release \
  -DMETAMOD_PATH=/path/to/metamod-source \
  -DHL2SDK_PATH=/path/to/hl2sdk
make
```

#### Windows

```cmd
cd CS2-Bot-Identity
mkdir build && cd build
cmake .. -G "Visual Studio 17 2022" -A x64 ^
  -DMETAMOD_PATH=C:\path\to\metamod-source ^
  -DHL2SDK_PATH=C:\path\to\hl2sdk
cmake --build . --config Release
```

## Installation

1. Copy the built plugin to your server:
   ```
   game/csgo/addons/BotIdentity/bin/linuxsteamrt64/BotIdentity.so  (Linux)
   game/csgo/addons/BotIdentity/bin/windows/BotIdentity.dll         (Windows)
   ```

2. Copy `config.json` to:
   ```
   game/csgo/addons/BotIdentity/config.json
   ```

3. Add to `game/csgo/addons/metamod/metaplugins.ini`:
   ```
   addons/BotIdentity/bin/linuxsteamrt64/BotIdentity.so    (Linux)
   addons/BotIdentity/bin/windows/BotIdentity.dll           (Windows)
   ```

4. Restart your server

## Configuration

Edit `config.json`:

```json
{
  "bots": {
    "BotName1": {
      "steamId": 76561197960287XXX,
      "name": "Custom Name 1",
      "crosshairCode": "CSGO-XXXXX-XXXXX-XXXXX-XXXXX-XXXXX",
      "scoreboardFlair": 874,
      "avatarPath": "avatars/bot1.png"
    }
  },
  "features": {
    "enableFakePing": true,
    "fakePingMin": 10,
    "fakePingMax": 60,
    "enableScoreboardFlair": true,
    "scoreboardFlairProbability": 0.3
  }
}
```

## How It Works

1. **Hook OnClientConnected**: When a bot connects, we intercept the connection
2. **Convert to Fake-player**: Set `m_bFakePlayer = 1` in `CServerSideClient`
3. **Apply Identity**: 
   - Write custom SteamID to memory
   - Set custom player name
   - Apply other customizations
4. **Hook ClientPutInServer**: Reapply identity to ensure it's set correctly
5. **Hook ClientDisconnect**: Clean up managed bot data

## Memory Offsets

The following offsets are used for direct memory manipulation (Linux):

| Field | Offset | Type |
|-------|--------|------|
| m_Name | 64 | CUtlString |
| m_bFakePlayer | 160 | bool |
| m_SteamID | 171 | uint64 |
| m_SteamIDMirror | 179 | uint64 |
| m_nConnectionTypeFlags | 96 | byte |

**Note**: These offsets may change with game updates. If the plugin stops working after an update, these offsets may need to be recalculated.

## Troubleshooting

### Plugin doesn't load
- Check that all dependencies are met
- Verify the plugin path in `metaplugins.ini`
- Check server logs for error messages

### Bot identities don't apply
- Verify memory offsets are correct for your game version
- Check that bots are being created as Native bots
- Enable debug logging to see what's happening

### Crashes
- Ensure the plugin is compatible with your Metamod version
- Check for conflicts with other plugins
- Report crashes with stack traces

## Development

### Project Structure

```
CS2-Bot-Identity/
├── src/
│   ├── plugin.h          # Metamod plugin interface
│   ├── plugin.cpp        # Plugin implementation and hooks
│   ├── bot_identity.h    # Bot identity management
│   ├── config.h          # Configuration management
│   └── memory_ops.h      # Direct memory operations
├── CMakeLists.txt        # Build configuration
├── config.json           # Example configuration
└── README.md             # This file
```

### Key Components

- **memory_ops.h**: Direct memory manipulation functions
- **bot_identity.h**: Bot identity management logic
- **plugin.cpp**: Metamod hooks and plugin lifecycle
- **config.h**: Configuration loading and management

## License

MIT License

## Credits

- Inspired by CS2-Bot-Hider project
- Thanks to the Metamod:Source and AlliedModders communities

## Disclaimer

This plugin modifies game memory directly. Use at your own risk. May not be compatible with all server configurations or future game updates.
