#include "players.h"
#include "serversideclient.h"
#include <iserver.h>
#include <tier1/utlvector.h>

// Byte offset from INetworkGameServer to CUtlVector<CServerSideClient *>.
// Matches the value used in cs2docker-autorestart.
#ifdef _WIN32
static constexpr int CLIENT_LIST_OFFSET = 592;
#else
static constexpr int CLIENT_LIST_OFFSET = 592;
#endif

static CUtlVector<CServerSideClient *> *GetClientList()
{
    if (!g_pNetworkServerService)
        return nullptr;
    return reinterpret_cast<CUtlVector<CServerSideClient *> *>(
        reinterpret_cast<char *>(g_pNetworkServerService->GetIGameServer()) + CLIENT_LIST_OFFSET);
}

bool IsRealPlayer(int slot)
{
    auto *clients = GetClientList();
    if (!clients)
        return false;

    FOR_EACH_VEC(*clients, i)
    {
        CServerSideClient *client = clients->Element(i);
        if (client && client->IsConnected() && !client->IsFakeClient() && !client->IsHLTV())
        {
            if (client->GetPlayerSlot().Get() == slot)
                return true;
        }
    }
    return false;
}

int GetRealPlayerCount()
{
    int count = 0;
    auto *clients = GetClientList();
    if (!clients)
        return 0;

    FOR_EACH_VEC(*clients, i)
    {
        CServerSideClient *client = clients->Element(i);
        if (client && client->IsConnected() && !client->IsFakeClient() && !client->IsHLTV())
            count++;
    }
    return count;
}

// No-ops: player state is now queried live from the network server client list.
void Players_OnClientConnected(int /*slot*/, bool /*isFakePlayer*/) {}
void Players_OnClientDisconnect(int /*slot*/) {}
