// ============================================================
// kryotoO.dll - every patch KryotoOnline applies.
//
// This is the half that writes to other modules' code. It links
// MinHook; the steam_api proxy no longer does, and that is the
// point of the split - the file whose export table has to match
// Valve's byte for byte is now pure forwarding, and a hook fix
// ships as one small DLL dropped next to it.
//
// What lives here:
//
//   1. SteamStub  Valve's executable wrapper checks, through the
//                 live client, that the account owns the REAL
//                 AppId. Under the Spacewar spoof it does not, so
//                 the stub refuses before the game reaches its
//                 entry point. This flips the branch it makes
//                 that decision on. Opt-in (GetStubbedLol),
//                 because the packaged path strips the stub with
//                 Steamless instead and a patch looking for a
//                 signature that is not there is only overhead.
//
//   2. The AppId/ownership spoof, on the live client:
//                 GetAppID reports ogAppId; BIsSubscribedApp,
//                 BIsAppInstalled, BIsDlcInstalled and
//                 UserHasLicenseForApp say yes for the game and
//                 its configured DLC (and only those);
//                 GetAppInstallDir points at the running game;
//                 GetFriendGamePlayed reports a friend on the
//                 spoofed id as playing ogAppId; and server
//                 browser requests for ogAppId go to the spoofed id
//                 the servers registered under.
//
//   3. Lobby isolation: every lobby search is filtered on this
//                 game's tag and widened to worldwide, because the
//                 spoofed AppId is one pool shared with every other
//                 game spoofed onto it.
//
// The spoof stands down entirely when there is no ogAppId, or when
// it equals AppId: there is no mapping to make, and hooking the
// live client to return what it already returns is pure risk. The
// same goes for lobby isolation on an AppId the game has to itself.
// ============================================================

#include <Windows.h>
#include <intrin.h>
#include <stdarg.h>

#include <atomic>
#include <vector>

#include "kryotoo_abi.h"
#include "kryotoo_config.h"
#include "kryotoo_plugins.h"

#include "../include/MinHook.h"

// ============================================================
// State
// ============================================================

static KryotoO_LogFn      g_Log = nullptr;
static kryotoo::Config    g_Config;
static kryotoo::Plugins   g_Plugins;
static std::vector<uint32_t> g_Dlc;

static uint32_t g_ForcedAppId = kryotoo::kSpacewarAppId;
static uint32_t g_OgAppId = 0;

static bool g_MinHookUp = false;
static bool g_StubHookOn = false;
static bool g_StartedUp = false;

/// Logging goes through the proxy so both halves land in one file
/// in one order. A null logger is normal when the core is loaded
/// by something that does not provide one; it must never crash.
static void Log(const char* fmt, ...)
{
    if (!g_Log)
        return;

    // The host's Log is printf-style and variadic, and there is no
    // portable way to forward a va_list into it. Format here and
    // hand it a finished string; "%s" keeps a stray percent in a
    // path or a plugin name from being read as a directive.
    char msg[2048] = { 0 };
    va_list args;
    va_start(args, fmt);
    _vsnprintf_s(msg, sizeof(msg), _TRUNCATE, fmt, args);
    va_end(args);

    g_Log("%s", msg);
}

/// MinHook is initialised once and shared by both patches.
static bool EnsureMinHook()
{
    if (g_MinHookUp)
        return true;

    MH_STATUS s = MH_Initialize();
    // ALREADY_INITIALIZED is success as far as this is concerned -
    // something else in the process got there first, and the hooks
    // below work either way.
    if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED)
    {
        Log("[KryotoOnline] MH_Initialize failed: %d", s);
        return false;
    }

    g_MinHookUp = true;
    return true;
}

// ============================================================
// Patch 1: SteamStub
//
// Original approach from DenuvoSanctuary, in Rust; this is the
// same idea rewritten to live inside the DLL that needs it.
//
// The stub calls GetTickCount as part of its check. Hook that,
// look back from the return address at the code that called us,
// and if the ownership comparison is sitting there, turn its
// JZ (0x84) into a JNZ (0x85). One site is enough - the stub
// asks once - so the hook disables itself immediately afterwards
// rather than scanning on every tick for the rest of the run.
// ============================================================

static constexpr uint8_t kStubSignature[] = { 0x44, 0x0F, 0xB6, 0xF8, 0x3C, 0x30, 0x0F, 0x84 };
/// How far back from the return address to look. The comparison is
/// within a few instructions of the call; a wider window only
/// raises the chance of matching something that is not the stub.
static constexpr size_t kStubScanBytes = 128;

typedef DWORD(WINAPI* GetTickCount_Fn)(void);
static GetTickCount_Fn g_OrigGetTickCount = nullptr;
static std::atomic<bool> g_StubPatched{ false };

static uint8_t* FindSignature(uint8_t* start, uint8_t* end, const uint8_t* sig, size_t len)
{
    if (!start || !end || end <= start || (size_t)(end - start) < len)
        return nullptr;

    for (uint8_t* p = start; p <= end - len; ++p)
    {
        if (memcmp(p, sig, len) == 0)
            return p;
    }
    return nullptr;
}

static DWORD WINAPI Hooked_GetTickCount(void)
{
    // Already done: fall straight through. This runs on the game's
    // hot path until the hook is removed, so it does no work.
    if (g_StubPatched.load(std::memory_order_acquire))
        return g_OrigGetTickCount();

    uint8_t* start = reinterpret_cast<uint8_t*>(_ReturnAddress());
    uint8_t* end = start + kStubScanBytes;

    DWORD oldProtect = 0;
    if (!VirtualProtect(start, kStubScanBytes, PAGE_EXECUTE_READWRITE, &oldProtect))
        return g_OrigGetTickCount();

    uint8_t* found = FindSignature(start, end, kStubSignature, sizeof(kStubSignature));
    if (found)
    {
        found[7] = 0x85;  // JZ -> JNZ: the ownership test now passes.
        g_StubPatched.store(true, std::memory_order_release);
    }

    VirtualProtect(start, kStubScanBytes, oldProtect, &oldProtect);

    if (found)
    {
        // Disabled, not removed: MH_RemoveHook frees the trampoline
        // this call is about to return through.
        MH_DisableHook(reinterpret_cast<LPVOID>(GetTickCount));
        Log("[KryotoOnline] SteamStub ownership branch patched");
    }

    return g_OrigGetTickCount();
}

static void InstallSteamStubPatch()
{
    if (!EnsureMinHook())
        return;

    LPVOID target = reinterpret_cast<LPVOID>(GetTickCount);
    MH_STATUS s = MH_CreateHook(target, &Hooked_GetTickCount,
                                reinterpret_cast<LPVOID*>(&g_OrigGetTickCount));
    if (s != MH_OK)
    {
        Log("[KryotoOnline] SteamStub: MH_CreateHook failed: %d", s);
        return;
    }

    s = MH_EnableHook(target);
    if (s != MH_OK)
    {
        Log("[KryotoOnline] SteamStub: MH_EnableHook failed: %d", s);
        return;
    }

    g_StubHookOn = true;
    Log("[KryotoOnline] SteamStub patch armed");
}

// ============================================================
// Patches 2+: the client hooks
//
// Everything below redirects a method on one of the live client's
// interfaces. Three things about that are easy to get wrong, and the
// 1.9 hooks got all three wrong:
//
//   CALLING CONVENTION  steamclient is built with MSVC, so on x86 its
//       interface methods are __thiscall: `this` in ECX, arguments on
//       the stack, callee pops them. A __cdecl detour reads `this`
//       off the stack (it is the first argument instead), and returns
//       without popping, so every 32-bit game that called a hooked
//       method with an argument came back with a broken stack. A
//       __fastcall detour with a dummy second parameter takes ECX and
//       EDX in registers and pops the rest, which is exactly thiscall.
//       x64 has one convention and needs none of this.
//
//   INTERFACE VERSION  Hooking the vtable of the version this build
//       was compiled against only catches games that use that version.
//       An older C++ game asks steamclient for SteamUtils009 or
//       SteamFriends017 itself, gets another adapter with its own
//       code, and never touches the hook. So every hook is installed on
//       each version whose slot layout is known, through the resolver
//       the proxy hands over, and a version this client does not have
//       is simply skipped.
//
//   ONE TRAMPOLINE PER TARGET  Each version's method is its own
//       function with its own trampoline, so each needs its own
//       detour to know which original to call. Hence the templates:
//       Detour<0> calls the first target's original, Detour<1> the
//       second's, and so on.
//
// A function hooked once is never hooked twice: the client folds
// identical adapter thunks together, so two versions often share one
// address, and the server pipe hands out the same code as the client.
// ============================================================

#if defined(_M_IX86)
#  define KCC       __fastcall
#  define KTHIS     void* self, void* /*edx*/
#  define KSELF(s)  s, nullptr
#else
#  define KCC
#  define KTHIS     void* self
#  define KSELF(s)  s
#endif

static constexpr int kMaxTargets = 6;

#define KRYOTO_DETOURS(fn) \
    { (void*)&fn<0>, (void*)&fn<1>, (void*)&fn<2>, (void*)&fn<3>, (void*)&fn<4>, (void*)&fn<5> }

struct VersionSlot
{
    const char* version;
    size_t      slot;
};

/// Every function this core has redirected, client and server.
static std::vector<void*> g_HookedTargets;

static bool g_SpoofOn = false;           // ogAppId set and different from AppId
static bool g_LobbyIsolation = false;
static bool g_LobbyWorldwide = false;
static char g_LobbyTagKey[32] = "kryoto_app";
static char g_LobbyTagValue[64] = { 0 };

static bool IsReadablePtr(const void* p)
{
    MEMORY_BASIC_INFORMATION mbi = {};
    if (!p || VirtualQuery(p, &mbi, sizeof(mbi)) == 0)
        return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
        return false;
    return true;
}

static bool IsExecutablePtr(const void* p)
{
    MEMORY_BASIC_INFORMATION mbi = {};
    if (!p || VirtualQuery(p, &mbi, sizeof(mbi)) == 0)
        return false;
    if (mbi.State != MEM_COMMIT)
        return false;
    return (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
                           PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

static bool AlreadyHooked(void* target)
{
    for (void* t : g_HookedTargets)
        if (t == target)
            return true;
    return false;
}

/// The function at `slot` of `version`'s vtable, or null when the
/// client does not have that version or the slot does not look like code.
static void* ResolveSlot(const KryotoO_Interfaces* ifaces, const char* version, size_t slot)
{
    void* iface = ifaces->GetInterface(version);
    if (!iface || !IsReadablePtr(iface))
        return nullptr;

    void** vtable = *reinterpret_cast<void***>(iface);
    if (!vtable || !IsReadablePtr(vtable + slot))
        return nullptr;

    void* target = vtable[slot];
    return IsExecutablePtr(target) ? target : nullptr;
}

/// Install one logical hook on every interface version in `where`.
/// `detours` and `originals` are parallel arrays of `maxTargets`;
/// `used` counts how many of them are taken and carries across calls,
/// so a second InstallHooks (the game server) appends rather than
/// overwriting a trampoline that is live.
static int HookAcross(const KryotoO_Interfaces* ifaces, const char* what,
                      const VersionSlot* where, size_t whereCount,
                      void* const* detours, void** originals, int* used, int maxTargets)
{
    int installed = 0;

    for (size_t i = 0; i < whereCount; ++i)
    {
        void* target = ResolveSlot(ifaces, where[i].version, where[i].slot);
        if (!target || AlreadyHooked(target))
            continue;

        if (*used >= maxTargets)
        {
            Log("[KryotoOnline] %s: out of detour slots, %s not hooked", what, where[i].version);
            break;
        }

        int n = *used;
        MH_STATUS s = MH_CreateHook(target, detours[n], &originals[n]);
        if (s != MH_OK)
        {
            Log("[KryotoOnline] %s (%s): MH_CreateHook failed: %d", what, where[i].version, s);
            continue;
        }

        s = MH_EnableHook(target);
        if (s != MH_OK)
        {
            Log("[KryotoOnline] %s (%s): MH_EnableHook failed: %d", what, where[i].version, s);
            MH_RemoveHook(target);
            continue;
        }

        ++*used;
        g_HookedTargets.push_back(target);
        ++installed;
        Log("[KryotoOnline] %s hooked (%s, slot %u)", what, where[i].version, (unsigned)where[i].slot);
    }

    return installed;
}

static bool IsUnlockedDlc(uint32_t appId)
{
    for (uint32_t id : g_Dlc)
        if (id == appId)
            return true;
    return false;
}

/// "The game is asking about itself": the real AppId, or the spoofed
/// one when no real one was configured.
static bool IsSelfApp(uint32_t appId)
{
    if (appId == 0)
        return false;
    return appId == g_OgAppId || appId == g_ForcedAppId;
}

/// The folder the running executable is in, for games that ask Steam
/// where they are installed. The answer Steam gives is for an AppId
/// this account does not have installed - or worse, for Spacewar.
static uint32_t ExeDir(char* out, uint32_t cch)
{
    if (!out || cch == 0)
        return 0;

    char path[MAX_PATH] = { 0 };
    DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        return 0;

    char* slash = strrchr(path, '\\');
    if (!slash)
        return 0;
    *slash = '\0';

    size_t len = strlen(path);
    if (len + 1 > cch)
        return 0;

    memcpy(out, path, len + 1);
    return (uint32_t)(len + 1);
}

// ---- ISteamUtils::GetAppID ----------------------------------
//
// Report ogAppId: the client genuinely thinks Spacewar is running,
// and a game building its state off that talks to the wrong title.

typedef uint32_t(KCC* Fn_GetAppID)(KTHIS);
static void* g_OrigGetAppID[kMaxTargets];
static int   g_UsedGetAppID = 0;
static std::atomic<bool> g_LoggedGetAppID{ false };

template <int I>
static uint32_t KCC Hooked_GetAppID(KTHIS)
{
    uint32_t real = reinterpret_cast<Fn_GetAppID>(g_OrigGetAppID[I])(KSELF(self));
    if (!g_LoggedGetAppID.exchange(true))
        Log("[KryotoOnline] GetAppID -> %u (client says %u)", g_OgAppId, real);
    return g_OgAppId;
}

// ---- ISteamApps --------------------------------------------
//
// Ownership questions about the game itself and the configured DLC
// answer yes. Every other app keeps the client's answer: a game told
// it owns everything behaves worse than one told the truth.

typedef bool(KCC* Fn_AppQuery)(KTHIS, uint32_t appId);
static void* g_OrigSubscribedApp[kMaxTargets];
static int   g_UsedSubscribedApp = 0;
static void* g_OrigDlcInstalled[kMaxTargets];
static int   g_UsedDlcInstalled = 0;
static void* g_OrigAppInstalled[kMaxTargets];
static int   g_UsedAppInstalled = 0;

template <int I>
static bool KCC Hooked_BIsSubscribedApp(KTHIS, uint32_t appId)
{
    bool owned = reinterpret_cast<Fn_AppQuery>(g_OrigSubscribedApp[I])(KSELF(self), appId);
    if (owned)
        return true;
    return (g_SpoofOn && appId == g_OgAppId) || IsUnlockedDlc(appId);
}

template <int I>
static bool KCC Hooked_BIsDlcInstalled(KTHIS, uint32_t appId)
{
    bool installed = reinterpret_cast<Fn_AppQuery>(g_OrigDlcInstalled[I])(KSELF(self), appId);
    return installed || IsUnlockedDlc(appId);
}

template <int I>
static bool KCC Hooked_BIsAppInstalled(KTHIS, uint32_t appId)
{
    bool installed = reinterpret_cast<Fn_AppQuery>(g_OrigAppInstalled[I])(KSELF(self), appId);
    return installed || IsSelfApp(appId) || IsUnlockedDlc(appId);
}

typedef uint32_t(KCC* Fn_GetAppInstallDir)(KTHIS, uint32_t appId, char* folder, uint32_t cch);
static void* g_OrigAppInstallDir[kMaxTargets];
static int   g_UsedAppInstallDir = 0;

template <int I>
static uint32_t KCC Hooked_GetAppInstallDir(KTHIS, uint32_t appId, char* folder, uint32_t cch)
{
    // The game's own folder is where it is running, whatever Steam
    // has on file for Spacewar or for a title this account never
    // installed. Games that load content from this path otherwise
    // load nothing.
    if (IsSelfApp(appId))
    {
        uint32_t n = ExeDir(folder, cch);
        if (n != 0)
            return n;
    }
    return reinterpret_cast<Fn_GetAppInstallDir>(g_OrigAppInstallDir[I])(KSELF(self), appId, folder, cch);
}

// ---- ISteamUser / ISteamGameServer::UserHasLicenseForApp ----
//
// After BeginAuthSession, a host asking "does this player own the
// game?" hears no - nobody in the session owns ogAppId - and kicks
// them. Answer yes for the game and its configured DLC. NoAuth (no
// auth session at all) is left alone; that is a real error.
//
// CSteamID crosses as a uint64: it is an 8-byte trivially-copyable
// class, passed on the stack on x86 and in a register on x64, which
// is exactly how a uint64 travels.

static constexpr int32_t kHasLicense = 0;
static constexpr int32_t kDoesNotHaveLicense = 1;

typedef int32_t(KCC* Fn_UserHasLicense)(KTHIS, uint64_t steamId, uint32_t appId);
static void* g_OrigUserLicense[kMaxTargets];
static int   g_UsedUserLicense = 0;

template <int I>
static int32_t KCC Hooked_UserHasLicenseForApp(KTHIS, uint64_t steamId, uint32_t appId)
{
    int32_t r = reinterpret_cast<Fn_UserHasLicense>(g_OrigUserLicense[I])(KSELF(self), steamId, appId);
    if (r == kDoesNotHaveLicense && ((g_SpoofOn && appId == g_OgAppId) || IsUnlockedDlc(appId)))
        return kHasLicense;
    return r;
}

// ---- ISteamFriends::GetFriendGamePlayed ---------------------
//
// A friend running the same spoofed game is "playing Spacewar". The
// game compares that against GetAppID - which now says ogAppId - and
// decides the friend is in another game: no Join button, no invite.
// Report the friend's game as ogAppId when it is the spoofed id.
//
// FriendGameInfo_t starts with a CGameID: AppId in the low 24 bits,
// the type in the next 8 (0 = a plain app), mod id above.

typedef bool(KCC* Fn_FriendGamePlayed)(KTHIS, uint64_t friendId, void* info);
static void* g_OrigFriendGame[kMaxTargets];
static int   g_UsedFriendGame = 0;

template <int I>
static bool KCC Hooked_GetFriendGamePlayed(KTHIS, uint64_t friendId, void* info)
{
    bool ok = reinterpret_cast<Fn_FriendGamePlayed>(g_OrigFriendGame[I])(KSELF(self), friendId, info);
    if (ok && info)
    {
        uint64_t* gameId = reinterpret_cast<uint64_t*>(info);
        uint32_t app = (uint32_t)(*gameId & 0xFFFFFFull);
        uint32_t type = (uint32_t)((*gameId >> 24) & 0xFFull);
        if (type == 0 && app == g_ForcedAppId)
            *gameId = (*gameId & ~0xFFFFFFull) | (uint64_t)(g_OgAppId & 0xFFFFFFu);
    }
    return ok;
}

// ---- ISteamMatchmaking: lobby search ------------------------
//
// AppId 480 is one lobby pool shared by Valve's sample and every
// game spoofed onto it. A search returns at most 50 lobbies, nearest
// first, so a game that does not filter on a key of its own gets
// other titles' lobbies and can join them, and one that does filter
// gets nothing once the pool is busy. Every search is filtered on
// this game's tag (which the proxy writes onto the lobbies it owns),
// and widened to worldwide unless the game chose a distance itself -
// "nearby" on a player base this small is usually an empty list.

typedef uint64_t(KCC* Fn_RequestLobbyList)(KTHIS);
typedef void(KCC* Fn_AddStringFilter)(KTHIS, const char* key, const char* value, int comparison);
typedef void(KCC* Fn_AddDistanceFilter)(KTHIS, int distance);

static constexpr size_t kSlotAddStringFilter = 5;   // SteamMatchMaking009
static constexpr size_t kSlotAddDistanceFilter = 9; // SteamMatchMaking009
static constexpr int kLobbyComparisonEqual = 0;
static constexpr int kLobbyDistanceWorldwide = 3;

static void* g_OrigRequestLobbyList[kMaxTargets];
static int   g_UsedRequestLobbyList = 0;
static void* g_OrigDistanceFilter[kMaxTargets];
static int   g_UsedDistanceFilter = 0;
static std::atomic<bool> g_GameSetDistance{ false };
static std::atomic<bool> g_LoggedLobbyFilter{ false };

template <int I>
static void KCC Hooked_AddDistanceFilter(KTHIS, int distance)
{
    g_GameSetDistance.store(true);
    reinterpret_cast<Fn_AddDistanceFilter>(g_OrigDistanceFilter[I])(KSELF(self), distance);
}

template <int I>
static uint64_t KCC Hooked_RequestLobbyList(KTHIS)
{
    void** vtable = *reinterpret_cast<void***>(self);
    bool gameChoseDistance = g_GameSetDistance.exchange(false);

    if (g_LobbyIsolation && g_LobbyTagValue[0])
        reinterpret_cast<Fn_AddStringFilter>(vtable[kSlotAddStringFilter])(
            KSELF(self), g_LobbyTagKey, g_LobbyTagValue, kLobbyComparisonEqual);

    if (g_LobbyWorldwide && !gameChoseDistance)
        reinterpret_cast<Fn_AddDistanceFilter>(vtable[kSlotAddDistanceFilter])(
            KSELF(self), kLobbyDistanceWorldwide);

    // The call above came back through Hooked_AddDistanceFilter.
    g_GameSetDistance.store(false);

    if (!g_LoggedLobbyFilter.exchange(true))
        Log("[KryotoOnline] Lobby search filtered: %s=%s%s%s",
            g_LobbyTagKey, g_LobbyIsolation ? g_LobbyTagValue : "(isolation off)",
            g_LobbyWorldwide ? ", worldwide" : "",
            gameChoseDistance ? " (game set its own distance)" : "");

    return reinterpret_cast<Fn_RequestLobbyList>(g_OrigRequestLobbyList[I])(KSELF(self));
}

// ---- ISteamMatchmakingServers: the server browser -----------
//
// Servers register under the spoofed AppId (the proxy initialises
// them with it). A game that asks for "servers of GetAppID()" now
// asks for ogAppId and finds none. Send those requests to the
// spoofed id. One detour per slot - each is its own method.

typedef void*(KCC* Fn_ServerList)(KTHIS, uint32_t app, void** filters, uint32_t count, void* response);
typedef void*(KCC* Fn_LanServerList)(KTHIS, uint32_t app, void* response);
static void* g_OrigServerList[kMaxTargets];  // indexed by vtable slot 0..5
static int   g_UsedServerListSlot[kMaxTargets] = { 0 };

static uint32_t MapServerApp(uint32_t app)
{
    return (g_SpoofOn && app == g_OgAppId) ? g_ForcedAppId : app;
}

template <int I>
static void* KCC Hooked_ServerList(KTHIS, uint32_t app, void** filters, uint32_t count, void* response)
{
    return reinterpret_cast<Fn_ServerList>(g_OrigServerList[I])(KSELF(self), MapServerApp(app), filters, count, response);
}

template <int I>
static void* KCC Hooked_LanServerList(KTHIS, uint32_t app, void* response)
{
    return reinterpret_cast<Fn_LanServerList>(g_OrigServerList[I])(KSELF(self), MapServerApp(app), response);
}

// ---- Where each hook goes -----------------------------------
//
// Slots are from the Steamworks headers of each version. Only
// versions whose layout up to the slot is certain are listed; a
// wrong slot would redirect a different method with a different
// signature, which is worse than not hooking at all.

// ISteamUtils: GetCSERIPPort (private) at 7 has kept GetAppID at 9
// since SteamUtils005.
static const VersionSlot kUtilsGetAppID[] = {
    { "SteamUtils010", 9 }, { "SteamUtils009", 9 }, { "SteamUtils008", 9 },
    { "SteamUtils007", 9 }, { "SteamUtils006", 9 }, { "SteamUtils005", 9 },
};

// ISteamApps: the first eight methods have not moved since 004.
static const VersionSlot kAppsSubscribedApp[] = {
    { "STEAMAPPS_INTERFACE_VERSION008", 6 }, { "STEAMAPPS_INTERFACE_VERSION007", 6 },
    { "STEAMAPPS_INTERFACE_VERSION006", 6 },
};
static const VersionSlot kAppsDlcInstalled[] = {
    { "STEAMAPPS_INTERFACE_VERSION008", 7 }, { "STEAMAPPS_INTERFACE_VERSION007", 7 },
    { "STEAMAPPS_INTERFACE_VERSION006", 7 },
};
static const VersionSlot kAppsInstallDir[] = { { "STEAMAPPS_INTERFACE_VERSION008", 18 } };
static const VersionSlot kAppsAppInstalled[] = { { "STEAMAPPS_INTERFACE_VERSION008", 19 } };

// ISteamUser 023 put GetAuthTicketForWebApi at 14, pushing this to 18.
static const VersionSlot kUserLicense[] = { { "SteamUser023", 18 } };
// ISteamGameServer: InitGameServer (private) at 0.
static const VersionSlot kGameServerLicense[] = { { "SteamGameServer015", 29 } };

// ISteamFriends 018 dropped SetPersonaName from slot 1.
static const VersionSlot kFriendsGamePlayed[] = {
    { "SteamFriends018", 7 }, { "SteamFriends017", 8 },
    { "SteamFriends016", 8 }, { "SteamFriends015", 8 },
};

// ISteamMatchmaking 009 has been current since 2012.
static const VersionSlot kMatchmakingRequestLobbyList[] = { { "SteamMatchMaking009", 4 } };
static const VersionSlot kMatchmakingDistanceFilter[] = { { "SteamMatchMaking009", 9 } };

static const char kServersVersion[] = "SteamMatchMakingServers002";

/// SteamFriends018 slot 7 is GetFriendGamePlayed only if 018 really
/// dropped SetPersonaName. The client folds identical adapter thunks,
/// so if 018's slot 7 is the same code as 017's slot 7 (which is
/// GetFriendPersonaName), the layout assumption is wrong for this
/// client and that one entry must not be hooked.
static bool FriendsLayoutLooksWrong(const KryotoO_Interfaces* ifaces)
{
    void* v18 = ResolveSlot(ifaces, "SteamFriends018", 7);
    void* v17Name = ResolveSlot(ifaces, "SteamFriends017", 7);
    return v18 && v17Name && v18 == v17Name;
}

static void InstallClientHooks(const KryotoO_Interfaces* ifaces)
{
    if (g_SpoofOn)
    {
        static void* const dGetAppID[] = KRYOTO_DETOURS(Hooked_GetAppID);
        HookAcross(ifaces, "GetAppID", kUtilsGetAppID, _countof(kUtilsGetAppID),
                   dGetAppID, g_OrigGetAppID, &g_UsedGetAppID, kMaxTargets);

        static void* const dInstallDir[] = KRYOTO_DETOURS(Hooked_GetAppInstallDir);
        HookAcross(ifaces, "GetAppInstallDir", kAppsInstallDir, _countof(kAppsInstallDir),
                   dInstallDir, g_OrigAppInstallDir, &g_UsedAppInstallDir, kMaxTargets);

        if (!ifaces->IsGameServer)
        {
            // kFriendsGamePlayed[0] is SteamFriends018; skip it when the
            // client's 018 is not laid out the way the SDK says.
            static void* const dFriend[] = KRYOTO_DETOURS(Hooked_GetFriendGamePlayed);
            size_t first = 0;
            if (FriendsLayoutLooksWrong(ifaces))
            {
                Log("[KryotoOnline] SteamFriends018 layout differs from the SDK; "
                    "GetFriendGamePlayed hooked on older versions only");
                first = 1;
            }
            HookAcross(ifaces, "GetFriendGamePlayed", kFriendsGamePlayed + first,
                       _countof(kFriendsGamePlayed) - first,
                       dFriend, g_OrigFriendGame, &g_UsedFriendGame, kMaxTargets);

            // Server browser: slot 1 (LAN) has its own signature.
            static void* const dServer[] = KRYOTO_DETOURS(Hooked_ServerList);
            static void* const dLan[] = KRYOTO_DETOURS(Hooked_LanServerList);
            for (size_t slot = 0; slot < 6; ++slot)
            {
                VersionSlot where = { kServersVersion, slot };
                void* const* detours = (slot == 1) ? dLan : dServer;
                HookAcross(ifaces, slot == 1 ? "RequestLANServerList" : "Request*ServerList",
                           &where, 1, detours + slot, g_OrigServerList + slot,
                           &g_UsedServerListSlot[slot], 1);
            }
        }
    }

    if (g_SpoofOn || !g_Dlc.empty())
    {
        static void* const dSubscribed[] = KRYOTO_DETOURS(Hooked_BIsSubscribedApp);
        HookAcross(ifaces, "BIsSubscribedApp", kAppsSubscribedApp, _countof(kAppsSubscribedApp),
                   dSubscribed, g_OrigSubscribedApp, &g_UsedSubscribedApp, kMaxTargets);

        static void* const dDlc[] = KRYOTO_DETOURS(Hooked_BIsDlcInstalled);
        HookAcross(ifaces, "BIsDlcInstalled", kAppsDlcInstalled, _countof(kAppsDlcInstalled),
                   dDlc, g_OrigDlcInstalled, &g_UsedDlcInstalled, kMaxTargets);

        static void* const dInstalled[] = KRYOTO_DETOURS(Hooked_BIsAppInstalled);
        HookAcross(ifaces, "BIsAppInstalled", kAppsAppInstalled, _countof(kAppsAppInstalled),
                   dInstalled, g_OrigAppInstalled, &g_UsedAppInstalled, kMaxTargets);

        static void* const dLicense[] = KRYOTO_DETOURS(Hooked_UserHasLicenseForApp);
        if (ifaces->IsGameServer)
            HookAcross(ifaces, "GameServer::UserHasLicenseForApp", kGameServerLicense,
                       _countof(kGameServerLicense), dLicense, g_OrigUserLicense,
                       &g_UsedUserLicense, kMaxTargets);
        else
            HookAcross(ifaces, "UserHasLicenseForApp", kUserLicense, _countof(kUserLicense),
                       dLicense, g_OrigUserLicense, &g_UsedUserLicense, kMaxTargets);
    }

    if (!ifaces->IsGameServer && (g_LobbyIsolation || g_LobbyWorldwide))
    {
        static void* const dDistance[] = KRYOTO_DETOURS(Hooked_AddDistanceFilter);
        HookAcross(ifaces, "AddRequestLobbyListDistanceFilter", kMatchmakingDistanceFilter,
                   _countof(kMatchmakingDistanceFilter), dDistance, g_OrigDistanceFilter,
                   &g_UsedDistanceFilter, kMaxTargets);

        static void* const dRequest[] = KRYOTO_DETOURS(Hooked_RequestLobbyList);
        HookAcross(ifaces, "RequestLobbyList", kMatchmakingRequestLobbyList,
                   _countof(kMatchmakingRequestLobbyList), dRequest, g_OrigRequestLobbyList,
                   &g_UsedRequestLobbyList, kMaxTargets);
    }
}

// ============================================================
// Exports
// ============================================================

extern "C" uint32_t __cdecl KryotoO_AbiVersion(void)
{
    return KRYOTOO_ABI_VERSION;
}

extern "C" int32_t __cdecl KryotoO_Startup(const KryotoO_Host* host, KryotoO_Config* outCfg)
{
    if (!host || host->StructSize != sizeof(KryotoO_Host))
        return 1;
    if (!outCfg || outCfg->StructSize != sizeof(KryotoO_Config))
        return 2;
    if (host->AbiVersion != KRYOTOO_ABI_VERSION)
        return 3;
    if (g_StartedUp)
        return 4;

    g_StartedUp = true;
    g_Log = host->Log;

    g_Config.Read();
    g_ForcedAppId = g_Config.AppId();
    g_OgAppId = g_Config.OgAppId();
    g_Dlc = g_Config.UnlockDlc();

    // There is something to spoof only when the real AppId is known
    // and is not the one Steam is being told. With no ogAppId, or with
    // ogAppId == AppId, the client already answers with the truth.
    g_SpoofOn = g_OgAppId != 0 && g_OgAppId != g_ForcedAppId;

    // Lobby filtering only makes sense on a shared AppId. A game run
    // as itself (ogAppId == AppId) has its own lobby pool already.
    bool sharedAppId = !(g_OgAppId != 0 && g_OgAppId == g_ForcedAppId);
    g_Config.LobbyTag(g_LobbyTagValue, sizeof(g_LobbyTagValue), g_OgAppId);
    g_LobbyIsolation = sharedAppId && g_Config.LobbyIsolation() && g_LobbyTagValue[0] != '\0';
    g_LobbyWorldwide = sharedAppId && g_Config.LobbyWorldwide();

    if (g_Config.HaveIni())
        Log("[KryotoOnline] Config: %s", g_Config.IniPath());
    else
        Log("[KryotoOnline] No kryoto-online.ini beside the executable; using defaults");

    Log("[KryotoOnline] AppId=%u ogAppId=%u dlc=%zu ticket=%d stub=%d lobbyTag=%s worldwide=%d",
        g_ForcedAppId, g_OgAppId, g_Dlc.size(),
        (int)g_Config.EmulateTicket(), (int)g_Config.SteamStubEnabled(),
        g_LobbyIsolation ? g_LobbyTagValue : "(off)", (int)g_LobbyWorldwide);

    outCfg->AppId = g_ForcedAppId;
    outCfg->OgAppId = g_OgAppId;
    outCfg->EmulateTicket = g_Config.EmulateTicket() ? 1 : 0;
    // What the ini asked for. The patch itself is armed later, in
    // KryotoO_LoadPlugins, so reporting `g_StubHookOn` here would
    // always answer 0.
    outCfg->SteamStubEnabled = g_Config.SteamStubEnabled() ? 1 : 0;
    outCfg->DlcCount = (uint32_t)g_Dlc.size();
    outCfg->Dlc = g_Dlc.empty() ? nullptr : g_Dlc.data();
    outCfg->HaveIni = g_Config.HaveIni() ? 1 : 0;
    outCfg->LobbyIsolation = g_LobbyIsolation ? 1 : 0;
    strncpy_s(outCfg->LobbyTagKey, sizeof(outCfg->LobbyTagKey), g_LobbyTagKey, _TRUNCATE);
    strncpy_s(outCfg->LobbyTagValue, sizeof(outCfg->LobbyTagValue), g_LobbyTagValue, _TRUNCATE);

    return 0;
}

extern "C" uint32_t __cdecl KryotoO_LoadPlugins(void)
{
    if (!g_StartedUp)
        return 0;

    char folder[MAX_PATH] = { 0 };
    g_Config.PluginsFolder(folder, sizeof(folder));
    g_Plugins.Load(folder, &Log);
    Log("[KryotoOnline] %zu plugin(s) loaded", g_Plugins.Count());

    // AFTER the plugins, which is where it has always gone.
    //
    // The hook fires on every GetTickCount until it finds its signature,
    // and what it inspects is the CALLER's code. Arming it earlier would
    // point it at each plugin's DllMain on the way past - an eight-byte
    // signature is unlikely to collide, but the reward for winning that
    // gamble is writing a byte into somebody else's function.
    if (g_Config.SteamStubEnabled())
        InstallSteamStubPatch();

    return (uint32_t)g_Plugins.Count();
}

extern "C" void __cdecl KryotoO_InstallHooks(const KryotoO_Interfaces* ifaces)
{
    if (!g_StartedUp || !ifaces || ifaces->StructSize != sizeof(KryotoO_Interfaces) || !ifaces->GetInterface)
        return;

    if (!g_SpoofOn && g_Dlc.empty() && !g_LobbyIsolation && !g_LobbyWorldwide)
    {
        Log("[KryotoOnline] Client hooks not needed (ogAppId=%u AppId=%u, no DLC, lobby filtering off)",
            g_OgAppId, g_ForcedAppId);
        return;
    }

    if (!EnsureMinHook())
        return;

    size_t before = g_HookedTargets.size();
    InstallClientHooks(ifaces);
    Log("[KryotoOnline] %s hooks: %zu new, %zu total",
        ifaces->IsGameServer ? "Game server" : "Client",
        g_HookedTargets.size() - before, g_HookedTargets.size());
}

extern "C" uint32_t __cdecl KryotoO_InitPlugins(const void* pluginCtx)
{
    if (!pluginCtx)
        return 0;
    return (uint32_t)g_Plugins.Init(
        reinterpret_cast<const KRYOTO_PluginContext*>(pluginCtx), &Log);
}

extern "C" void __cdecl KryotoO_Shutdown(void)
{
    g_Plugins.Shutdown();

    if (g_StubHookOn)
    {
        MH_DisableHook(reinterpret_cast<LPVOID>(GetTickCount));
        g_StubHookOn = false;
    }

    if (g_MinHookUp)
    {
        // Every hook at once. Individually disabling the vtable
        // slots would need the interface pointers again, and the
        // client may already be gone by the time this runs.
        MH_DisableHook(MH_ALL_HOOKS);
        MH_Uninitialize();
        g_MinHookUp = false;
    }

    g_HookedTargets.clear();
    g_Log = nullptr;
}

// ============================================================
// DllMain
//
// Does nothing. Everything happens in KryotoO_Startup, called
// explicitly by the proxy, because the work here loads libraries
// and patches code and none of that belongs under the loader
// lock any longer than it has to be.
// ============================================================

BOOL WINAPI DllMain(HMODULE module, DWORD reason, LPVOID reserved)
{
    (void)reserved;

    if (reason == DLL_PROCESS_ATTACH)
    {
        // Nothing here needs per-thread notifications, and a game
        // that spawns threads constantly pays for them.
        DisableThreadLibraryCalls(module);
    }
    return TRUE;
}
