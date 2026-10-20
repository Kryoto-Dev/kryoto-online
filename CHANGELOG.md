# Changelog

The `## [x.y.z]` heading for the version in `include/version.h` becomes
the body of that version's GitHub release, so write the entry before you
push the bump.

## [1.9.1] - 2026-10-20

Maintenance release for the Kryoto 0.13.0 update. No player-facing changes.

## [1.9.0] - 2026-10-09

### Added

- **The lobby you are in, for Kryoto Desktop's game invites.** While a
  game is in a Steam lobby, `steam_api(64).dll` writes
  `%LOCALAPPDATA%\Kryoto\online\<pid>.json` with the lobby id, your
  SteamID, the real AppId and a timestamp it refreshes every 30 seconds.
  Kryoto Desktop reads it to send a friend an invite that drops them into
  the same lobby (`+connect_lobby` or `steam://joinlobby/480/...`), not
  just into the same game.

  It learns the lobby from `LobbyEnter_t` and `LobbyCreated_t` (callbacks
  and call results, standard and manual dispatch), notices leaving through
  the flat `LeaveLobby` or, for games that call it through the C++
  interface, by checking membership every 5 seconds, and deletes the file
  on leave, `SteamAPI_Shutdown` and exit. Nothing leaves the machine; it
  is one local file. Under Wine/Proton it lands inside the prefix, where
  Kryoto Desktop looks for it too.

## [1.8.1] - 2026-08-27

### Added

- **`kryotoO.dll`** - a second DLL holding every patch: the ini, the
  plugin loader, the SteamStub branch flip and the ownership/AppId spoof
  hooks. `steam_api(64).dll` no longer patches anything and no longer
  links MinHook; it forwards to the real Steam client and calls into the
  core across a versioned C ABI (`core/kryotoo_abi.h`). Its export table
  is unchanged - all 1190 Steamworks exports, byte for byte.

  Ship both files together. The x86 core is `kryotoO32.dll`, so a game
  carrying both architectures in one folder can hold both cores.

- `include/version.h` as the one place the version lives. Both resource
  scripts stamp themselves from it and the release workflow reads it.

### Fixed

- **`Steam_GetHSteamUserCurrent` called itself** and would take the game
  down with a stack overflow the first time anything used it. MSVC had
  been reporting it (C4717) on every build since the export was written.
  It now returns the cached user handle, like the two exports next to it.

- **Releases were being cut on every push to `master`**, named and tagged
  after the branch instead of a version - which is where this repository's
  `master` and `test` tags came from, and why Kryoto Forge was installing
  something called `kryoto-online-master-debug.zip`. Releases now fire on
  a version bump only.

- **Release zips no longer wrap their contents in a directory.** The arch
  folders are at the root of the archive. Forge unpacks the zip into its
  tools folder and looks for `x64/steam_api64.dll` directly underneath;
  the wrapper directory put everything one level below anything that
  looked, so the install reported success and no online build ever found
  an emulator.

- **The x64 build stamped `OriginalFilename` as `steam_api.dll`.** It was
  gated on an `IS_64BIT` macro that nothing defined. Worth knowing if you
  touch a `.rc`: `rc.exe` predefines neither `_WIN64` nor `_M_AMD64`, so
  swapping in one of those fixes nothing. The arch macro is passed from
  the project file now, and the stamp is worth checking with
  `(Get-Item out.dll).VersionInfo` rather than trusting the `#ifdef`.

- **The Debug configuration was a Release build.** It defined `NDEBUG`,
  linked the release CRT and turned on whole-program optimisation, so the
  minidump handler - the only thing that made it a debug build - was
  never compiled once. `steam_api(64).dll` now builds Debug properly:
  `_DEBUG`, the debug CRT, no optimisation, and the minidump handler
  actually in the image. `kryotoO.dll` keeps the release CRT even in
  Debug, because the vendored `libMinHook.*.lib` is a `/MT` build with no
  debug variant and mixing the two puts two CRT heaps in one module -
  it gets optimisation off and full PDBs instead.

### Changed

- Plugin shutdown no longer calls `FreeLibrary`. Its only caller is
  process teardown, which holds the loader lock, where `FreeLibrary` is
  documented to deadlock. The process is exiting; Windows unmaps the
  modules regardless.

- `UnlockDLC` is parsed with `strtok_s`. The old `strtok` kept its state
  in a global that any plugin thread could walk over.

- The PR and release workflows share one composite action instead of
  keeping two copies of the same build steps.

### Removed

- `GetForceOwnership`, an ini setting nothing ever read.

## [1.7.0]

Last release before the changelog existed. See the commit history.
