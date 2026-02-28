#include <stdio.h>
#include "plugin.h"
#include "simplecmds.h"
#include "ctimer.h"
#include "module.h"
#include "gamerules.h"
#include "mapchooser.h"
#include "workshop.h"
#include <eiface.h>
#include <igamesystem.h>
#include <schemasystem/schemasystem.h>
#include <interfaces/interfaces.h>
#include <entity2/entitysystem.h>
#include <engine/igameeventsystem.h>
#include <networksystem/inetworkmessages.h>

SH_DECL_HOOK2_void(ISource2GameClients, ClientCommand, SH_NOATTRIB, false, CPlayerSlot, const CCommand &);
SH_DECL_HOOK3_void(ICvar, DispatchConCommand, SH_NOATTRIB, 0, ConCommandRef, const CCommandContext &, const CCommand &);
SH_DECL_HOOK1_void(IGameSystem, ServerGamePostSimulate, SH_NOATTRIB, false, const EventServerGamePostSimulate_t *);
SH_DECL_HOOK6_void(ISource2GameClients, OnClientConnected, SH_NOATTRIB, false, CPlayerSlot, const char *, uint64, const char *, const char *, bool);
SH_DECL_HOOK5_void(ISource2GameClients, ClientDisconnect, SH_NOATTRIB, false, CPlayerSlot, ENetworkDisconnectionReason, const char *, uint64, const char *);
SH_DECL_HOOK0_void(IServerGameDLL, GameServerSteamAPIActivated, SH_NOATTRIB, 0);

MMSPlugin g_ThisPlugin;
PLUGIN_EXPOSE(MMSPlugin, g_ThisPlugin);

IGameEventSystem *g_pGameEventSystem = nullptr;
ISource2Server *g_pServer = nullptr;
static void *g_pGameResourceService = nullptr;
static CModule *g_serverModule = nullptr;
static int g_serverGamePostSimulateHook = 0;

// GameEntitySystem() accessor required by entity2 SDK source files.
// CGameResourceService stores the CGameEntitySystem* at a known byte offset.
#ifdef _WIN32
static constexpr int GAME_ENTITY_SYSTEM_OFFSET = 88;
#else
static constexpr int GAME_ENTITY_SYSTEM_OFFSET = 80;
#endif

CGameEntitySystem *GameEntitySystem()
{
	if (!g_pGameResourceService)
		return nullptr;
	return *reinterpret_cast<CGameEntitySystem **>((uintptr_t)g_pGameResourceService + GAME_ENTITY_SYSTEM_OFFSET);
}

static void Hook_ClientCommand(CPlayerSlot slot, const CCommand &args)
{
	RETURN_META(scmd::OnClientCommand(slot, args));
}

static void Hook_DispatchConCommand(ConCommandRef cmd, const CCommandContext &ctx, const CCommand &args)
{
	RETURN_META(scmd::OnDispatchConCommand(cmd, ctx, args));
}

static void Hook_ServerGamePostSimulate(const EventServerGamePostSimulate_t *)
{
	ProcessTimers();
	OnGameFrame();
	RETURN_META(MRES_IGNORED);
}

static void Hook_OnClientConnected(CPlayerSlot slot, const char *pszName, uint64 xuid,
	const char *pszNetworkID, const char *pszAddress, bool bFakePlayer)
{
	MC_OnClientConnected(slot.Get(), bFakePlayer);
	RETURN_META(MRES_IGNORED);
}

static void Hook_ClientDisconnect(CPlayerSlot slot, ENetworkDisconnectionReason reason,
	const char *pszName, uint64 xuid, const char *pszNetworkID)
{
	MC_OnClientDisconnect(slot.Get());
	RETURN_META(MRES_IGNORED);
}

static void Hook_GameServerSteamAPIActivated()
{
	Workshop_OnSteamAPIActivated();
	RETURN_META(MRES_IGNORED);
}

bool MMSPlugin::Load(PluginId id, ISmmAPI *ismm, char *error, size_t maxlen, bool late)
{
	PLUGIN_SAVEVARS();

	GET_V_IFACE_CURRENT(GetServerFactory, g_pSource2GameClients, ISource2GameClients, INTERFACEVERSION_SERVERGAMECLIENTS);
	GET_V_IFACE_CURRENT(GetServerFactory, g_pServer, ISource2Server, INTERFACEVERSION_SERVERGAMEDLL);
	GET_V_IFACE_CURRENT(GetEngineFactory, g_pEngineServer, IVEngineServer2, INTERFACEVERSION_VENGINESERVER);
	GET_V_IFACE_CURRENT(GetEngineFactory, g_pSchemaSystem, ISchemaSystem, SCHEMASYSTEM_INTERFACE_VERSION);
	GET_V_IFACE_CURRENT(GetEngineFactory, g_pGameResourceService, void, GAMERESOURCESERVICESERVER_INTERFACE_VERSION);
	GET_V_IFACE_CURRENT(GetEngineFactory, g_pNetworkMessages, INetworkMessages, NETWORKMESSAGES_INTERFACE_VERSION);
	GET_V_IFACE_CURRENT(GetEngineFactory, g_pGameEventSystem, IGameEventSystem, GAMEEVENTSYSTEM_INTERFACE_VERSION);
	GET_V_IFACE_CURRENT(GetEngineFactory, g_pCVar, ICvar, CVAR_INTERFACE_VERSION);
	g_serverModule = new CModule(GAMEBIN, "server");

	SH_ADD_HOOK(ISource2GameClients, ClientCommand, g_pSource2GameClients, SH_STATIC(Hook_ClientCommand), false);
	SH_ADD_HOOK(ICvar, DispatchConCommand, g_pCVar, SH_STATIC(Hook_DispatchConCommand), false);
	SH_ADD_HOOK(ISource2GameClients, OnClientConnected, g_pSource2GameClients, SH_STATIC(Hook_OnClientConnected), false);
	SH_ADD_HOOK(ISource2GameClients, ClientDisconnect, g_pSource2GameClients, SH_STATIC(Hook_ClientDisconnect), true);
	SH_ADD_HOOK(IServerGameDLL, GameServerSteamAPIActivated, g_pServer, SH_STATIC(Hook_GameServerSteamAPIActivated), false);
	g_serverGamePostSimulateHook = SH_ADD_DVPHOOK(
		IGameSystem,
		ServerGamePostSimulate,
		(IGameSystem *)g_serverModule->FindVirtualTable("CEntityDebugGameSystem"),
		SH_STATIC(Hook_ServerGamePostSimulate),
		false
	);
	META_CONVAR_REGISTER(FCVAR_RELEASE | FCVAR_GAMEDLL);
	g_SMAPI->AddListener(this, this);
	MC_Init();

	return true;
}

bool MMSPlugin::Unload(char *error, size_t maxlen)
{
	SH_REMOVE_HOOK(ISource2GameClients, ClientCommand, g_pSource2GameClients, SH_STATIC(Hook_ClientCommand), false);
	SH_REMOVE_HOOK(ICvar, DispatchConCommand, g_pCVar, SH_STATIC(Hook_DispatchConCommand), false);
	SH_REMOVE_HOOK(ISource2GameClients, OnClientConnected, g_pSource2GameClients, SH_STATIC(Hook_OnClientConnected), false);
	SH_REMOVE_HOOK(ISource2GameClients, ClientDisconnect, g_pSource2GameClients, SH_STATIC(Hook_ClientDisconnect), true);
	SH_REMOVE_HOOK(IServerGameDLL, GameServerSteamAPIActivated, g_pServer, SH_STATIC(Hook_GameServerSteamAPIActivated), false);
	SH_REMOVE_HOOK_ID(g_serverGamePostSimulateHook);

	g_PersistentTimers.PurgeAndDeleteElements();
	g_NonPersistentTimers.PurgeAndDeleteElements();

	delete g_serverModule;
	g_serverModule = nullptr;

	return true;
}

void MMSPlugin::OnLevelInit(char const *pMapName, char const *pMapEntities, char const *pOldLevel,
	char const *pLandmarkName, bool loadGame, bool background)
{
	MC_OnLevelInit();
}

void MMSPlugin::AllPluginsLoaded()
{
	/* This is where we'd do stuff that relies on the mod or other plugins 
	 * being initialized (for example, cvars added and events registered).
	 */
}