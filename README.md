# kryoto-online

A drop-in `steam_api.dll` / `steam_api64.dll` that reports the running game as Spacewar (AppID 480)
to a real Steam client, so Steam's own matchmaking, lobbies and P2P keep working.

**Kryoto** · C++ (MSVC) · [License](LICENSE.md) · [All repos](https://github.com/Kryoto-Dev)

It is not an emulator: the Steam client must be running and signed in. For a game that runs without
Steam at all, use gbe_fork instead.

## Quick start

1. Copy both DLLs from a [release](../../releases/latest) into the game folder
   (`steam_api64.dll` + `kryotoO.dll`, or `steam_api.dll` + `kryotoO32.dll` for 32-bit).
2. Put a `kryoto-online.ini` next to the game's executable:

   ```ini
   [Settings]
   AppId=480
   ogAppId=220
   ```

3. Start the game. The log is `%TEMP%\kryoto-online.log`.

All settings, SteamStub and plugins: [docs/configuration.md](docs/configuration.md).

## Build

Visual Studio Build Tools 2022+ with "Desktop development with C++":

```bat
build.bat         :: release build into relbuild\
build.bat debug   :: debug build with the minidump handler, into debbuild\
```

## Layout

| Path | What's there |
|---|---|
| `dllmain.cpp`, `kryoto_online.vcxproj` | the proxy DLL (forwards to the real Steam client) |
| `core/`, `kryotoO.vcxproj` | the core DLL: every patch, the ini, the plugin loader |
| `core/kryotoo_abi.h` | the contract between proxy and core |
| `plugins/` | per-game plugins (`photon_universal`, `raft_mp`, `unity_auth_bypass`) |
| `include/` | headers, `version.h`, the plugin ABI `kryoto_plugin.h` |

## Release

Edit `KRYOTOO_VERSION_STR` (and the three numbers beside it) in `include/version.h`, add the
matching `## [x.y.z]` section to `CHANGELOG.md`, and push to `master`. Nothing else cuts a release.

## License

See [LICENSE.md](LICENSE.md). Selling this is prohibited, modified or not.
