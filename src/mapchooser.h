#pragma once

// Called from plugin.cpp hooks to notify mapchooser of player lifecycle events.
void MC_OnClientConnected(int slot, bool isFakePlayer);
void MC_OnClientDisconnect(int slot);

// Called every server frame from Hook_ServerGamePostSimulate.
void OnGameFrame();

// Called once after plugin Load.
void MC_Init();

// Set the map pool from a workshop collection query result.
void MapPool_SetFromWorkshop(const char **maps, int count);
