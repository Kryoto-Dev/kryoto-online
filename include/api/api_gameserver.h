S_API ESteamAPIInitResult S_CALLTYPE SteamInternal_GameServer_Init_V2(uint32 unIP, uint16 usGamePort, uint16 usQueryPort, EServerMode eMode, const char* pszVersion, const char* pszVersions, SteamErrMsg* pErr)
{
	KRYOTOLOG("[KryotoOnline] SteamInternal_GameServer_Init_V2\r\n");

	if (g_ServerClient)
	{
		KryotoColor(FOREGROUND_RED | FOREGROUND_INTENSITY, "[KryotoOnline] GameServer_Init already called, use Shutdown first\r\n");
		return k_ESteamAPIInitResult_OK;
	}

	g_ServerMode = eMode;

	g_ServerClient = static_cast<ISteamClient*>(InitSteamClient(&g_ServerModule, true, STEAMCLIENT_INTERFACE_VERSION));

	if (!g_ServerClient)
	{
		KryotoColor(FOREGROUND_RED | FOREGROUND_INTENSITY, "[KryotoOnline] GameServer_Init: failed to get SteamClient\r\n");
		SteamGameServer_Shutdown();
		return k_ESteamAPIInitResult_FailedGeneric;
	}

	g_pServerClient = g_ServerClient;

	SteamIPAddress_t bindAddr;
	bindAddr.m_eType = k_ESteamIPTypeIPv4;
	bindAddr.m_unIPv4 = unIP;

	g_ServerClient->SetLocalIPBinding(bindAddr, 0);
	g_ServerUser = g_ServerClient->CreateLocalUser(&g_ServerPipe, k_EAccountTypeGameServer);

	if (g_ServerUser != 0 && g_ServerPipe != 0)
	{
		ISteamGameServer* pGS = (ISteamGameServer*)g_ServerClient->GetISteamGameServer(g_ServerUser, g_ServerPipe, STEAMGAMESERVER_INTERFACE_VERSION);

		if (pGS)
		{
			ISteamUtils* pUtils = (ISteamUtils*)g_ServerClient->GetISteamUtils(g_ServerPipe, STEAMUTILS_INTERFACE_VERSION);

			if (pUtils)
			{
				uint32 flags = k_unServerFlagNone;
				if (eMode == eServerModeAuthenticationAndSecure) flags |= k_unServerFlagSecure;
				if (eMode == eServerModeNoAuthentication) flags |= k_unServerFlagPrivate;

				// The spoofed AppId, never what GetAppID says. Once the
				// client is up, GetAppID is hooked to report ogAppId, and
				// a server registered under ogAppId is one no client on
				// the spoofed id can find, join or authenticate against.
				// The client side has always been the forced id; the
				// server has to be the same one.
				uint32 appId = g_ForcedAppId;
				uint32 reportedId = pUtils->GetAppID();
				if (reportedId != appId)
					KRYOTOLOG("[KryotoOnline] GameServer_Init: registering as %u (utils reports %u)\r\n", appId, reportedId);

				if (appId != 0)
				{
					if (pGS->InitGameServer(unIP, usGamePort, usQueryPort, flags, appId, pszVersion))
					{
						Steam_RegisterInterfaceFuncs(g_ServerModule);
						SteamAPI_SetBreakpadAppID(appId);
						LoadBreakpadSymbols(g_ServerModule);

						g_bServerReady = g_ServerCtx.Init();

						if (!g_bServerReady)
						{
							KryotoColor(FOREGROUND_RED | FOREGROUND_INTENSITY, "[KryotoOnline] GameServer_Init: g_ServerCtx.Init failed\r\n");
							SteamGameServer_Shutdown();
							return k_ESteamAPIInitResult_VersionMismatch;
						}

						g_pGameServer = g_ServerCtx.SteamGameServer();
						g_pServerUtils = g_ServerCtx.SteamGameServerUtils();
						g_pSteamClientGameServer = g_ServerClient;
						g_pSteamClientGameServer_Latest = g_ServerClient;

						// The server pipe hands out its own ISteamGameServer,
						// whose UserHasLicenseForApp is what a dedicated or
						// listen server asks about each joining player.
						s_Core.InstallHooks(&ResolveServerInterface, true);

						return k_ESteamAPIInitResult_OK;
					}
					else
					{
						KryotoColor(FOREGROUND_RED | FOREGROUND_INTENSITY, "[KryotoOnline] GameServer_Init: InitGameServer failed\r\n");
						SteamGameServer_Shutdown();
						return k_ESteamAPIInitResult_FailedGeneric;
					}
				}
				else
				{
					KryotoColor(FOREGROUND_RED | FOREGROUND_INTENSITY, "[KryotoOnline] GameServer_Init: AppID is 0\r\n");
					SteamGameServer_Shutdown();
					return k_ESteamAPIInitResult_FailedGeneric;
				}
			}
			else
			{
				KryotoColor(FOREGROUND_RED | FOREGROUND_INTENSITY, "[KryotoOnline] GameServer_Init: failed to get ISteamUtils\r\n");
				WarnMissingInterface(g_ServerPipe, STEAMUTILS_INTERFACE_VERSION);
				SteamGameServer_Shutdown();
				return k_ESteamAPIInitResult_VersionMismatch;
			}
		}
		else
		{
			KryotoColor(FOREGROUND_RED | FOREGROUND_INTENSITY, "[KryotoOnline] GameServer_Init: failed to get ISteamGameServer\r\n");
			WarnMissingInterface(g_ServerPipe, STEAMGAMESERVER_INTERFACE_VERSION);
			SteamGameServer_Shutdown();
			return k_ESteamAPIInitResult_VersionMismatch;
		}
	}

	KryotoColor(FOREGROUND_RED | FOREGROUND_INTENSITY, "[KryotoOnline] GameServer_Init failed\r\n");
	SteamGameServer_Shutdown();
	return k_ESteamAPIInitResult_FailedGeneric;
}

S_API bool S_CALLTYPE SteamInternal_GameServer_Init(uint32 unIP, uint16 usPort, uint16 usGamePort, uint16 usQueryPort, EServerMode eMode, const char* pszVersion)
{
	(void)usPort;
	return SteamInternal_GameServer_Init_V2(unIP, usGamePort, usQueryPort, eMode, pszVersion, nullptr, nullptr) == k_ESteamAPIInitResult_OK;
}

S_API bool S_CALLTYPE SteamGameServer_InitSafe(uint32 unIP, uint16 usPort, uint16 usGamePort, uint16 usQueryPort, EServerMode eMode, const char* pszVersion)
{
	KRYOTOLOG("[KryotoOnline] SteamGameServer_InitSafe\r\n");

	if (SteamInternal_GameServer_Init_V2(unIP, usGamePort, usQueryPort, eMode, pszVersion, nullptr, nullptr) == k_ESteamAPIInitResult_OK && g_pServerClient)
		return true;

	KryotoColor(FOREGROUND_RED | FOREGROUND_INTENSITY, "[KryotoOnline] SteamGameServer_InitSafe failed\r\n");
	return false;
}

S_API bool S_CALLTYPE SteamGameServer_BSecure()
{
	KRYOTOLOG("[KryotoOnline] SteamGameServer_BSecure\r\n");

	if (g_ServerMode == eServerModeNoAuthentication) return false;
	if (!g_pGameServer) return false;

	return g_pGameServer->BSecure();
}

S_API HSteamPipe S_CALLTYPE SteamGameServer_GetHSteamPipe()
{
	// Hot path (per-frame callback dispatch) -- not logged to avoid flooding the log.
	return g_ServerPipe;
}

S_API HSteamUser S_CALLTYPE SteamGameServer_GetHSteamUser()
{
	KRYOTOLOG("[KryotoOnline] SteamGameServer_GetHSteamUser\r\n");
	return g_ServerUser;
}

S_API uint32 S_CALLTYPE SteamGameServer_GetIPCCallCount()
{
	KRYOTOLOG("[KryotoOnline] SteamGameServer_GetIPCCallCount\r\n");
	return g_pServerUtils ? g_pServerUtils->GetIPCCallCount() : 0;
}

S_API uint64 S_CALLTYPE SteamGameServer_GetSteamID()
{
	KRYOTOLOG("[KryotoOnline] SteamGameServer_GetSteamID\r\n");

	if (g_ServerMode == eServerModeNoAuthentication)
		return *(uint64*)&k_steamIDLanModeGS;

	if (!g_pGameServer)
		return *(uint64*)&k_steamIDNotInitYetGS;

	return g_pGameServer->GetSteamID().ConvertToUint64();
}

S_API void S_CALLTYPE SteamGameServer_RunCallbacks()
{
	KRYOTOLOG("[KryotoOnline] SteamGameServer_RunCallbacks\r\n");

	if (g_DispatchMode == 1) return;
	g_DispatchMode = 2;

	if (g_ServerPipe != 0)
		Steam_RunCallbacks(g_ServerPipe, true);
}
