#include <Windows.h>
#include <Shlwapi.h>
#include <DbgHelp.h>
#include <new.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <vector>

#define STEAM_API_EXPORTS

#include "include/sdk/steam_api.h"
#include "include/sdk/steamclientpublic.h"
#include "include/sdk/steam_gameserver.h"
#include "include/sdk/steamtypes.h"

S_API ISteamClient* g_pSteamClientGameServer = nullptr;

#include <vector>

#include "include/registfuncs.h"
#include "include/callback_dispatcher.h"
#include "include/kryoto_plugin.h"
#include "include/globals.h"
#include "include/kryotoo_host.h"
#include "include/dump_handler.h"
#include "include/lobby_watch.h"

#include "include/api/api_callbacks.h"
#include "include/api/api_client.h"
#include "include/api/api_interfaces.h"
#include "include/api/api_interfaces_v.h"
#include "include/api/api_gameserver.h"
#include "include/api/api_breakpad.h"
#include "include/api/api_shutdown.h"
#include "include/api/api_factory.h"
#include "include/api/api_flat.h"

// ============================================================
// Global variable definitions
// ============================================================

HMODULE g_ClientModule = nullptr;
HSteamPipe g_ClientPipe = 0;
HSteamUser g_ClientUser = 0;
ISteamClient* g_pSteamClient = nullptr;
ISteamClient* g_pSteamClientSafe = nullptr;
ISteamUtils* g_pUtilsForCallbacks = nullptr;
ISteamController* g_pControllerForCallbacks = nullptr;
ISteamInput* g_pInputForCallbacks = nullptr;
CSteamAPIContext g_ClientCtx;
bool g_bClientReady = false;
SRWLOCK g_CtxLock;

HMODULE g_ServerModule = nullptr;
HSteamPipe g_ServerPipe = 0;
HSteamUser g_ServerUser = 0;
ISteamClient* g_ServerClient = nullptr;
ISteamClient* g_pServerClient = nullptr;
ISteamClient* g_pSteamClientGameServer_Latest = nullptr;
ISteamGameServer* g_pGameServer = nullptr;
ISteamUtils* g_pServerUtils = nullptr;
CSteamGameServerAPIContext g_ServerCtx;
bool g_bServerReady = false;
EServerMode g_ServerMode = eServerModeInvalid;

bool g_bUsingBreakpad = false;
bool g_bFullDumps = false;
void* g_BreakpadCtx = nullptr;
PFNPreMinidumpCallback g_BreakpadCallback = nullptr;
char g_BreakpadVer[64] = { 0 };
char g_BreakpadTimestamp[64] = { 0 };
uint32 g_BreakpadAppID = 0;
uint64 g_BreakpadSteamID = 0;

bool g_bTryCatch = false;
int g_DispatchMode = 0;
char g_InstallPath[MAX_PATH] = { 0 };
bool g_bHaveInstallPath = false;
SRWLOCK g_CallbackLock;
uint32 g_ForcedAppId = 480;
uint32 g_OriginalAppId = 0;

Fn_CreateInterface g_pfnCreateInterface = nullptr;
Fn_ReleaseThreadLocal g_pfnReleaseThreadLocal = nullptr;
Fn_IsKnownInterface g_pfnIsKnownInterface = nullptr;
Fn_NotifyMissing g_pfnNotifyMissing = nullptr;
Fn_BreakpadInit g_pfnBreakpadInit = nullptr;
Fn_BreakpadSetAppID g_pfnBreakpadSetAppID = nullptr;
Fn_BreakpadSetSteamID g_pfnBreakpadSetSteamID = nullptr;
Fn_BreakpadSetComment g_pfnBreakpadSetComment = nullptr;
Fn_BreakpadWriteDump g_pfnBreakpadWriteDump = nullptr;

uintp g_CtxCounter = 0;

// ============================================================
// SteamInternal_ContextInit
// ============================================================

S_API void* S_CALLTYPE SteamInternal_ContextInit(void* pData)
{
	KRYOTOLOG("[KryotoOnline] SteamInternal_ContextInit");
	if (!pData) return nullptr;
	void** pArr = (void**)pData;
	uintp* pCounter = (uintp*)&pArr[1];
	char* pBase = (char*)pData;
	#if defined(_M_IX86)
		if (*pCounter == g_CtxCounter) return pBase + 8;
		AcquireSRWLockExclusive(&g_CtxLock);
		if (*pCounter != g_CtxCounter) { void(*pFn)(void*) = (void(*)(void*))pArr[0]; pFn(pBase + 8); *pCounter = g_CtxCounter; }
		ReleaseSRWLockExclusive(&g_CtxLock);
		return pBase + 8;
	#elif defined(_M_AMD64)
		if (*pCounter == g_CtxCounter) return pBase + 16;
		AcquireSRWLockExclusive(&g_CtxLock);
		if (*pCounter != g_CtxCounter) { void(*pFn)(void*) = (void(*)(void*))pArr[0]; pFn(pBase + 16); *pCounter = g_CtxCounter; }
		ReleaseSRWLockExclusive(&g_CtxLock);
		return pBase + 16;
	#endif
}

// ============================================================
// Logging
// ============================================================


static void KryotoLogImpl(const char* fmt, va_list args)
{
	char msg[2048] = { 0 };
	_vsnprintf_s(msg, sizeof(msg), _TRUNCATE, fmt, args);

	char logPath[MAX_PATH] = { 0 };
	DWORD len = GetTempPathA(MAX_PATH, logPath);
	if (len == 0 || len > (MAX_PATH - 25)) return;

	if (!PathAppendA(logPath, "kryoto-online.log")) return;

	FILE* f = nullptr;
	if (fopen_s(&f, logPath, "ab") != 0 || !f) return;

	SYSTEMTIME st = { 0 };
	GetLocalTime(&st);

	fprintf(f, "[%04u-%02u-%02u %02u:%02u:%02u.%03u] %s",
		st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, msg);

	size_t msgLen = strlen(msg);
	if (msgLen == 0 || msg[msgLen - 1] != '\n')
		fputs("\r\n", f);

	fclose(f);
}

void KRYOTOLOG(const char* fmt, ...)
{
	if (!fmt) return;
	va_list args;
	va_start(args, fmt);
	KryotoLogImpl(fmt, args);
	va_end(args);
}

void KryotoColor(WORD color, const char* text)
{
	(void)color;
	if (text && text[0])
		KRYOTOLOG("%s", text);
}

// ============================================================
// InitSteamClient
//
// Load the real steamclient and get one interface out of it.
//
// The install path comes from the registry (include/registfuncs.h).
// When Steam is not running, or the path is unusable, the fallback is
// a plain LoadLibrary by name - which only finds anything if a
// steamclient happens to sit beside the game, and is why `bLocal`
// exists: a caller that cannot use a local client says so and gets
// null instead of a surprise.
// ============================================================

void* InitSteamClient(HMODULE* phMod, bool bLocal, const char* iface)
{
	g_pUtilsForCallbacks = nullptr;
	g_pControllerForCallbacks = nullptr;
	g_pInputForCallbacks = nullptr;

	if (!phMod || !iface) return nullptr;

	*phMod = nullptr;

	char dllPath[MAX_PATH] = { 0 };
	const char* installDir = SteamAPI_GetSteamInstallPath();

	if (_stricmp(installDir, "KryotoOnline_InvalidPath") == 0)
	{
		if (!bLocal) return nullptr;
	}
	else
	{
		#if defined(_M_IX86)
			_snprintf_s(dllPath, MAX_PATH, _TRUNCATE, "%s\\steamclient.dll", installDir);
		#elif defined(_M_AMD64)
			_snprintf_s(dllPath, MAX_PATH, _TRUNCATE, "%s\\steamclient64.dll", installDir);
		#endif
	}

	if (SteamAPI_IsSteamRunning())
	{
		*phMod = LoadLibraryExA(dllPath, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
		if (!*phMod)
			KryotoColor(0, "[KryotoOnline] Failed to load steamclient DLL");
	}
	else
	{
		KryotoColor(0, "[KryotoOnline] Steam is not running");
	}

	if (!*phMod)
	{
		if (!bLocal)
		{
			KryotoColor(0, "[KryotoOnline] No Steam instance and bLocal is false");
			return nullptr;
		}

		#if defined(_M_IX86)
			*phMod = LoadLibraryW(L"steamclient.dll");
		#elif defined(_M_AMD64)
			*phMod = LoadLibraryW(L"steamclient64.dll");
		#endif

		if (!*phMod)
		{
			KryotoColor(0, "[KryotoOnline] Cannot find steamclient DLL");
			return nullptr;
		}
	}

	g_pfnCreateInterface = (Fn_CreateInterface)GetProcAddress(*phMod, "CreateInterface");

	if (g_pfnCreateInterface)
	{
		g_pSteamClientSafe = (ISteamClient*)g_pfnCreateInterface("SteamClient023", nullptr);
		g_pfnReleaseThreadLocal = (Fn_ReleaseThreadLocal)GetProcAddress(*phMod, "Steam_ReleaseThreadLocalMemory");
		g_CtxCounter++;

		return g_pfnCreateInterface(iface, nullptr);
	}
	else
	{
		KryotoColor(0, "[KryotoOnline] CreateInterface not found in steamclient");
		FreeLibrary(*phMod);
		*phMod = nullptr;
	}

	return nullptr;
}


// ============================================================
// LoadGameOverlay
// ============================================================

static void LoadGameOverlay()
{
#if defined(_WIN32)
	#if defined(_M_IX86)
		HMODULE hOverlay = GetModuleHandleW(L"GameOverlayRenderer.dll");
	#elif defined(_M_AMD64)
		HMODULE hOverlay = GetModuleHandleW(L"GameOverlayRenderer64.dll");
	#endif

	if (g_ForcedAppId != 769 && !hOverlay)
	{
		const char* installPath = SteamAPI_GetSteamInstallPath();
		if (_stricmp(installPath, "KryotoOnline_InvalidPath") != 0)
		{
			char overlayPath[MAX_PATH] = { 0 };
			#if defined(_M_IX86)
				_snprintf_s(overlayPath, MAX_PATH, _TRUNCATE, "%s\\GameOverlayRenderer.dll", installPath);
			#elif defined(_M_AMD64)
				_snprintf_s(overlayPath, MAX_PATH, _TRUNCATE, "%s\\GameOverlayRenderer64.dll", installPath);
			#endif
			HMODULE hLoaded = LoadLibraryExA(overlayPath, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
			if (hLoaded)
			{
				KRYOTOLOG("[KryotoOnline] Loaded game overlay: %s", overlayPath);
			}
			else
			{
				KRYOTOLOG("[KryotoOnline] Failed to load game overlay: %s (error %lu)", overlayPath, GetLastError());
			}
		}
	}
#endif // _WIN32
}

// ============================================================
// DllMain
// ============================================================

// File-scope so the SteamAPI_Init path in api_client.h can reach it:
// that is where the Steam interfaces the spoof hooks need finally
// exist, and where the plugin context is built.
CKryotoCore s_Core;

BOOL WINAPI DllMain(HMODULE hModule, DWORD dwReason, LPVOID lpReserved)
{
	if (dwReason == DLL_PROCESS_ATTACH)
	{
		KRYOTOLOG("[KryotoOnline] DllMain -> DLL_PROCESS_ATTACH");

		// Before anything reads g_ForcedAppId: SetAppIDEnv and
		// WriteAppIDFile below both publish it, and the overlay is
		// loaded off the back of it.
		s_Core.Load(hModule, &KRYOTOLOG);
		const KryotoO_Config& cfg = s_Core.Config();
		g_ForcedAppId = cfg.AppId;
		g_OriginalAppId = cfg.OgAppId;
		KryotoLobby::SetTag(cfg.LobbyIsolation != 0, cfg.LobbyTagKey, cfg.LobbyTagValue);

		SetAppIDEnv();
		WriteAppIDFile();

		// Load the game overlay early to ensure it can hook into graphics APIs
		LoadGameOverlay();

		char dllPath[MAX_PATH] = { 0 };
		DWORD len = GetModuleFileNameA(hModule, dllPath, sizeof(dllPath));

		if (len == 0)
			return FALSE;

		if (GetLastError() == ERROR_INSUFFICIENT_BUFFER)
			return FALSE;

		KRYOTOLOG("[KryotoOnline] DLL Path: %s", dllPath);

		// A game loads this because of its FILENAME - nothing else
		// about it says Steamworks. Renamed, it is inert, and the
		// symptom is a game that behaves as though no crack was
		// applied at all. One line in the log beats guessing.
		#if defined(_M_IX86)
			if (!StrStrIA(dllPath, "steam_api.dll"))
				KRYOTOLOG("[KryotoOnline] Warning: not named steam_api.dll");
		#elif defined(_M_AMD64)
			if (!StrStrIA(dllPath, "steam_api64.dll"))
				KRYOTOLOG("[KryotoOnline] Warning: not named steam_api64.dll");
		#endif

		KRYOTOLOG("[KryotoOnline] PID: %lu", GetCurrentProcessId());
		KRYOTOLOG("[KryotoOnline] Thread: %lu", GetCurrentThreadId());

		InitializeSRWLock(&g_CtxLock);
		InitializeSRWLock(&g_CallbackLock);

		#ifdef _DEBUG
		g_pDumpHandler = new CDumpHandler();
		#endif

		// Here, not inside the core's startup call: a plugin's DllMain runs
		// inside this, and plugins have always been loaded at a point where
		// SteamAppId is already in the environment, steam_appid.txt is
		// written and the locks above exist.
		s_Core.LoadPlugins();

		// DLC unlocking and ticket emulation are answers this DLL
		// gives to game calls, not patches, so they stay on this
		// side of the split - the core only reads them out of the
		// ini and hands them over.
		std::vector<uint32> unlockedDLC;
		if (cfg.Dlc && cfg.DlcCount)
			unlockedDLC.assign(cfg.Dlc, cfg.Dlc + cfg.DlcCount);
		CSteamAppsStub::SetUnlockedDLCAppIds(unlockedDLC);

		if (cfg.EmulateTicket)
		{
			// Tickets are minted for the REAL title. Falling back to
			// AppId keeps an ogAppId-less config working rather than
			// emitting a ticket for app 0.
			uint32 emulatedAppId = cfg.OgAppId != 0 ? cfg.OgAppId : cfg.AppId;
			CSteamUserStub::SetEmulatedApp(emulatedAppId);
		}
	}
	else if (dwReason == DLL_PROCESS_DETACH)
	{
		KRYOTOLOG("[KryotoOnline] DllMain -> DLL_PROCESS_DETACH");
		KryotoLobby::Shutdown();
		s_Core.Shutdown();
	}

	return TRUE;
}

// ============================================================
// CCallbackDispatcher
// ============================================================

// ============================================================
// Callback patcher registry
//
// Plugins (see include/kryoto_plugin.h) can register a callback
// patcher for a specific iCallback. Patchers run -- in
// registration order -- on every matching callback before it
// is dispatched to the game's CCallback.
//
// The registry replaces the previous hard-coded auth callback
// patching. Per-game auth behavior now lives in a plugin.
// ============================================================
struct CallbackPatcherEntry
{
    int                    iCallback;
    KRYOTO_CallbackPatcherFn  fn;
};
static std::vector<CallbackPatcherEntry> g_CallbackPatchers;
static SRWLOCK g_CallbackPatcherLock = SRWLOCK_INIT;

void KRYOTO_RegisterCallbackPatcher(int iCallback, KRYOTO_CallbackPatcherFn fn)
{
    if (!fn) return;
    AcquireSRWLockExclusive(&g_CallbackPatcherLock);
    g_CallbackPatchers.push_back({ iCallback, fn });
    ReleaseSRWLockExclusive(&g_CallbackPatcherLock);
    KRYOTOLOG("[KryotoOnline] Callback patcher registered for iCallback=%d", iCallback);
}

static void RunCallbackPatchers(int iCallback, uint8* pBuf, uint32 cbBuf)
{
    if (!pBuf || cbBuf == 0) return;
    AcquireSRWLockShared(&g_CallbackPatcherLock);
    // Iterate by index in case a patcher misbehaves and re-enters.
    size_t n = g_CallbackPatchers.size();
    for (size_t i = 0; i < n; i++)
    {
        const auto& e = g_CallbackPatchers[i];
        if (e.iCallback == iCallback)
            e.fn(pBuf, cbBuf);
    }
    ReleaseSRWLockShared(&g_CallbackPatcherLock);
}

// ============================================================
// What changed in 1.10, because every item was a multiplayer bug:
//
//   * ONE message per RunCallbacks. The pipe was read once a frame,
//     so on joining a lobby - where Steam posts a PersonaStateChange
//     per member, a LobbyDataUpdate per key and a LobbyChatUpdate per
//     join - the message the game was waiting for sat behind dozens
//     of others, a frame each. Timeouts, "failed to join", hosts that
//     never saw a client arrive. Valve's dispatcher drains the pipe;
//     this one now does too.
//
//   * ONE listener per callback. Dispatch stopped at the first
//     registered CCallback for an id. Games register several - the
//     lobby UI, the network layer and the voice system all listen
//     for LobbyChatUpdate_t - and all but one went deaf.
//
//   * Pipe-wide callbacks dropped. A message was delivered only if
//     its m_hSteamUser was the client user. ISteamUtils' callbacks
//     (SteamShutdown_t, GamepadTextInputDismissed_t, ...) are posted
//     for the pipe with user 0 and never arrived.
//
//   * Unregistering never happened. Every SteamAPI_Unregister* was
//     gated on a flag nothing ever set, so a destroyed CCallback (a
//     scene change, leaving a lobby menu) stayed in the map and the
//     next matching message called into freed memory.
//
//   * Call results were never retired. The entry stayed in the map
//     after firing, its buffer leaked, and the next completion walked
//     a list of dangling pointers. A failed GetAPICallResult deleted
//     the buffer and told the game nothing, so a join that failed at
//     the Steam end waited forever. And if a handler registered a new
//     callback, every LobbyEnter_t listener in the game was erased
//     and the call result was run a second time.
// ============================================================

CCallbackDispatcher::CCallbackDispatcher()
{
	KryotoColor(FOREGROUND_BLUE | FOREGROUND_INTENSITY, "[KryotoOnline] CCallbackDispatcher constructed\r\n");

	m_pfnBGetCallback = nullptr;
	m_pfnFreeLastCallback = nullptr;
	m_pfnGetAPICallResult = nullptr;
	m_CurrentUser = 0;
	m_ManualCbId = 0;
	m_ManualCbSize = 0;
	m_bProcessing = false;
	InitializeSRWLock(&m_MapLock);

	// The flag every Unregister and CountRegisteredCallbacks checks.
	// Nothing used to set it.
	s_bDispatcherInited = true;
}

CCallbackDispatcher::~CCallbackDispatcher()
{
	s_bDispatcherInited = false;
	m_pfnBGetCallback = nullptr;
	m_pfnFreeLastCallback = nullptr;
	m_pfnGetAPICallResult = nullptr;
}

void CCallbackDispatcher::Shutdown()
{
	KryotoColor(FOREGROUND_BLUE | FOREGROUND_INTENSITY, "[KryotoOnline] CCallbackDispatcher shutdown\r\n");

	m_pfnBGetCallback = nullptr;
	m_pfnFreeLastCallback = nullptr;
	m_pfnGetAPICallResult = nullptr;
	m_CurrentUser = 0;
	m_ManualCbId = 0;
	m_ManualCbSize = 0;
	m_bProcessing = false;

	// Pending call results die with the pipe that would have answered
	// them. Registered callbacks do NOT: they belong to game objects
	// that outlive a SteamAPI_Shutdown/SteamAPI_Init cycle, and those
	// objects unregister themselves when they are destroyed.
	AcquireSRWLockExclusive(&m_MapLock);
	for (auto& e : m_CallResultMap)
		e.second->m_nCallbackFlags &= ~CCallbackBase::k_ECallbackFlagsRegistered;
	m_CallResultMap.clear();
	ReleaseSRWLockExclusive(&m_MapLock);
}

bool CCallbackDispatcher::StillRegistered(int iCallback, CCallbackBase* pCb)
{
	bool found = false;
	AcquireSRWLockShared(&m_MapLock);
	auto range = m_CallbackMap.equal_range(iCallback);
	for (auto it = range.first; it != range.second; ++it)
	{
		if (it->second == pCb)
		{
			found = true;
			break;
		}
	}
	ReleaseSRWLockShared(&m_MapLock);
	return found;
}

uint32 CCallbackDispatcher::CountRegistered(int iCallback)
{
	AcquireSRWLockShared(&m_MapLock);
	uint32 n = (uint32)m_CallbackMap.count(iCallback);
	ReleaseSRWLockShared(&m_MapLock);
	return n;
}

void CCallbackDispatcher::RunCallResult(HSteamPipe hPipe, const SteamAPICallCompleted_t& done, bool bServer, bool bSafe)
{
	// Take it out of the map BEFORE running it, as Valve does. The
	// handler is free to Set() the same CCallResult on a new call (a
	// retry), and the result is one-shot either way.
	CCallbackBase* pCb = nullptr;
	AcquireSRWLockExclusive(&m_MapLock);
	auto it = m_CallResultMap.find(done.m_hAsyncCall);
	if (it != m_CallResultMap.end())
	{
		pCb = it->second;
		m_CallResultMap.erase(it);
		pCb->m_nCallbackFlags &= ~CCallbackBase::k_ECallbackFlagsRegistered;
	}
	ReleaseSRWLockExclusive(&m_MapLock);

	if (!pCb)
		return;

	// Size the buffer for whichever is larger: what the client has
	// (m_cubParam) or what the game's struct expects. A game built
	// against an older SDK can expect a few bytes less or more; the
	// client is asked for exactly what it holds, and the game reads
	// its own struct out of a buffer at least that big.
	int cbGame = pCb->GetCallbackSizeBytes();
	size_t cbAlloc = (size_t)(cbGame > (int)done.m_cubParam ? cbGame : (int)done.m_cubParam);
	if (cbAlloc == 0)
		cbAlloc = 1;
	std::vector<uint8> buffer(cbAlloc, 0);

	bool bFailed = false;
	if (pCb->GetICallback() != done.m_iCallback)
	{
		KRYOTOLOG("[KryotoOnline] Call result %llu: game expects %d, client completed %d\r\n",
			(unsigned long long)done.m_hAsyncCall, pCb->GetICallback(), done.m_iCallback);
		bFailed = true;
	}
	else if (!m_pfnGetAPICallResult(hPipe, done.m_hAsyncCall, buffer.data(), (int)done.m_cubParam, done.m_iCallback, &bFailed))
	{
		bFailed = true;
	}

	if (!bFailed && !bServer)
		KryotoLobby::OnCallback(done.m_iCallback, buffer.data(), (int)done.m_cubParam);

	KRYOTOLOG("[KryotoOnline] Call result -> call=%llu cb=%d failed=%d\r\n",
		(unsigned long long)done.m_hAsyncCall, done.m_iCallback, (int)bFailed);

	// Delivered failed or not. A CCallResult that is never run is a
	// game waiting forever on a join or a lobby list.
	if (bSafe)
	{
		try { pCb->Run(buffer.data(), bFailed, done.m_hAsyncCall); }
		catch (...) { KryotoColor(FOREGROUND_RED | FOREGROUND_INTENSITY, "[KryotoOnline] Exception in call result\r\n"); }
	}
	else
	{
		pCb->Run(buffer.data(), bFailed, done.m_hAsyncCall);
	}
}

void CCallbackDispatcher::DispatchOne(HSteamPipe hPipe, const CallbackMsg_t& msg, bool bServer, bool bSafe)
{
	m_CurrentUser = msg.m_hSteamUser;

	if (!bServer)
		KryotoLobby::OnCallback(msg.m_iCallback, msg.m_pubParam, msg.m_cubParam);

	if (msg.m_iCallback == SteamAPICallCompleted_t::k_iCallback && msg.m_cubParam >= (int)sizeof(SteamAPICallCompleted_t))
	{
		SteamAPICallCompleted_t done;
		memcpy(&done, msg.m_pubParam, sizeof(done));
		RunCallResult(hPipe, done, bServer, bSafe);
		return;
	}

	// The overlay's 1x1 placeholder paints; HTML surfaces handle the
	// real ones.
	if (msg.m_iCallback == HTML_NeedsPaint_t::k_iCallback && msg.m_cubParam == (int)sizeof(HTML_NeedsPaint_t))
	{
		const HTML_NeedsPaint_t* pPaint = (const HTML_NeedsPaint_t*)msg.m_pubParam;
		if (pPaint->unWide == 1 || pPaint->unTall == 1)
			return;
	}

	// Snapshot the listeners, then call them without the lock. A
	// handler may register or unregister anything, itself included;
	// each listener is re-checked against the live map just before it
	// runs so one that was removed meanwhile is not called.
	CCallbackBase* local[16];
	std::vector<CCallbackBase*> heap;
	size_t count = 0;

	AcquireSRWLockShared(&m_MapLock);
	auto range = m_CallbackMap.equal_range(msg.m_iCallback);
	for (auto it = range.first; it != range.second; ++it)
	{
		CCallbackBase* pCb = it->second;
		if (!(pCb->m_nCallbackFlags & CCallbackBase::k_ECallbackFlagsRegistered))
			continue;

		// Game-server listeners hear the server pipe, everyone else the
		// client pipe - the same split Valve's dispatcher makes.
		bool bServerListener = (pCb->m_nCallbackFlags & CCallbackBase::k_ECallbackFlagsGameServer) != 0;
		if (bServerListener != bServer)
			continue;

		if (count < _countof(local))
			local[count] = pCb;
		else
		{
			if (heap.empty())
				heap.assign(local, local + count);
			heap.push_back(pCb);
		}
		++count;
	}
	ReleaseSRWLockShared(&m_MapLock);

	if (count == 0)
		return;

	CCallbackBase** listeners = heap.empty() ? local : heap.data();

	if (!bServer)
		RunCallbackPatchers(msg.m_iCallback, msg.m_pubParam, (uint32)msg.m_cubParam);

	for (size_t i = 0; i < count; ++i)
	{
		CCallbackBase* pCb = listeners[i];
		if (i > 0 && !StillRegistered(msg.m_iCallback, pCb))
			continue;

		if (bSafe)
		{
			try { pCb->Run(msg.m_pubParam); }
			catch (...) { KryotoColor(FOREGROUND_RED | FOREGROUND_INTENSITY, "[KryotoOnline] Exception in callback dispatch\r\n"); }
		}
		else
		{
			pCb->Run(msg.m_pubParam);
		}
	}
}

void CCallbackDispatcher::DispatchFrame(HSteamPipe hPipe, bool bServer, bool bSafe)
{
	if (!m_pfnBGetCallback || !m_pfnFreeLastCallback || !m_pfnGetAPICallResult)
		return;

	// Bounded only so a handler that makes Steam post a callback
	// synchronously cannot spin this forever; no real frame gets near it.
	for (int guard = 0; guard < 8192; ++guard)
	{
		CallbackMsg_t msg = { 0 };
		if (!m_pfnBGetCallback(hPipe, &msg))
			break;

		DispatchOne(hPipe, msg, bServer, bSafe);
		m_pfnFreeLastCallback(hPipe);

		// SteamAPI_Shutdown from inside a handler pulls the pipe out
		// from under the loop.
		if (!m_pfnBGetCallback || !m_pfnFreeLastCallback)
			break;
	}

	m_CurrentUser = 0;
}

void CCallbackDispatcher::Add(CCallbackBase* pCb, int iCallback)
{
	if (!pCb)
	{
		KryotoColor(FOREGROUND_RED | FOREGROUND_INTENSITY, "[KryotoOnline] Add: null callback ptr\r\n");
		return;
	}

	if (pCb->GetCallbackSizeBytes() == 0)
		return;

	KRYOTOLOG("[KryotoOnline] Register callback -> %d size=%d flags=%d\r\n", iCallback, pCb->GetCallbackSizeBytes(), pCb->m_nCallbackFlags);

	AcquireSRWLockExclusive(&m_MapLock);

	// Registering twice must not mean being called twice.
	for (auto it = m_CallbackMap.begin(); it != m_CallbackMap.end(); )
	{
		if (it->second == pCb)
			it = m_CallbackMap.erase(it);
		else
			++it;
	}

	pCb->m_nCallbackFlags |= CCallbackBase::k_ECallbackFlagsRegistered;
	pCb->m_iCallback = iCallback;
	m_CallbackMap.insert(std::make_pair(iCallback, pCb));

	ReleaseSRWLockExclusive(&m_MapLock);
}

void CCallbackDispatcher::AddCallResult(CCallbackBase* pCb, SteamAPICall_t hCall)
{
	if (!pCb || hCall == k_uAPICallInvalid)
	{
		KryotoColor(FOREGROUND_RED | FOREGROUND_INTENSITY, "[KryotoOnline] AddCallResult: invalid args\r\n");
		return;
	}

	if (pCb->GetICallback() == 0 || pCb->GetCallbackSizeBytes() == 0)
	{
		KryotoColor(FOREGROUND_RED | FOREGROUND_INTENSITY, "[KryotoOnline] AddCallResult: zero callback\r\n");
		return;
	}

	KRYOTOLOG("[KryotoOnline] Register call result -> %llu cb=%d size=%d\r\n",
		(unsigned long long)hCall, pCb->GetICallback(), pCb->GetCallbackSizeBytes());

	AcquireSRWLockExclusive(&m_MapLock);
	pCb->m_nCallbackFlags |= CCallbackBase::k_ECallbackFlagsRegistered;
	m_CallResultMap[hCall] = pCb;
	ReleaseSRWLockExclusive(&m_MapLock);
}

void CCallbackDispatcher::Remove(CCallbackBase* pCb)
{
	if (!pCb)
		return;

	AcquireSRWLockExclusive(&m_MapLock);
	pCb->m_nCallbackFlags &= ~CCallbackBase::k_ECallbackFlagsRegistered;
	for (auto it = m_CallbackMap.begin(); it != m_CallbackMap.end(); )
	{
		if (it->second == pCb)
			it = m_CallbackMap.erase(it);
		else
			++it;
	}
	ReleaseSRWLockExclusive(&m_MapLock);

	KRYOTOLOG("[KryotoOnline] Unregister callback -> %d\r\n", pCb->GetICallback());
}

void CCallbackDispatcher::RemoveCallResult(CCallbackBase* pCb, SteamAPICall_t hCall)
{
	if (!pCb)
		return;

	AcquireSRWLockExclusive(&m_MapLock);
	pCb->m_nCallbackFlags &= ~CCallbackBase::k_ECallbackFlagsRegistered;

	auto it = m_CallResultMap.find(hCall);
	if (it != m_CallResultMap.end() && it->second == pCb)
	{
		m_CallResultMap.erase(it);
	}
	else
	{
		// Cancel() with a handle we do not have under that object: drop
		// the object wherever it is, so nothing can call into it later.
		for (auto jt = m_CallResultMap.begin(); jt != m_CallResultMap.end(); )
		{
			if (jt->second == pCb)
				jt = m_CallResultMap.erase(jt);
			else
				++jt;
		}
	}
	ReleaseSRWLockExclusive(&m_MapLock);

	KRYOTOLOG("[KryotoOnline] Unregister call result -> %llu\r\n", (unsigned long long)hCall);
}

CCallbackDispatcher* GetDispatcher()
{
	static CCallbackDispatcher instance;
	return &instance;
}

// ============================================================
// CDumpHandler (_DEBUG only)
// ============================================================

#ifdef _DEBUG

#include <DbgHelp.h>

CDumpHandler::CDumpHandler()
{
	m_Comment.clear();
	m_hDbgHelp = nullptr;
	m_pfnWriteDump = nullptr;
	m_bReady = false;

	m_hDbgHelp = LoadLibraryExW(L"DbgHelp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
	if (!m_hDbgHelp)
		return;

	FARPROC fp = GetProcAddress(m_hDbgHelp, "MiniDumpWriteDump");
	if (!fp)
	{
		FreeLibrary(m_hDbgHelp);
		m_hDbgHelp = nullptr;
		return;
	}

	m_pfnWriteDump = (Fn_MiniDumpWriteDump)fp;

	InitializeSRWLock(&m_Lock);
	m_bReady = true;
}

CDumpHandler::~CDumpHandler()
{
	m_pfnWriteDump = nullptr;
	m_Comment.clear();

	if (m_hDbgHelp)
	{
		FreeLibrary(m_hDbgHelp);
		m_hDbgHelp = nullptr;
	}

	m_bReady = false;
}

bool CDumpHandler::IsReady()
{
	return m_bReady;
}

void CDumpHandler::SetComment(const wchar_t* comment)
{
	m_Comment.assign(comment);
}

size_t CDumpHandler::GetCommentByteSize()
{
	if (m_Comment.empty()) return 0;
	return m_Comment.length() * sizeof(wchar_t);
}

const wchar_t* CDumpHandler::GetComment()
{
	return m_Comment.c_str();
}

void CDumpHandler::ClearComment()
{
	m_Comment.clear();
}

void CDumpHandler::WriteDump(DWORD exceptionCode, _EXCEPTION_POINTERS* pExceptionInfo)
{
	if (!m_hDbgHelp || !m_pfnWriteDump)
		return;

	HANDLE hProcess = GetCurrentProcess();
	DWORD pid = GetCurrentProcessId();

	AcquireSRWLockExclusive(&m_Lock);

	MINIDUMP_EXCEPTION_INFORMATION excInfo = { 0 };
	excInfo.ThreadId = GetCurrentThreadId();
	excInfo.ExceptionPointers = pExceptionInfo;
	excInfo.ClientPointers = FALSE;

	MINIDUMP_TYPE dumpType = (MINIDUMP_TYPE)(MiniDumpNormal | MiniDumpWithHandleData | MiniDumpWithUnloadedModules |
		MiniDumpWithProcessThreadData | MiniDumpWithFullMemoryInfo | MiniDumpWithThreadInfo);

	if (GetCommentByteSize() != 0)
	{
		MINIDUMP_USER_STREAM streams[2] = { 0 };

		streams[0].Type = CommentStreamW;
		streams[0].BufferSize = (ULONG)GetCommentByteSize();
		streams[0].Buffer = (LPVOID)GetComment();

		MINIDUMP_EXCEPTION_STREAM excStream = { 0 };
		excStream.ExceptionRecord.ExceptionCode = exceptionCode;

		streams[1].Type = ExceptionStream;
		streams[1].BufferSize = sizeof(MINIDUMP_EXCEPTION_STREAM);
		streams[1].Buffer = &excStream;

		MINIDUMP_USER_STREAM_INFORMATION streamInfo = { 0 };
		streamInfo.UserStreamCount = 2;
		streamInfo.UserStreamArray = streams;

		m_pfnWriteDump(hProcess, pid, nullptr, dumpType, &excInfo, &streamInfo, nullptr);
	}
	else
	{
		MINIDUMP_USER_STREAM streams[1] = { 0 };

		MINIDUMP_EXCEPTION_STREAM excStream = { 0 };
		excStream.ExceptionRecord.ExceptionCode = exceptionCode;

		streams[0].Type = ExceptionStream;
		streams[0].BufferSize = sizeof(MINIDUMP_EXCEPTION_STREAM);
		streams[0].Buffer = &excStream;

		MINIDUMP_USER_STREAM_INFORMATION streamInfo = { 0 };
		streamInfo.UserStreamCount = 1;
		streamInfo.UserStreamArray = streams;

		m_pfnWriteDump(hProcess, pid, nullptr, dumpType, &excInfo, &streamInfo, nullptr);
	}

	ReleaseSRWLockExclusive(&m_Lock);
}
#endif
