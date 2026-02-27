#pragma once

#define MAX_PLAYERS 64

// Returns true if slot has a real (non-bot, non-disconnected) player.
bool IsRealPlayer(int slot);

// Returns the number of real (human) players currently on the server.
int GetRealPlayerCount();

// Called from plugin hooks.
void Players_OnClientConnected(int slot, bool isFakePlayer);
void Players_OnClientDisconnect(int slot);
