#pragma once
#include <stdint.h>

// Called from GameServerSteamAPIActivated hook once Steam is ready,
// and also attempted from MC_Init() for late-loaded scenarios.
void Workshop_OnSteamAPIActivated();
void Workshop_TryLoad();

// Returns true once the workshop collection has been successfully loaded,
// or immediately if no collection ID is configured (mm_workshop_collection 0).
bool Workshop_IsReady();

// Returns the workshop file ID for a map by title, or 0 if not found.
uint64_t Workshop_GetMapID(const char *mapName);
