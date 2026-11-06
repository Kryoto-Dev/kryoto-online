// Steam's callback and call-result plumbing.
//
// Every asynchronous answer a multiplayer game waits on arrives through
// here: matchmaking results, lobby create/join, the server list. The
// game registers a CCallback, the client pipe produces a CallbackMsg_t,
// and this maps one onto the other. Nothing that waits on Steam works
// without it.
//
// Plugins can register a patcher against an iCallback to mutate a
// message before the game sees it - see include/kryoto_plugin.h.

#pragma once

#include <map>

typedef bool (S_CALLTYPE* Fn_BGetCallback)(HSteamPipe hPipe, CallbackMsg_t* pMsg);
typedef void (S_CALLTYPE* Fn_FreeLastCallback)(HSteamPipe hPipe);
typedef bool (S_CALLTYPE* Fn_GetAPICallResult)(HSteamPipe hPipe, SteamAPICall_t hCall, void* pBuf, int cubBuf, int iExpected, bool* pbFailed);

class CCallbackDispatcher
{
public:
	Fn_BGetCallback m_pfnBGetCallback;
	Fn_FreeLastCallback m_pfnFreeLastCallback;
	Fn_GetAPICallResult m_pfnGetAPICallResult;
	HSteamUser m_CurrentUser;
	int m_ManualCbId;
	int m_ManualCbSize;
	bool m_bProcessing;

	// Every CCallback the game registered, by iCallback. A multimap
	// because one callback id routinely has several listeners, and
	// every one of them is owed the message.
	std::multimap<int, CCallbackBase*> m_CallbackMap;

	// Pending CCallResults, by API call handle. One-shot: an entry is
	// removed the moment its result is delivered.
	std::map<SteamAPICall_t, CCallbackBase*> m_CallResultMap;

	CCallbackDispatcher();
	~CCallbackDispatcher();

	void Shutdown();

	// Drain the pipe: every message waiting on it is delivered before
	// this returns, as Valve's SteamAPI_RunCallbacks does.
	void DispatchFrame(HSteamPipe hPipe, bool bServer, bool bSafe = false);
	void DispatchFrameSafe(HSteamPipe hPipe, bool bServer) { DispatchFrame(hPipe, bServer, true); }

	void Add(CCallbackBase* pCb, int iCallback);
	void AddCallResult(CCallbackBase* pCb, SteamAPICall_t hCall);
	void Remove(CCallbackBase* pCb);
	void RemoveCallResult(CCallbackBase* pCb, SteamAPICall_t hCall);
	uint32 CountRegistered(int iCallback);

private:
	// Guards both maps. Held only while reading or changing them,
	// never across a call into the game: a handler that registers or
	// unregisters (which most lobby code does) would deadlock on it.
	SRWLOCK m_MapLock;

	void DispatchOne(HSteamPipe hPipe, const CallbackMsg_t& msg, bool bServer, bool bSafe);
	void RunCallResult(HSteamPipe hPipe, const SteamAPICallCompleted_t& done, bool bServer, bool bSafe);
	bool StillRegistered(int iCallback, CCallbackBase* pCb);
};

CCallbackDispatcher* GetDispatcher();
