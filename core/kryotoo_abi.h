// ============================================================
// The contract between steam_api(64).dll and kryotoO.dll.
//
// Two DLLs because they have two different jobs, and mixing them
// cost us releases:
//
//   steam_api(64).dll  is a Steamworks IMPERSONATOR. Its entire
//                      reason to exist is that its name and its
//                      export table match Valve's, so the game
//                      loads it instead. It forwards to the real
//                      steamclient and must not do anything a
//                      real steam_api wouldn't.
//
//   kryotoO.dll        is where every PATCH lives - the ini, the
//                      plugin loader, the SteamStub branch flip,
//                      the ownership/AppId spoof hooks. It is
//                      the only half that links MinHook and the
//                      only half that writes to another module's
//                      code.
//
// Keeping the patches in the impersonator meant every change to
// them was a change to a file that must stay byte-comparable to
// Valve's export list, and it meant a hotfix to a hook shipped as
// a new steam_api64.dll that every packaged game had to have
// swapped. Now a hook fix is a new kryotoO.dll dropped beside it.
//
// The proxy calls into this ABI and NEVER links kryotoO.dll -
// it is resolved with LoadLibrary/GetProcAddress at runtime, so
// a missing or older core degrades to "no patches" instead of
// refusing to load and leaving the player with a game that dies
// before it draws a frame. See include/kryotoo_host.h.
//
// Bump KRYOTOO_ABI_VERSION whenever a struct in here changes
// shape. The proxy checks it and refuses a mismatched core
// rather than reading a struct with the wrong layout.
// ============================================================
#pragma once

#include <stdint.h>

#define KRYOTOO_ABI_VERSION 2

// The x64 core is `kryotoO.dll`; the x86 one carries a suffix.
//
// Same folder, two architectures: a game that ships both
// steam_api.dll and steam_api64.dll side by side would otherwise
// need one file to be both, and LoadLibrary would just fail on
// whichever arch lost. The 64-bit build keeps the plain name
// because that is what nearly every release is.
#if defined(_M_AMD64) || defined(__x86_64__)
#  define KRYOTOO_CORE_DLL "kryotoO.dll"
#else
#  define KRYOTOO_CORE_DLL "kryotoO32.dll"
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef void(__cdecl* KryotoO_LogFn)(const char* fmt, ...);

// What the proxy hands the core on startup.
//
// `Log` is the proxy's own logger rather than a second one in the
// core: one process writing one file from two implementations
// interleaves badly, and a split log is the worst thing to hand
// somebody debugging why a game will not start.
typedef struct KryotoO_Host
{
    uint32_t      StructSize;   // == sizeof(KryotoO_Host)
    uint32_t      AbiVersion;   // == KRYOTOO_ABI_VERSION
    KryotoO_LogFn Log;          // may be null; the core then logs nothing
} KryotoO_Host;

// What the core hands back, read out of kryoto-online.ini.
//
// The proxy needs these because the Steamworks surface it
// implements is the thing that answers with them - DLC ownership
// and ticket emulation are replies to game calls, not patches.
typedef struct KryotoO_Config
{
    uint32_t StructSize;        // == sizeof(KryotoO_Config)

    uint32_t AppId;             // [Settings]AppId, default 480
    uint32_t OgAppId;           // [Settings]ogAppId, 0 when unset
    int32_t  EmulateTicket;     // [Settings]EmulateTicket
    int32_t  SteamStubEnabled;  // [Settings]GetStubbedLol, as configured

    // [Settings]UnlockDLC. Owned by the core and valid until
    // KryotoO_Shutdown; the proxy copies what it needs.
    uint32_t        DlcCount;
    const uint32_t* Dlc;

    // Whether an ini was actually found. False means every value
    // above is a default, which is the single most common reason
    // for "I set it up and nothing happened" - the proxy logs it
    // loudly rather than letting it pass silently.
    int32_t  HaveIni;

    // ---- ABI 2 -------------------------------------------------
    //
    // Lobby isolation. Every game spoofed as Spacewar shares one
    // lobby pool with every other game spoofed as Spacewar, so an
    // unfiltered RequestLobbyList fills its 50 results with other
    // titles' lobbies and the game's own never show up. When this is
    // on, lobbies this game owns are tagged LobbyTagKey=LobbyTagValue
    // and every lobby search is filtered on the same pair. The core
    // adds the filter (it hooks the client); the proxy writes the tag
    // (it is the half that sees LobbyCreated_t / LobbyEnter_t).
    int32_t  LobbyIsolation;    // [Settings]LobbyIsolation, default on when spoofing
    char     LobbyTagKey[32];   // always "kryoto_app"
    char     LobbyTagValue[64]; // [Settings]LobbyTag, else ogAppId, else the exe name
} KryotoO_Config;

// Resolve one of the live client's interfaces by version string
// ("SteamUtils009", "SteamMatchMaking009", ...). Null when the
// client does not know that version. Provided by the proxy, which
// owns the pipe and user the interface has to be created against.
typedef void* (__cdecl* KryotoO_GetInterfaceFn)(const char* version);

// What the proxy hands the core to hook.
//
// A resolver rather than a fixed set of pointers, because a game does
// not have to use the interface version this build was compiled
// against. An older C++ game asks steamclient for SteamUtils009 or
// SteamFriends017 directly, gets a different adapter with its own
// vtable, and walks straight past a hook installed on the latest one.
// With the resolver the core hooks every version whose layout it knows.
typedef struct KryotoO_Interfaces
{
    uint32_t               StructSize;   // == sizeof(KryotoO_Interfaces)
    KryotoO_GetInterfaceFn GetInterface;
    int32_t                IsGameServer; // resolving against the game-server pipe
} KryotoO_Interfaces;

// ---- Exports -------------------------------------------------
//
// Every one is __cdecl and C-linkage so GetProcAddress finds it
// under the plain name in both architectures.

// Checked before anything else is called.
uint32_t __cdecl KryotoO_AbiVersion(void);

// Read the ini and install the SteamStub patch if it is enabled.
// Called first thing in the proxy's DLL_PROCESS_ATTACH, because the
// AppId it reads back is what the proxy publishes to the environment
// and to steam_appid.txt a few lines later.
// Returns 0 on success. `outCfg->StructSize` must be filled in by
// the caller before the call.
int32_t __cdecl KryotoO_Startup(const KryotoO_Host* host, KryotoO_Config* outCfg);

// Load the plugin DLLs. Returns how many loaded.
//
// SEPARATE from startup, and it matters. A plugin's DllMain runs
// inside this call, and plugins have always been loaded at a point
// where the proxy had already published SteamAppId to the
// environment, written steam_appid.txt and initialised its locks.
// Folding this into KryotoO_Startup moved all of that after them,
// so a plugin reading SteamAppId at load time got nothing.
uint32_t __cdecl KryotoO_LoadPlugins(void);

// Install the spoof and lobby hooks on the live client's vtables.
// Called once SteamAPI_Init has resolved the client, and again from
// SteamGameServer_Init with the server pipe's resolver. Safe to call
// any number of times: a function already hooked is skipped, and each
// hook is installed independently, so one unknown interface version
// costs that hook and nothing else.
void __cdecl KryotoO_InstallHooks(const KryotoO_Interfaces* ifaces);

// Hand every loaded plugin its KRYOTO_PluginContext. The struct is
// built by the proxy (it owns the callback dispatcher the context
// points into), so it crosses as an opaque pointer. Returns the
// number of plugins that reported success.
uint32_t __cdecl KryotoO_InitPlugins(const void* pluginCtx);

// Shut plugins down and remove every hook. Idempotent.
void __cdecl KryotoO_Shutdown(void);

#ifdef __cplusplus
}
#endif
