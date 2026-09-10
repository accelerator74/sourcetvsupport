#include "extension.h"
#include "wrappers.h"

#include "sdk/public/engine/inetsupport.h"
#include "sdk/engine/networkstringtable.h"

#include <vector>
#include <am-string.h>
#include <checksum_crc.h>
#include <extensions/IBinTools.h>
#include <extensions/ISDKTools.h>

// Interfaces
INetworkStringTableContainer* networkStringTableContainerServer = NULL;
IHLTVDirector* hltvdirector = NULL;
IHLTVServer* g_pHLTVServer = NULL;
INetSupport* g_pNetSupport = NULL;
IPlayerInfoManager* playerinfomanager = NULL;
IServerGameEnts* gameents = NULL;
IBinTools* bintools = NULL;
ISDKTools* sdktools = NULL;

CGlobalVars* gpGlobals = NULL;

IServer* g_pGameIServer = NULL;

int CBasePlayer::sendprop_m_fFlags = 0;
int CBaseServer::offset_stringTableCRC = 0;
int CBaseServer::vtblindex_GetChallengeNr = 0;
int CBaseServer::vtblindex_GetChallengeType = 0;
int CBaseServer::vtblindex_ReplyChallenge = 0;
int CBaseServer::vtblindex_FillServerInfo = 0;
int CBaseServer::vtblindex_ConnectClient = 0;
void* CBaseServer::pfn_IsExclusiveToLobbyConnections = NULL;
KHook::Function<bool, void*>* CBaseServer::detour_IsExclusiveToLobbyConnections = NULL;
ICallWrapper* CBaseServer::vcall_GetChallengeNr = NULL;
ICallWrapper* CBaseServer::vcall_GetChallengeType = NULL;
int CHLTVServer::offset_m_DemoRecorder = 0;
int CHLTVServer::offset_CClientFrameManager = 0;
int CHLTVServer::offset_CBaseServer = 0;
int CHLTVServer::vtblindex_FillServerInfo = 0;
void* CHLTVServer::pfn_AddNewFrame = NULL;

KHook::Function<CClientFrame*, void*, CClientFrame*>* CHLTVServer::detour_AddNewFrame = NULL;
KHook::Virtual<CBaseServer, void, netadr_s&, bf_read&>* CHLTVServer::hook_ReplyChallenge = NULL;
KHook::Virtual<CBaseServer, void, SVC_ServerInfo&>* CHLTVServer::hook_FillServerInfo = NULL;
KHook::Virtual<CHLTVServer, void, SVC_ServerInfo&>* CHLTVServer::hook_hltv_FillServerInfo = NULL;
KHook::Virtual<CBaseServer, IClient*, netadr_t&, int, int, int, const char*,
	const char*, const char*, int, CUtlVector<NetMessageCvar_t>&, bool>* CHLTVServer::hook_ConnectClient = NULL;

KHook::Virtual<IServer, bool>* CGameServer::hook_IsPausable = NULL;

int CBaseClient::offset_m_SteamID = 0;
void* CBaseClient::pfn_SendFullConnectEvent = NULL;
KHook::Function<void, void*>* CBaseClient::detour_SendFullConnectEvent = NULL;

void* CSteam3Server::pfn_NotifyClientDisconnect = NULL;
KHook::Function<void, void*, CBaseClient*>* CSteam3Server::detour_NotifyClientDisconnect = NULL;

int CFrameSnapshotManager::offset_m_PackedEntitiesPool = 0;
void* CFrameSnapshotManager::pfn_LevelChanged = NULL;
KHook::Function<void, void*>* CFrameSnapshotManager::detour_LevelChanged = NULL;

void* CBaseAbility::pfn_ShouldTransmit = NULL;
KHook::Function<int, void*, const CCheckTransmitInfo*>* CBaseAbility::detour_ShouldTransmit = NULL;

void* HitAnnouncement::pfn_ForEachTerrorPlayer = NULL;
KHook::Function<bool, HitAnnouncement&>* HitAnnouncement::detour_ForEachTerrorPlayer = NULL;
int HitAnnouncement::pzMsgId = 0;

void* pfn_DataTable_WriteSendTablesBuffer = NULL;
void* pfn_SteamGameServer_GetHSteamPipe = NULL;
void* pfn_SteamGameServer_GetHSteamUser = NULL;
void* pfn_SteamInternal_CreateInterface = NULL;
void* pfn_SteamInternal_GameServer_Init = NULL;
void* pfn_OpenSocketInternal = NULL;

KHook::Function<bool, uint32, uint16, uint16, uint16, EServerMode, const char*>* detour_SteamInternal_GameServer_Init = NULL;

KHook::Virtual<IHLTVDirector, void, IHLTVServer*>* g_HookSetHLTVServer = NULL;
KHook::Virtual<CHLTVDemoRecorder, void>* g_HookRecordStringTables = NULL;
KHook::Virtual<CHLTVDemoRecorder, void, ServerClass*>* g_HookRecordServerClasses = NULL;
KHook::Virtual<ISteamGameServer, void>* g_HookSteamGameServer_LogOff = NULL;
KHook::Virtual<IServerGameEnts, void, CCheckTransmitInfo*, const unsigned short*, int>* g_HookCheckTransmit = NULL;

// Need this for demofile.h to not link tier2
bool CUtlStreamBuffer::IsOpen() const
{
	return m_hFileHandle != NULL;
}

SMExtension g_Extension;
SMEXT_LINK(&g_Extension);

void TrySendPZMsgToSourceTV(const HitAnnouncement& rMsg)
{
	if (g_pHLTVServer == NULL)
		return;

	int iSourceTVIndex = g_pHLTVServer->GetHLTVSlot() + 1;
	CBasePlayer* pSourceTV = UTIL_PlayerByIndex(iSourceTVIndex);
	if (pSourceTV == NULL || !pSourceTV->IsHLTV())
		return;

	if (rMsg.m_pAttacker == NULL || rMsg.m_pVictim == NULL)
		return;

	bf_write* pBf = usermsgs->StartBitBufMessage(HitAnnouncement::pzMsgId, &iSourceTVIndex, 1, USERMSG_RELIABLE);
	if (pBf != NULL) {
		pBf->WriteByte(rMsg.m_iEventType);
		pBf->WriteShort(rMsg.m_pAttacker->GetUserID());
		pBf->WriteShort(rMsg.m_pVictim->GetUserID());
		pBf->WriteShort((rMsg.m_pInflictor) ? rMsg.m_pInflictor->GetUserID() : 0);
		pBf->WriteShort(rMsg.m_iDamageAmount);
		usermsgs->EndMessage();

		/*Msg("Send usermessage \"PZDmgMsg\" to SourceTV. Msg id: %d, attacker: %d, victim: %d, inflictor: %d, damage: %d, team check: %d""\n", \
					rMsg.m_iEventType, 
					rMsg.m_pAttacker->GetUserID(), 
					rMsg.m_pVictim->GetUserID(), 
					(rMsg.m_pInflictor) ? rMsg.m_pInflictor->GetUserID() : 0, 
					rMsg.m_iDamageAmount, 
					rMsg.m_bIgnoreTeamCheck);*/
	}
}

// A1m`: Visual bug. usermessage "PZDmgMsg" is not sent to the SourceTV client
KHook::Return<bool> Handler_ForEachTerrorPlayer__HitAnnouncement(HitAnnouncement& rMsg)
{
	bool bRetVal = HitAnnouncement::detour_ForEachTerrorPlayer->CallOriginal(rMsg);
	TrySendPZMsgToSourceTV(rMsg);
	return { KHook::Action::Supersede, bRetVal };
}

// A1m`: Visual bug. infected players abilities are not sent, so the cooldown of abilities in versus-like modes is not visible in the HUD below. 
// This is also relevant for spectators.
KHook::Return<int> Handler_CBaseAbility__ShouldTransmit(void* pThis, const CCheckTransmitInfo* pInfo)
{
	int iRetVal = CBaseAbility::detour_ShouldTransmit->CallOriginal(pThis, pInfo);

	if (iRetVal == FL_EDICT_ALWAYS)
		return { KHook::Action::Supersede, iRetVal };

	IGamePlayer* pPlayer = playerhelpers->GetGamePlayer(pInfo->m_pClientEnt);
	if (pPlayer == NULL)
		return { KHook::Action::Supersede, iRetVal };

	IPlayerInfo* pPInfo = pPlayer->GetPlayerInfo();
	if (pPInfo == NULL)
		return { KHook::Action::Supersede, iRetVal };

	if (pPlayer->IsSourceTV() || pPInfo->GetTeamIndex() == TEAM_SPECTATOR)
		return { KHook::Action::Supersede, FL_EDICT_ALWAYS };

	return { KHook::Action::Supersede, iRetVal };
}

// bug#X: hltv clients are sending "player_full_connect" event
// user ids of hltv clients can collide with user ids of sv
// event "player_full_connect" fires with userid of hltv client
KHook::Return<void> Handler_CBaseClient_SendFullConnectEvent(void* pThis)
{
	CBaseClient* _this = reinterpret_cast<CBaseClient*>(pThis);
	IServer* pServer = _this->GetServer();
	if (pServer != NULL && pServer->IsHLTV())
		return { KHook::Action::Supersede };

	return { KHook::Action::Ignore };
}

KHook::Return<bool> Handler_CBaseServer_IsExclusiveToLobbyConnections(void* pThis)
{
	CBaseServer* _this = reinterpret_cast<CBaseServer*>(pThis);
	if (_this->IsHLTV())
		return { KHook::Action::Supersede, false };
	return { KHook::Action::Ignore };
}

KHook::Return<CClientFrame*> Handler_CHLTVServer_AddNewFrame(void* pThis, CClientFrame* clientFrame)
{
	CHLTVServer* _this = reinterpret_cast<CHLTVServer*>(pThis);

	// bug##: hibernation causes to leak memory when adding new frames to hltv
	// forcefully remove oldest frames
	CClientFrame* pFrame = CHLTVServer::detour_AddNewFrame->CallOriginal(pThis, clientFrame);

	// Only keep the number of packets required to satisfy tv_delay at our tv snapshot rate
	static ConVarRef tv_delay("tv_delay"), tv_snapshotrate("tv_snapshotrate");
	int numFramesToKeep = 2 * ((1 + MAX(1.0f, tv_delay.GetFloat())) * tv_snapshotrate.GetInt());
	if (numFramesToKeep < MAX_CLIENT_FRAMES)
		numFramesToKeep = MAX_CLIENT_FRAMES;

	CClientFrameManager& frameManager = _this->GetClientFrameManager();
	int nClientFrameCount = frameManager.CountClientFrames();
	while (nClientFrameCount > numFramesToKeep) {
		frameManager.RemoveOldestFrame();
		--nClientFrameCount;
	}

	return { KHook::Action::Supersede, pFrame };
}

// bug#8: ticket auth (authprotocol = 2) with hltv clients crashes server in steamclient.so on disconnect
// malformed steamid of unauthentificated hltv client passed to CSteamGameServer012::EndAuthSession
KHook::Return<void> Handler_CSteam3Server_NotifyClientDisconnect(void* pThis, CBaseClient* client)
{
	if (!client->IsConnected() || client->IsFakeClient())
		return { KHook::Action::Supersede };
	if (!client->m_SteamID().IsValid())
		return { KHook::Action::Supersede };

	return { KHook::Action::Ignore };
}

#if SOURCE_ENGINE == SE_LEFT4DEAD2
KHook::Return<bool> Handler_SteamInternal_GameServer_Init(uint32 unIP, uint16 usPort, uint16 usGamePort, uint16 usQueryPort, EServerMode eServerMode, const char* pchVersionString)
{
	// bug##: without overriding usQueryPort parm, it uses HLTV port (if having -hltv in launch parameters), which is already bound
	// failing SteamInternal_GameServer_Init also causes game to freeze
	static ConVarRef sv_master_share_game_socket("sv_master_share_game_socket");
	usQueryPort = sv_master_share_game_socket.GetBool() ? MASTERSERVERUPDATERPORT_USEGAMESOCKETSHARE : usPort - 1;

	bool ok = detour_SteamInternal_GameServer_Init->CallOriginal(unIP, usPort, usGamePort, usQueryPort, eServerMode, pchVersionString);
	if (!ok)
		return { KHook::Action::Supersede, false };

	g_Extension.OnGameServer_Init();
	return { KHook::Action::Supersede, true };
}
#endif

KHook::Return<void> Handler_CFrameSnapshotManager_LevelChanged(void* pThis)
{
	CFrameSnapshotManager* _this = reinterpret_cast<CFrameSnapshotManager*>(pThis);

	// bug##: Underlying method CClassMemoryPool::Clear creates CUtlRBTree with insufficient iterator size (unsigned short)
	// memory object of which fails to iterate over more than 65535 of packed entities
	_this->m_PackedEntitiesPool().Clear();

	// CFrameSnapshotManager::m_PackedEntitiesPool shouldn't have elements to free from now on
	CFrameSnapshotManager::detour_LevelChanged->CallOriginal(pThis);
	return { KHook::Action::Supersede };
}

KHook::Return<void> SMExtension::Handler_CHLTVDirector_SetHLTVServer(IHLTVDirector* /*pThis*/, IHLTVServer* pIHLTVServer)
{
	OnSetHLTVServer(pIHLTVServer);
	return { KHook::Action::Ignore };
}

KHook::Return<void> SMExtension::Handler_CHLTVDemoRecorder_RecordStringTables(CHLTVDemoRecorder* pThis)
{
	// bug#2
	// insufficient buffer size in CHLTVDemoRecorder::RecordStringTables, overflowing it with stringtables data (starting at CHLTVDemoRecorder::RecordStringTables)
	// stringtables wont be saved properly, causing demo file to be corrupted
	std::vector<byte> bigBuffer(DEMO_RECORD_BUFFER_SIZE);
	bf_write buf(bigBuffer.data(), bigBuffer.size());

	int numTables = networkStringTableContainerServer->GetNumTables();
	buf.WriteByte(numTables);
	for (int i = 0; i < numTables; i++) {
		INetworkStringTable* table = networkStringTableContainerServer->GetTable(i);
		buf.WriteString(table->GetTableName());

		int numstrings = table->GetNumStrings();
		buf.WriteWord(numstrings);
		for (int j = 0; j < numstrings; j++) {
			buf.WriteString(table->GetString(j));
			int userDataSize;
			const void* pUserData = table->GetStringUserData(j, &userDataSize);
			if (userDataSize > 0) {
				buf.WriteOneBit(1);
				buf.WriteShort(userDataSize);
				buf.WriteBytes(pUserData, userDataSize);
			} else {
				buf.WriteOneBit(0);
			}
		}

		// No client side items on server
		buf.WriteOneBit(0);
	}

	if (buf.IsOverflowed())
		smutils->LogError(myself, "Unable to record string tables");

	pThis->GetDemoFile()->WriteStringTables(&buf, pThis->GetRecordingTick());
	return { KHook::Action::Supersede };
}

KHook::Return<void> SMExtension::Handler_CHLTVDemoRecorder_RecordServerClasses(CHLTVDemoRecorder* pThis, ServerClass* pClasses)
{
	std::vector<byte> bigBuffer(DEMO_RECORD_BUFFER_SIZE);
	bf_write buf(bigBuffer.data(), bigBuffer.size());

	// Send SendTable info.
	InvokeDataTable_WriteSendTablesBuffer(pClasses, &buf);

	// Send class descriptions.
	DataTable_WriteClassInfosBuffer(pClasses, &buf);

	if (buf.IsOverflowed())
		smutils->LogError(myself, "Unable to record server classes");

	pThis->GetDemoFile()->WriteNetworkDataTables(&buf, pThis->GetRecordingTick());
	return { KHook::Action::Supersede };
}

KHook::Return<void> SMExtension::Handler_CHLTVServer_ReplyChallenge(CBaseServer* pThis, netadr_s& adr, bf_read& inmsg)
{
	char buffer[512];
	bf_write msg(buffer, sizeof(buffer));

	char context[256] = { 0 };
	inmsg.ReadString(context, sizeof(context));

	msg.WriteLong(CONNECTIONLESS_HEADER);
	msg.WriteByte(S2C_CHALLENGE);

	int challengeNr = pThis->GetChallengeNr(adr);
	int authprotocol = pThis->GetChallengeType(adr);

	msg.WriteLong(challengeNr);
	msg.WriteLong(authprotocol);

	msg.WriteShort(1);
	msg.WriteLongLong(0LL);
	msg.WriteByte(0);

	msg.WriteString(context);

	// bug#6: CBaseServer::ReplyChallenge called on CHLTVServer instance, on reserved server will force join client to a steam lobby
	// joining steam lobby will force a client to connect to game server, instead of HLTV one
	// replying with empty lobby id and no means for lobby requirement
	msg.WriteLong(g_pNetSupport->GetEngineBuildNumber());
	msg.WriteString("");
	msg.WriteByte(0);

	msg.WriteLongLong(0LL);

	g_pNetSupport->SendPacket(NULL, NS_HLTV, adr, msg.GetData(), msg.GetNumBytesWritten());
	return { KHook::Action::Supersede };
}

// bug##: "Connection to Steam servers lost." upon shutting hltv down
// CHLTVServer::Shutdown is invoking CBaseServer::Shutdown which calling SteamGameServer()->LogOff()
// without any condition on whether it was just hltv shut down
// as long as sv instance is still active - prevent ISteamGameServer::LogOff from being invoked
KHook::Return<void> SMExtension::Handler_ISteamGameServer_LogOff(ISteamGameServer* /*pThis*/)
{
	// Game server still active - intercept ISteamGameServer::LogOff
	if (g_pGameIServer != NULL && g_pGameIServer->IsActive())
		return { KHook::Action::Supersede };
	return { KHook::Action::Ignore };
}

KHook::Return<bool> SMExtension::Handler_CGameServer_IsPausable(const IServer* /*pThis*/)
{
	static ConVarRef sv_pausable("sv_pausable");
	return { KHook::Action::Supersede, sv_pausable.GetBool() };
}

KHook::Return<void> SMExtension::Handler_CHLTVServer_FillServerInfo(CBaseServer* /*pThis*/, SVC_ServerInfo& serverinfo)
{
	serverinfo.m_bIsVanilla = false;
	return { KHook::Action::Ignore };
}

KHook::Return<void> SMExtension::Handler_CHLTVServer_FillServerInfo_HLTV(CHLTVServer* /*pThis*/, SVC_ServerInfo& serverinfo)
{
	serverinfo.m_bIsVanilla = false;
	return { KHook::Action::Ignore };
}

KHook::Return<void> SMExtension::Handler_CServerGameEnts_CheckTransmit(IServerGameEnts* /*pThis*/, CCheckTransmitInfo* pInfo, const unsigned short* pEdictIndices, int nEdicts)
{
	// former SET_META_RESULT(MRES_OVERRIDE) — let original run, then fix up
	// KHook post-callback would be ideal; here we call original then patch
	// For simplicity treat as post-like: Ignore lets original run first when used as post.
	// When used as pre with Override semantics on transmit flags we just fix after via Ignore + side effects.

	IGamePlayer* pRecipientPlayer = playerhelpers->GetGamePlayer(pInfo->m_pClientEnt);
	if (pRecipientPlayer == NULL)
		return { KHook::Action::Ignore };

	if (!pRecipientPlayer->IsSourceTV())
		return { KHook::Action::Ignore };

	int maxClients = playerhelpers->GetMaxClients();
	for (int i = 0; i < nEdicts; i++) {
		int iEdict = pEdictIndices[i];
		if (iEdict > maxClients)
			break;

		// bug##: if hltvdirector follows a bot player and tv_transmitall is set to 0, world entities won't be transmitted
		// reason being PVSInfo_t::m_vCenter never set on bots
		edict_t* pEdict = &gpGlobals->pEdicts[iEdict];
		IServerNetworkable* pNetworkable = pEdict->GetNetworkable();
		if (pNetworkable != NULL) {
			//if (hltvdirector->GetPVSEntity() != iEdict) {
			//	continue;
			//}

			// @see CBasePlayer::ShouldTransmit
			// HACK: force calling RecomputePVSInformation to update PVS data
			pNetworkable->AreaNum();
		}
	}

	return { KHook::Action::Ignore };
}

KHook::Return<IClient*> SMExtension::Handler_CHLTVServer_ConnectClient(CBaseServer* /*pThis*/, netadr_t& adr, int protocol, int challenge, int authProtocol, const char* name,
	const char* password, const char* hashedCDkey, int cdKeyLen, CUtlVector<NetMessageCvar_t>& splitScreenClients, bool isClientLowViolence)
{
	if (splitScreenClients.Count() > 1) {
		char buffer[512];
		bf_write msg(buffer, sizeof(buffer));
		msg.WriteLong(CONNECTIONLESS_HEADER);
		msg.WriteByte(S2C_CONNREJECT);
		msg.WriteString("Splitscreen is not allowed in HLTV\n");
		g_pNetSupport->SendPacket(NULL, NS_HLTV, adr, msg.GetData(), msg.GetNumBytesWritten());
		return { KHook::Action::Supersede, nullptr };
	}
	return { KHook::Action::Ignore, nullptr };
}

void SMExtension::Load()
{
	if ((g_pGameIServer = sdktools->GetIServer()) == NULL) {
		smutils->LogError(myself, "Unable to retrieve sv instance pointer!");
		return;
	}

	SourceMod::PassInfo params[] = {
#if SMINTERFACE_BINTOOLS_VERSION == 4
		{ PassType_Basic, PASSFLAG_BYVAL, sizeof(int), NULL, 0 },
		{ PassType_Basic, PASSFLAG_BYVAL, sizeof(netadr_t*), NULL, 0 },
#else
		// sm1.9- support
		{ PassType_Basic, PASSFLAG_BYVAL, sizeof(int) },
		{ PassType_Basic, PASSFLAG_BYVAL, sizeof(netadr_t*) },
#endif
	};

	CBaseServer::vcall_GetChallengeNr = bintools->CreateVCall(CBaseServer::vtblindex_GetChallengeNr, 0, 0, &params[0], &params[1], 1);
	if (CBaseServer::vcall_GetChallengeNr == NULL) {
		smutils->LogError(myself, "Unable to create virtual call for \"CBaseServer::GetChallengeNr\"!");
		return;
	}

	CBaseServer::vcall_GetChallengeType = bintools->CreateVCall(CBaseServer::vtblindex_GetChallengeType, 0, 0, &params[0], &params[1], 1);
	if (CBaseServer::vcall_GetChallengeType == NULL) {
		smutils->LogError(myself, "Unable to create virtual call for \"CBaseServer::GetChallengeType\"!");
		return;
	}

#if SOURCE_ENGINE == SE_LEFT4DEAD2
	detour_SteamInternal_GameServer_Init = new KHook::Function<bool, uint32, uint16, uint16, uint16, EServerMode, const char*>(
		reinterpret_cast<bool (*)(uint32, uint16, uint16, uint16, EServerMode, const char*)>(pfn_SteamInternal_GameServer_Init),
		&Handler_SteamInternal_GameServer_Init,
		nullptr
	);

	CBaseClient::detour_SendFullConnectEvent = new KHook::Function<void, void*>(
		reinterpret_cast<void (*)(void*)>(CBaseClient::pfn_SendFullConnectEvent),
		&Handler_CBaseClient_SendFullConnectEvent,
		nullptr
	);
#endif

	CBaseServer::detour_IsExclusiveToLobbyConnections = new KHook::Function<bool, void*>(
		reinterpret_cast<bool (*)(void*)>(CBaseServer::pfn_IsExclusiveToLobbyConnections),
		&Handler_CBaseServer_IsExclusiveToLobbyConnections,
		nullptr
	);

	CSteam3Server::detour_NotifyClientDisconnect = new KHook::Function<void, void*, CBaseClient*>(
		reinterpret_cast<void (*)(void*, CBaseClient*)>(CSteam3Server::pfn_NotifyClientDisconnect),
		&Handler_CSteam3Server_NotifyClientDisconnect,
		nullptr
	);

	CHLTVServer::detour_AddNewFrame = new KHook::Function<CClientFrame*, void*, CClientFrame*>(
		reinterpret_cast<CClientFrame* (*)(void*, CClientFrame*)>(CHLTVServer::pfn_AddNewFrame),
		&Handler_CHLTVServer_AddNewFrame,
		nullptr
	);

	CFrameSnapshotManager::detour_LevelChanged = new KHook::Function<void, void*>(
		reinterpret_cast<void (*)(void*)>(CFrameSnapshotManager::pfn_LevelChanged),
		&Handler_CFrameSnapshotManager_LevelChanged,
		nullptr
	);

	CBaseAbility::detour_ShouldTransmit = new KHook::Function<int, void*, const CCheckTransmitInfo*>(
		reinterpret_cast<int (*)(void*, const CCheckTransmitInfo*)>(CBaseAbility::pfn_ShouldTransmit),
		&Handler_CBaseAbility__ShouldTransmit,
		nullptr
	);

	HitAnnouncement::detour_ForEachTerrorPlayer = new KHook::Function<bool, HitAnnouncement&>(
		reinterpret_cast<bool (*)(HitAnnouncement&)>(HitAnnouncement::pfn_ForEachTerrorPlayer),
		&Handler_ForEachTerrorPlayer__HitAnnouncement,
		nullptr
	);

#if SOURCE_ENGINE == SE_LEFT4DEAD
	CGameServer::hook_IsPausable = new KHook::Virtual<IServer, bool>(
		&IServer::IsPausable,
		this,
		&SMExtension::Handler_CGameServer_IsPausable,
		nullptr
	);
	CGameServer::hook_IsPausable->Add(g_pGameIServer);
#endif

	g_HookSetHLTVServer = new KHook::Virtual<IHLTVDirector, void, IHLTVServer*>(
		&IHLTVDirector::SetHLTVServer,
		this,
		nullptr, // pre
		&SMExtension::Handler_CHLTVDirector_SetHLTVServer // post
	);
	g_HookSetHLTVServer->Add(hltvdirector);

	OnSetHLTVServer(hltvdirector->GetHLTVServer());
	OnGameServer_Init();

#ifdef TV_RELAYTEST
	static ConVarRef clientport("clientport");
	InvokeOpenSocketInternal(NS_CLIENT, clientport.GetInt(), PORT_SERVER, "client", true);
#endif

	// Let plugins know when it's safe to use SourceTV features
	sharesys->RegisterLibrary(myself, "sourcetvsupport");
}

void SMExtension::Unload()
{
	delete CBaseAbility::detour_ShouldTransmit;
	CBaseAbility::detour_ShouldTransmit = NULL;

	if (CBaseServer::vcall_GetChallengeNr != NULL) {
		CBaseServer::vcall_GetChallengeNr->Destroy();
		CBaseServer::vcall_GetChallengeNr = NULL;
	}
	if (CBaseServer::vcall_GetChallengeType != NULL) {
		CBaseServer::vcall_GetChallengeType->Destroy();
		CBaseServer::vcall_GetChallengeType = NULL;
	}

	delete CBaseServer::detour_IsExclusiveToLobbyConnections;
	CBaseServer::detour_IsExclusiveToLobbyConnections = NULL;

	delete CHLTVServer::detour_AddNewFrame;
	CHLTVServer::detour_AddNewFrame = NULL;

	delete CBaseClient::detour_SendFullConnectEvent;
	CBaseClient::detour_SendFullConnectEvent = NULL;

	delete detour_SteamInternal_GameServer_Init;
	detour_SteamInternal_GameServer_Init = NULL;

	delete CSteam3Server::detour_NotifyClientDisconnect;
	CSteam3Server::detour_NotifyClientDisconnect = NULL;

	delete CFrameSnapshotManager::detour_LevelChanged;
	CFrameSnapshotManager::detour_LevelChanged = NULL;

	delete HitAnnouncement::detour_ForEachTerrorPlayer;
	HitAnnouncement::detour_ForEachTerrorPlayer = NULL;

	OnGameServer_Shutdown();
	OnSetHLTVServer(NULL);

	if (g_HookSetHLTVServer) {
		g_HookSetHLTVServer->Remove(hltvdirector);
		delete g_HookSetHLTVServer;
		g_HookSetHLTVServer = NULL;
	}

	delete CGameServer::hook_IsPausable;
	CGameServer::hook_IsPausable = NULL;
}

bool SMExtension::SetupFromGameConfig(IGameConfig* gc, char* error, int maxlength)
{
	static const struct {
		const char* key;
		int& offset;
	} s_offsets[] = {
		{ "CBaseServer::stringTableCRC", CBaseServer::offset_stringTableCRC },
		{ "CHLTVServer::CClientFrameManager", CHLTVServer::offset_CClientFrameManager },
		{ "CHLTVServer::CBaseServer", CHLTVServer::offset_CBaseServer },
		{ "CHLTVServer::m_DemoRecorder", CHLTVServer::offset_m_DemoRecorder },
		{ "CFrameSnapshotManager::m_PackedEntitiesPool", CFrameSnapshotManager::offset_m_PackedEntitiesPool },
		{ "CBaseServer::GetChallengeNr", CBaseServer::vtblindex_GetChallengeNr },
		{ "CBaseServer::GetChallengeType", CBaseServer::vtblindex_GetChallengeType },
		{ "CBaseServer::ReplyChallenge", CBaseServer::vtblindex_ReplyChallenge },
#if SOURCE_ENGINE == SE_LEFT4DEAD2
		{ "CBaseServer::FillServerInfo", CBaseServer::vtblindex_FillServerInfo },
	#if !defined _WIN32
		{ "CHLTVServer::FillServerInfo", CHLTVServer::vtblindex_FillServerInfo },
	#endif
#endif
		{ "CBaseClient::m_SteamID", CBaseClient::offset_m_SteamID },
		{ "CBaseServer::ConnectClient", CBaseServer::vtblindex_ConnectClient },
	};

	for (auto&& el : s_offsets) {
		if (!gc->GetOffset(el.key, &el.offset)) {
			ke::SafeSprintf(error, maxlength, "Unable to get offset for \"%s\" from game config (file: \"" GAMEDATA_FILE ".txt\")", el.key);
			return false;
		}
	}

	static const struct {
		const char* key;
		void*& address;
	} s_sigs[] = {
		{ "DataTable_WriteSendTablesBuffer", pfn_DataTable_WriteSendTablesBuffer },
		{ "CBaseServer::IsExclusiveToLobbyConnections", CBaseServer::pfn_IsExclusiveToLobbyConnections },
		{ "CSteam3Server::NotifyClientDisconnect", CSteam3Server::pfn_NotifyClientDisconnect },
		{ "CHLTVServer::AddNewFrame", CHLTVServer::pfn_AddNewFrame },
		{ "CFrameSnapshotManager::LevelChanged", CFrameSnapshotManager::pfn_LevelChanged },
#if SOURCE_ENGINE == SE_LEFT4DEAD2
		{ "CBaseClient::SendFullConnectEvent", CBaseClient::pfn_SendFullConnectEvent },
#endif
		{ "CBaseAbility::ShouldTransmit", CBaseAbility::pfn_ShouldTransmit },
#ifdef TV_RELAYTEST
		{ "OpenSocketInternal", pfn_OpenSocketInternal },
#endif
	};

	for (auto&& el : s_sigs) {
		if (!gc->GetMemSig(el.key, &el.address)) {
			ke::SafeSprintf(error, maxlength, "Unable to find signature for \"%s\" from game config (file: \"" GAMEDATA_FILE ".txt\")", el.key);
			return false;
		}
		if (el.address == NULL) {
			ke::SafeSprintf(error, maxlength, "Sigscan for \"%s\" failed (game config file: \"" GAMEDATA_FILE ".txt\")", el.key);
			return false;
		}
	}

#if defined _WIN32
	ptrdiff_t relative = 0;
#endif

	static const struct {
		const char* key;
		void*& address;
	} s_addresses[] = {
		{ "ForEachTerrorPlayer<HitAnnouncement>", HitAnnouncement::pfn_ForEachTerrorPlayer },
#if defined _WIN32
		{ "CTerrorPlayer::OnPouncedOnSurvivor::`relofs to ForEachTerrorPlayer<HitAnnouncement>", reinterpret_cast<void*&>(relative) },
#endif
	};

	for (auto&& el : s_addresses) {
		if (!gc->GetAddress(el.key, &el.address)) {
			ke::SafeSprintf(error, maxlength, "Failed to get address of function \"%s\" from game config (file: \"" GAMEDATA_FILE ".txt\")", el.key);
			return false;
		}
		if (el.address == NULL) {
			ke::SafeSprintf(error, maxlength, "Unable to resolve address \"%s\" (game config file: \"" GAMEDATA_FILE ".txt\")", el.key);
			return false;
		}
	}

#if defined _WIN32
	HitAnnouncement::SetupFromRelativeAddress(relative);
#endif

	return true;
}

bool SMExtension::SetupFromSteamAPILibrary(char* error, int maxlength)
{
#if SOURCE_ENGINE == SE_LEFT4DEAD2
	char path[256];
	ke::path::Format(path, sizeof(path), "bin/" LIBSTEAMAPI_FILE);

	char libError[512];
	ke::RefPtr<ke::SharedLib> steam_api = ke::SharedLib::Open(path, libError, sizeof(libError));
	if (!steam_api) {
		ke::SafeSprintf(error, maxlength, "Unable to load library \"%s\" (reason: \"%s\")", path, libError);
		return false;
	}

	static const struct {
		const char* symbol;
		void*& address;
	} s_symbols[] = {
		{ "SteamInternal_CreateInterface", pfn_SteamInternal_CreateInterface },
		{ "SteamInternal_GameServer_Init", pfn_SteamInternal_GameServer_Init },
		{ "SteamGameServer_GetHSteamPipe", pfn_SteamGameServer_GetHSteamPipe },
		{ "SteamGameServer_GetHSteamUser", pfn_SteamGameServer_GetHSteamUser },
	};

	for (auto&& el : s_symbols) {
		el.address = steam_api->lookup(el.symbol);
		if (el.address == NULL) {
			ke::SafeSprintf(error, maxlength, "Unable to find symbol \"%s\" (file: \"%s\")", el.symbol, path);
			return false;
		}
	}
#endif
	return true;
}

void SMExtension::OnGameServer_Init()
{
	OnGameServer_Shutdown();

#if SOURCE_ENGINE == SE_LEFT4DEAD2
	HSteamPipe hSteamPipe = InvokeGetHSteamPipe();
	if (hSteamPipe == 0)
		return;

	ISteamClient* pSteamClient = static_cast<ISteamClient*>(InvokeCreateInterface(STEAMCLIENT_INTERFACE_VERSION));
	if (pSteamClient == NULL)
		return;

	HSteamUser hSteamUser = InvokeGetHSteamUser();
	ISteamGameServer* pSteamGameServer = pSteamClient->GetISteamGameServer(hSteamUser, hSteamPipe, STEAMGAMESERVER_INTERFACE_VERSION);
	if (pSteamGameServer == NULL)
		return;

	g_HookSteamGameServer_LogOff = new KHook::Virtual<ISteamGameServer, void>(
		&ISteamGameServer::LogOff,
		this,
		&SMExtension::Handler_ISteamGameServer_LogOff,
		nullptr
	);
	g_HookSteamGameServer_LogOff->Add(pSteamGameServer);
#endif
}

void SMExtension::OnGameServer_Shutdown()
{
	if (g_HookSteamGameServer_LogOff) {
		delete g_HookSteamGameServer_LogOff;
		g_HookSteamGameServer_LogOff = NULL;
	}
}

void SMExtension::OnSetHLTVServer(IHLTVServer* pIHLTVServer)
{
	// Remove previous
	if (CHLTVServer::hook_ReplyChallenge) {
		delete CHLTVServer::hook_ReplyChallenge;
		CHLTVServer::hook_ReplyChallenge = NULL;
	}
	if (CHLTVServer::hook_FillServerInfo) {
		delete CHLTVServer::hook_FillServerInfo;
		CHLTVServer::hook_FillServerInfo = NULL;
	}
	if (CHLTVServer::hook_hltv_FillServerInfo) {
		delete CHLTVServer::hook_hltv_FillServerInfo;
		CHLTVServer::hook_hltv_FillServerInfo = NULL;
	}
	if (CHLTVServer::hook_ConnectClient) {
		delete CHLTVServer::hook_ConnectClient;
		CHLTVServer::hook_ConnectClient = NULL;
	}
	if (g_HookRecordStringTables) {
		delete g_HookRecordStringTables;
		g_HookRecordStringTables = NULL;
	}
	if (g_HookRecordServerClasses) {
		delete g_HookRecordServerClasses;
		g_HookRecordServerClasses = NULL;
	}
	if (g_HookCheckTransmit) {
		delete g_HookCheckTransmit;
		g_HookCheckTransmit = NULL;
	}

	g_pHLTVServer = pIHLTVServer;
	if (pIHLTVServer == NULL)
		return;

	CBaseServer* pServer = CBaseServer::FromIHLTVServer(pIHLTVServer);
	if (pServer == NULL)
		return;

	CHLTVServer* pHLTVServer = CHLTVServer::FromBaseServer(pServer);

	// Manual hooks by vtable index
	CHLTVServer::hook_ReplyChallenge = new KHook::Virtual<CBaseServer, void, netadr_s&, bf_read&>(
		(std::uint32_t)CBaseServer::vtblindex_ReplyChallenge,
		this,
		&SMExtension::Handler_CHLTVServer_ReplyChallenge,
		nullptr
	);
	CHLTVServer::hook_ReplyChallenge->Add(pServer);

#if SOURCE_ENGINE == SE_LEFT4DEAD2
	CHLTVServer::hook_FillServerInfo = new KHook::Virtual<CBaseServer, void, SVC_ServerInfo&>(
		(std::uint32_t)CBaseServer::vtblindex_FillServerInfo,
		this,
		nullptr,
		&SMExtension::Handler_CHLTVServer_FillServerInfo // post
	);
	CHLTVServer::hook_FillServerInfo->Add(pServer);

#if !defined _WIN32
	CHLTVServer::hook_hltv_FillServerInfo = new KHook::Virtual<CHLTVServer, void, SVC_ServerInfo&>(
		(std::uint32_t)CHLTVServer::vtblindex_FillServerInfo,
		this,
		nullptr,
		&SMExtension::Handler_CHLTVServer_FillServerInfo_HLTV
	);
	CHLTVServer::hook_hltv_FillServerInfo->Add(pHLTVServer);
#endif
#endif

	CHLTVServer::hook_ConnectClient = new KHook::Virtual<CBaseServer, IClient*, netadr_t&, int, int, int, const char*,
		const char*, const char*, int, CUtlVector<NetMessageCvar_t>&, bool>(
		(std::uint32_t)CBaseServer::vtblindex_ConnectClient,
		this,
		&SMExtension::Handler_CHLTVServer_ConnectClient,
		nullptr
	);
	CHLTVServer::hook_ConnectClient->Add(pServer);

	CHLTVDemoRecorder& demoRecorder = pHLTVServer->m_DemoRecorder();

	g_HookRecordStringTables = new KHook::Virtual<CHLTVDemoRecorder, void>(
		&CHLTVDemoRecorder::RecordStringTables,
		this,
		&SMExtension::Handler_CHLTVDemoRecorder_RecordStringTables,
		nullptr
	);
	g_HookRecordStringTables->Add(&demoRecorder);

	g_HookRecordServerClasses = new KHook::Virtual<CHLTVDemoRecorder, void, ServerClass*>(
		&CHLTVDemoRecorder::RecordServerClasses,
		this,
		&SMExtension::Handler_CHLTVDemoRecorder_RecordServerClasses,
		nullptr
	);
	g_HookRecordServerClasses->Add(&demoRecorder);

	g_HookCheckTransmit = new KHook::Virtual<IServerGameEnts, void, CCheckTransmitInfo*, const unsigned short*, int>(
		&IServerGameEnts::CheckTransmit,
		this,
		nullptr,
		&SMExtension::Handler_CServerGameEnts_CheckTransmit // post
	);
	g_HookCheckTransmit->Add(gameents);

	CNetworkStringTable* pStringTableGameRules = static_cast<CNetworkStringTable*>(pServer->m_StringTables()->FindTable("GameRulesCreation"));
	if (pStringTableGameRules != NULL) {
		// This would copy itemchange_s contents into CNetworkStringTableItem without reallocation
		pStringTableGameRules->RestoreTick(TIME_TO_TICKS(1));
	}

	// bug##: in CHLTVServer::StartMaster, bot is executing "spectate" command which does nothing and it keeps him in unassigned team (index 0)
	// bot's going to fall under CDirectorSessionManager::UpdateNewPlayers's conditions to be auto-assigned to some playable team
	// enforce team change here to spectator (index 1)
	CBasePlayer* pPlayer = UTIL_PlayerByIndex(pIHLTVServer->GetHLTVSlot() + 1);
	if (pPlayer != NULL && pPlayer->IsHLTV()) {
		pPlayer->AddFlag(FL_FAKECLIENT);
		pPlayer->ChangeTeam(TEAM_SPECTATOR);
	}

	// bug#1: stringTableCRC are not set in CHLTVServer::StartMaster
	// client doesn't allow stringTableCRC to be empty
	// CHLTVServer instance must copy property stringTableCRC from CGameServer instance
	pServer->stringTableCRC() = CBaseServer::FromIServer(g_pGameIServer)->stringTableCRC();
}

bool SMExtension::SDK_OnLoad(char* error, size_t maxlength, bool late)
{
	HitAnnouncement::pzMsgId = usermsgs->GetMessageIndex("PZDmgMsg");
	if (HitAnnouncement::pzMsgId == -1) {
		ke::SafeStrcpy(error, maxlength, "Unable to find usermessage \"PZDmgMsg\"!");
		return false;
	}

	sm_sendprop_info_t info;
	if (!gamehelpers->FindSendPropInfo("CBasePlayer", "m_fFlags", &info)) {
		ke::SafeStrcpy(error, maxlength, "Unable to find SendProp \"CBasePlayer::m_fFlags\"");
		return false;
	}
	CBasePlayer::sendprop_m_fFlags = info.actual_offset;

	IGameConfig* gc = NULL;
	if (!gameconfs->LoadGameConfigFile(GAMEDATA_FILE, &gc, error, maxlength)) {
		ke::SafeStrcpy(error, maxlength, "Unable to load a gamedata file \"" GAMEDATA_FILE ".txt\"");
		return false;
	}

	if (!SetupFromGameConfig(gc, error, maxlength)) {
		gameconfs->CloseGameConfigFile(gc);
		return false;
	}
	gameconfs->CloseGameConfigFile(gc);

	if (!SetupFromSteamAPILibrary(error, maxlength))
		return false;

	sharesys->AddDependency(myself, "bintools.ext", true, true);
	sharesys->AddDependency(myself, "sdktools.ext", true, true);
	return true;
}

void SMExtension::SDK_OnUnload()
{
	Unload();
}

void SMExtension::SDK_OnAllLoaded()
{
	SM_GET_LATE_IFACE(SDKTOOLS, sdktools);
	SM_GET_LATE_IFACE(BINTOOLS, bintools);
	if (sdktools != NULL && bintools != NULL)
		Load();
}

bool SMExtension::SDK_OnMetamodLoad(ISmmAPI* ismm, char* error, size_t maxlen, bool late)
{
	GET_V_IFACE_CURRENT(GetEngineFactory, networkStringTableContainerServer, INetworkStringTableContainer, INTERFACENAME_NETWORKSTRINGTABLESERVER);
	GET_V_IFACE_CURRENT(GetEngineFactory, g_pNetSupport, INetSupport, INETSUPPORT_VERSION_STRING);
	GET_V_IFACE_CURRENT(GetServerFactory, hltvdirector, IHLTVDirector, INTERFACEVERSION_HLTVDIRECTOR);
	GET_V_IFACE_CURRENT(GetServerFactory, playerinfomanager, IPlayerInfoManager, INTERFACEVERSION_PLAYERINFOMANAGER);
	GET_V_IFACE_CURRENT(GetServerFactory, gameents, IServerGameEnts, INTERFACEVERSION_SERVERGAMEENTS);
	gpGlobals = ismm->GetCGlobals();

	// For ConVarRef
	GET_V_IFACE_CURRENT(GetEngineFactory, g_pCVar, ICvar, CVAR_INTERFACE_VERSION);
	return true;
}

bool SMExtension::QueryInterfaceDrop(SMInterface* pInterface)
{
	if (bintools == pInterface)
		return false;
	if (sdktools == pInterface)
		return g_pGameIServer != NULL;
	return IExtensionInterface::QueryInterfaceDrop(pInterface);
}

void SMExtension::NotifyInterfaceDrop(SMInterface* pInterface)
{
	if (bintools == pInterface || (sdktools == pInterface && g_pGameIServer == NULL))
		SDK_OnUnload();
}

bool SMExtension::QueryRunning(char* error, size_t maxlength)
{
	SM_CHECK_IFACE(SDKTOOLS, sdktools);
	SM_CHECK_IFACE(BINTOOLS, bintools);
	return true;
}
