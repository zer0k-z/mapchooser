#pragma once

// Called from GameServerSteamAPIActivated hook once Steam is ready,
// and also attempted from MC_Init() for late-loaded scenarios.
void Workshop_OnSteamAPIActivated();
void Workshop_TryLoad();

// Returns true once the workshop collection has been successfully loaded,
// or immediately if no collection ID is configured (mm_workshop_collection 0).
bool Workshop_IsReady();
