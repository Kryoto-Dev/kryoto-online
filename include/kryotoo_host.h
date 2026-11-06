// The proxy's handle on kryotoO.dll.
//
// steam_api(64).dll does not link the core. It resolves it at
// runtime, and every wrapper below is safe to call when it did not
// resolve. That is deliberate: a game whose steam_api64.dll refuses
// to load dies at startup with no message anybody can act on, so a
// missing or mismatched core degrades to "the Steamworks forwarding
// works, no patches were applied" and says so in the log.
//
// The build tool is where that becomes an error. Kryoto Forge
// checks for kryotoO.dll in preflight and refuses to package a
// release without it, because a warning in a log nobody reads is
// how releases went out broken before.
#pragma once

#include <Windows.h>
#include <Shlwapi.h>

#include "../core/kryotoo_abi.h"

class CKryotoCore
{
public:
    /// Load the core and read its configuration.
    ///
    /// Called once from DLL_PROCESS_ATTACH. `self` is the proxy's own
    /// module handle, used to look beside it first.
    bool Load(HMODULE self, KryotoO_LogFn log)
    {
        if (m_Module)
            return true;

        m_Module = Find(self);
        if (!m_Module)
        {
            log("[KryotoOnline] %s not found - no patches will be applied. "
                "It belongs in the same folder as this DLL.", KRYOTOO_CORE_DLL);
            return false;
        }

        // The version first, on its own: a core from another release is
        // missing or renaming exports, and "speaks ABI 1, this build
        // speaks 2" says what to do about it where "missing exports"
        // does not.
        m_AbiVersion = (Fn_AbiVersion)GetProcAddress(m_Module, "KryotoO_AbiVersion");
        if (!m_AbiVersion)
        {
            log("[KryotoOnline] %s has no KryotoO_AbiVersion - ignoring it.", KRYOTOO_CORE_DLL);
            m_Module = nullptr;
            return false;
        }

        // A core from a different release would be read with the
        // wrong struct layout, which is a crash rather than a
        // misconfiguration. Refuse instead.
        uint32_t abi = m_AbiVersion();
        if (abi != KRYOTOO_ABI_VERSION)
        {
            log("[KryotoOnline] %s speaks ABI %u, this build speaks %u - ignoring it. "
                "Ship the two files from the same release.",
                KRYOTOO_CORE_DLL, abi, (uint32_t)KRYOTOO_ABI_VERSION);
            m_Module = nullptr;
            return false;
        }

        m_Startup = (Fn_Startup)GetProcAddress(m_Module, "KryotoO_Startup");
        m_LoadPlugins = (Fn_LoadPlugins)GetProcAddress(m_Module, "KryotoO_LoadPlugins");
        m_InstallHooks = (Fn_InstallHooks)GetProcAddress(m_Module, "KryotoO_InstallHooks");
        m_InitPlugins = (Fn_InitPlugins)GetProcAddress(m_Module, "KryotoO_InitPlugins");
        m_ShutdownFn = (Fn_Shutdown)GetProcAddress(m_Module, "KryotoO_Shutdown");

        if (!m_Startup || !m_LoadPlugins || !m_InstallHooks || !m_InitPlugins || !m_ShutdownFn)
        {
            log("[KryotoOnline] %s is missing exports - ignoring it.", KRYOTOO_CORE_DLL);
            m_Module = nullptr;
            return false;
        }

        KryotoO_Host host = {};
        host.StructSize = sizeof(host);
        host.AbiVersion = KRYOTOO_ABI_VERSION;
        host.Log = log;

        m_Config.StructSize = sizeof(m_Config);

        int32_t rc = m_Startup(&host, &m_Config);
        if (rc != 0)
        {
            log("[KryotoOnline] %s startup returned %d - no patches applied.", KRYOTOO_CORE_DLL, rc);
            m_Module = nullptr;
            return false;
        }

        m_Ready = true;
        return true;
    }

    bool Ready() const { return m_Ready; }

    /// Configuration read from the ini. When the core is absent this
    /// is the default set - Spacewar, no ogAppId, nothing unlocked -
    /// which is exactly what an unconfigured install produces anyway.
    const KryotoO_Config& Config() const { return m_Config; }

    /// Load the plugin DLLs. Called at the point in DLL_PROCESS_ATTACH
    /// where plugins have always been loaded - after the AppId is published
    /// and the locks are up, because a plugin's DllMain runs inside this.
    unsigned int LoadPlugins()
    {
        return m_Ready ? m_LoadPlugins() : 0u;
    }

    /// Hook the live client. `resolve` hands back an interface by
    /// version string against whichever pipe is being hooked.
    void InstallHooks(KryotoO_GetInterfaceFn resolve, bool gameServer)
    {
        if (!m_Ready || !resolve)
            return;

        KryotoO_Interfaces ifaces = {};
        ifaces.StructSize = sizeof(ifaces);
        ifaces.GetInterface = resolve;
        ifaces.IsGameServer = gameServer ? 1 : 0;
        m_InstallHooks(&ifaces);
    }

    unsigned int InitPlugins(const void* ctx)
    {
        return m_Ready ? m_InitPlugins(ctx) : 0u;
    }

    void Shutdown()
    {
        if (m_Ready)
        {
            m_ShutdownFn();
            m_Ready = false;
        }
    }

private:
    /// Beside this DLL first, then beside the executable.
    ///
    /// Those are the same folder for most games and are not for a
    /// Unity one, where this DLL sits under <Game>_Data\Plugins\x86_64\
    /// and the exe is at the root - the same split that decides where
    /// kryoto-online.ini has to go. Accepting either means one copy of
    /// the core serves a tree however it is laid out.
    static HMODULE Find(HMODULE self)
    {
        char path[MAX_PATH] = { 0 };

        if (GetModuleFileNameA(self, path, MAX_PATH) != 0 && PathRemoveFileSpecA(path))
        {
            if (HMODULE m = TryLoad(path))
                return m;
        }

        if (GetModuleFileNameA(nullptr, path, MAX_PATH) != 0 && PathRemoveFileSpecA(path))
        {
            if (HMODULE m = TryLoad(path))
                return m;
        }

        return nullptr;
    }

    static HMODULE TryLoad(const char* dir)
    {
        char full[MAX_PATH] = { 0 };
        if (_snprintf_s(full, MAX_PATH, _TRUNCATE, "%s\\%s", dir, KRYOTOO_CORE_DLL) == _TRUNCATE)
            return nullptr;

        if (GetFileAttributesA(full) == INVALID_FILE_ATTRIBUTES)
            return nullptr;

        // LOAD_WITH_ALTERED_SEARCH_PATH so the core's own imports
        // resolve from its folder rather than from the game's root.
        return LoadLibraryExA(full, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    }

    typedef uint32_t(__cdecl* Fn_AbiVersion)(void);
    typedef int32_t(__cdecl* Fn_Startup)(const KryotoO_Host*, KryotoO_Config*);
    typedef uint32_t(__cdecl* Fn_LoadPlugins)(void);
    typedef void(__cdecl* Fn_InstallHooks)(const KryotoO_Interfaces*);
    typedef uint32_t(__cdecl* Fn_InitPlugins)(const void*);
    typedef void(__cdecl* Fn_Shutdown)(void);

    HMODULE m_Module = nullptr;
    bool    m_Ready = false;

    Fn_AbiVersion        m_AbiVersion = nullptr;
    Fn_Startup           m_Startup = nullptr;
    Fn_LoadPlugins       m_LoadPlugins = nullptr;
    Fn_InstallHooks      m_InstallHooks = nullptr;
    Fn_InitPlugins       m_InitPlugins = nullptr;
    Fn_Shutdown          m_ShutdownFn = nullptr;

    // Defaults stand in when the core is absent: Spacewar (480), no
    // ogAppId, nothing unlocked, no ticket emulation. Same values the
    // core produces from a missing ini, so the two paths agree.
    KryotoO_Config m_Config = {
        sizeof(KryotoO_Config),  // StructSize
        480,                     // AppId
        0,                       // OgAppId
        0,                       // EmulateTicket
        0,                       // SteamStubEnabled
        0,                       // DlcCount
        nullptr,                 // Dlc
        0,                       // HaveIni
        0,                       // LobbyIsolation: off without a core to filter searches
        "kryoto_app",            // LobbyTagKey
        "",                      // LobbyTagValue
    };
};
