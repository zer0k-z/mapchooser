#include "players.h"

static bool g_realPlayer[MAX_PLAYERS] = {};

bool IsRealPlayer(int slot)
{
    if (slot < 0 || slot >= MAX_PLAYERS)
        return false;
    return g_realPlayer[slot];
}

int GetRealPlayerCount()
{
    int count = 0;
    for (int i = 0; i < MAX_PLAYERS; i++)
        if (g_realPlayer[i]) count++;
    return count;
}

void Players_OnClientConnected(int slot, bool isFakePlayer)
{
    if (slot < 0 || slot >= MAX_PLAYERS)
        return;
    g_realPlayer[slot] = !isFakePlayer;
}

void Players_OnClientDisconnect(int slot)
{
    if (slot < 0 || slot >= MAX_PLAYERS)
        return;
    g_realPlayer[slot] = false;
}
