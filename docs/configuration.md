# Configuring and extending kryoto-online

## What ships

| File | What it is |
|---|---|
| `x64/steam_api64.dll`, `x86/steam_api.dll` | The proxy. Forwards to the real Steam client and nothing else. |
| `x64/kryotoO.dll`, `x86/kryotoO32.dll` | The core. Every patch lives here. |
| `plugins/*.dll` | Per-game plugins. Opt-in - see below. |
| `patch.bat` | Photon auto-configurator for Unity games. |

**Both DLLs go in the game folder, together.** The proxy loads the core
from beside itself, falling back to beside the game's executable. Without
it the Steamworks forwarding still works and nothing is patched - no
ownership spoof, no DLC, no plugins, no SteamStub handling - and the log
says so on the first line.

They must come from the same release. The proxy checks the core's ABI
version and refuses a mismatched one rather than reading a struct with the
wrong shape.

The x86 core carries a suffix because a game shipping both architectures
in one folder needs both cores in that folder, and one filename cannot be
two files.

## Why two DLLs

`steam_api64.dll` only works because of its name: the game loads it
believing it is Valve's, and its export table has to match. That makes it
the worst possible home for anything that changes - every hook fix used to
mean shipping a new impersonator into every packaged game.

So the impersonation and the patching are now separate files. `kryotoO.dll`
holds the ini, the plugin loader, the SteamStub patch and the spoof hooks;
it is the only half that links MinHook and the only half that writes to
another module's code. A hook fix is now one small DLL dropped beside the
proxy.

`core/kryotoo_abi.h` is the contract between them.

## Configure

Put a `kryoto-online.ini` **next to the game's executable** - not next to
the DLL. The two are the same folder for most games and are not for Unity
ones, where the DLL lives under `<Game>_Data\Plugins\x86_64\`.

```ini
[Settings]
AppId=480
ogAppId=220
PluginsFolder=plugins
GetStubbedLol=false
UnlockDLC=123,456,789
EmulateTicket=true
LobbyIsolation=true
LobbyWorldwide=true
LobbyTag=
```

| Key | What it does |
|---|---|
| `AppId` | What the game is told it is. `480` (Spacewar) unless it refuses, in which case try `440`. |
| `ogAppId` | What it really is, so stats, achievements and DLC resolve against the right title. Unset disables the spoof hooks entirely. |
| `PluginsFolder` | Folder of extra DLLs to load, relative to the executable. |
| `GetStubbedLol` | Defeat SteamStub at runtime. See below. |
| `UnlockDLC` | Comma-separated AppIDs to report as owned and installed. |
| `EmulateTicket` | Emulate the encrypted app ticket. Everything else on `ISteamUser` (SteamID, session tickets, auth) still goes to the real client. |
| `LobbyIsolation` | Tag this game's lobbies and filter every lobby search on the tag. Default `true`. See below. |
| `LobbyWorldwide` | Search lobbies worldwide unless the game picks a distance itself. Default `true`. |
| `LobbyTag` | What the lobbies are tagged with. Default: `ogAppId`, or the exe name when there is none. |

**Without this file nothing is configured.** The core reports that on its
first log line and every setting takes its default: AppID 480 with no
`ogAppId`, no DLC, no plugins, no ticket emulation. The game launches and
behaves as though you had not set anything up, which is a confusing way to
spend an evening.

Logs go to `%TEMP%\kryoto-online.log`. Both DLLs write to it, in order.

### `GetStubbedLol` and SteamStub

SteamStub is Valve's *executable* wrapper - a separate layer from the
Steamworks API this DLL replaces. It asks the live client to unwrap the
exe against the game's real AppID, which the account does not own, so it
refuses before the game reaches its entry point.

Something has to deal with it, and there are two ways:

- **Strip it** with [Steamless](https://github.com/atom0s/Steamless), ahead
  of time. `GetStubbedLol=false`.
- **Patch it** at runtime. `GetStubbedLol=true`. The core hooks
  `GetTickCount`, finds the stub's ownership comparison on the caller's
  stack and inverts the branch.

For a game that has SteamStub, doing *neither* produces an executable that
will not start, and the two are not additive - if Steamless already
stripped it, the runtime patch spends the whole run looking for a
signature that is not there. Set it to `true` only for a build whose exe
is still wrapped.

Original runtime approach from DenuvoSanctuary, in Rust; rewritten here.

## Multiplayer on a shared AppId

Every game spoofed as Spacewar is, as far as Steam is concerned, the same
game. They share one lobby pool, one server list and one "playing" status,
and the core corrects for each:

- **Lobbies.** A lobby search returns at most 50 results, nearest first,
  from a pool every other Spacewar-spoofed game is also filling. A game
  that does not filter on its own key lists other titles' lobbies (and
  can join them); one that does gets nothing once the pool is busy. So a
  lobby this game creates is tagged `kryoto_app=<LobbyTag>`, every
  `RequestLobbyList` is filtered on that pair, and the search is widened
  to worldwide unless the game asked for a distance itself. Joining by
  invite, by friend or by lobby id is unaffected.

  **Everyone has to be on 1.10 or newer**, with the same `LobbyTag` (the
  default, `ogAppId`, already agrees between two copies of one game). A
  host on an older release creates untagged lobbies that a 1.10 search
  will not list. Set `LobbyIsolation=false` to go back to the raw pool.

- **Friends.** `GetFriendGamePlayed` reports a friend on the spoofed AppId
  as playing `ogAppId`, which is what the game compares against, so Join
  buttons and in-game friend lists work.

- **Dedicated and listen servers.** The game server always registers
  under the spoofed AppId, so clients can find and authenticate against
  it, and a server-browser request for `ogAppId` is sent to the spoofed
  one. `UserHasLicenseForApp` answers yes for the game and its configured
  DLC on both the client and the server side, so hosts stop kicking
  players for "not owning" a game nobody in the session owns.

None of this applies when `ogAppId` equals `AppId`: then the game runs as
itself and has its own pools already.

## Plugins

A plugin is a DLL in `PluginsFolder`, loaded alphabetically, holding the
per-game work that has no business in a generic Steam wrapper - Photon
backends, IL2CPP patches, auth-ticket synthesis. The ABI is
`include/kryoto_plugin.h` and it did not change when the core was split
out: a plugin never knew which module was calling it.

Three ship in this repository: `photon_universal`, `raft_mp` and
`unity_auth_bypass`. Each has its own README.
