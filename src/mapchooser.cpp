#include "plugin.h"
#include "simplecmds.h"
#include "ctimer.h"
#include "gamerules.h"
#include "print.h"
#include "players.h"
#include "vote.h"
#include "mapchooser.h"
#include "workshop.h"
#include <string.h>
#include <stdio.h>

// ============================================================
// Constants
// ============================================================

#define RTV_NEEDED_RATIO    0.6f   // 60% of real players must !rtv
#define RTV_DELAY_SECONDS   60.0   // Seconds after map start before RTV is allowed
#define RTV_FAILED_COOLDOWN 300.0  // Seconds before a new RTV is allowed after a failed vote
#define MAX_NOMINATIONS     8      // Max unique nominations shown in vote
#define VOTE_MAP_SLOTS      5      // Number of map options in a vote (excludes Extend & No Vote)

#define MAX_MAP_POOL        2000

// How many seconds before map-end to trigger the pre-vote
#define VOTE_PRE_TIME_SECONDS 300.0f

// ============================================================
// Map pool
// ============================================================

static char g_mapPool[MAX_MAP_POOL][128];
static int  g_mapPoolCount = 0;

void MapPool_SetFromWorkshop(const char **maps, int count)
{
    g_mapPoolCount = 0;
    for (int i = 0; i < count && i < MAX_MAP_POOL; i++)
    {
        strncpy(g_mapPool[g_mapPoolCount], maps[i], 127);
        g_mapPool[g_mapPoolCount][127] = '\0';
        g_mapPoolCount++;
    }
    META_CONPRINTF("[MapChooser] Map pool updated: %d maps from workshop.\n", g_mapPoolCount);
}

static bool IsInMapPool(const char *mapName)
{
    for (int i = 0; i < g_mapPoolCount; i++)
        if (V_stricmp(g_mapPool[i], mapName) == 0)
            return true;
    return false;
}

static const char *GetCurrentMapName()
{
    CGlobalVars *pGlobals = g_pEngineServer->GetServerGlobals();
    if (!pGlobals)
        return "";
    return pGlobals->mapname.ToCStr();
}

// ============================================================
// Nomination state
// ============================================================

static char g_nominations[MAX_PLAYERS][128]; // per-slot nomination ("" = none)

static void ClearNominations()
{
    for (int i = 0; i < MAX_PLAYERS; i++)
        g_nominations[i][0] = '\0';
}

// Fills `out` with unique nominations sorted by number of nominators.
// Returns count. out must have room for MAX_NOMINATIONS entries.
static int BuildNominationList(char out[][128], int maxOut)
{
    static char maps[MAX_PLAYERS][128];
    static int counts[MAX_PLAYERS];
    int uniqueCount = 0;

    for (int i = 0; i < MAX_PLAYERS; i++)
    {
        if (!IsRealPlayer(i) || g_nominations[i][0] == '\0')
            continue;

        bool found = false;
        for (int j = 0; j < uniqueCount; j++)
        {
            if (V_stricmp(maps[j], g_nominations[i]) == 0)
            {
                counts[j]++;
                found = true;
                break;
            }
        }
        if (!found && uniqueCount < MAX_PLAYERS)
        {
            strncpy(maps[uniqueCount], g_nominations[i], 127);
            maps[uniqueCount][127] = '\0';
            counts[uniqueCount] = 1;
            uniqueCount++;
        }
    }

    // Insertion sort by count descending
    for (int i = 1; i < uniqueCount; i++)
    {
        char tmpMap[128];
        int tmpCount = counts[i];
        strncpy(tmpMap, maps[i], sizeof(tmpMap));
        int j = i - 1;
        while (j >= 0 && counts[j] < tmpCount)
        {
            counts[j + 1] = counts[j];
            strncpy(maps[j + 1], maps[j], sizeof(maps[j + 1]));
            j--;
        }
        counts[j + 1] = tmpCount;
        strncpy(maps[j + 1], tmpMap, sizeof(maps[j + 1]));
    }

    int count = uniqueCount < maxOut ? uniqueCount : maxOut;
    for (int i = 0; i < count; i++)
    {
        strncpy(out[i], maps[i], 127);
        out[i][127] = '\0';
    }
    return count;
}

// ============================================================
// RTV state
// ============================================================

static bool g_rtvAllowed = false;
static bool g_rtvVoters[MAX_PLAYERS] = {};
static int g_rtvCount = 0;
static bool g_voteTriggered = false;
static char g_nextMap[128] = ""; // set when vote picks a map; used to changelevel at intermission end

static void ResetRtvState()
{
    g_rtvAllowed = false;
    g_rtvCount = 0;
    g_voteTriggered = false;
    g_nextMap[0] = '\0';
    for (int i = 0; i < MAX_PLAYERS; i++)
        g_rtvVoters[i] = false;
    ClearNominations();
}

static int GetRtvThreshold()
{
    int real = GetRealPlayerCount();
    int needed = (int)(real * RTV_NEEDED_RATIO + 0.5f);
    return needed < 1 ? 1 : needed;
}

// ============================================================
// Convar refs
// ============================================================

CConVarRef<CUtlString> nextlevel("nextlevel");
CConVarRef<int> mp_match_restart_delay("mp_match_restart_delay");
CConVarRef<float> mp_timelimit("mp_timelimit");

// ============================================================
// Vote building
// ============================================================

static void OnVoteEnd(int winnerIdx, int *voteCounts, int numOptions);

// Tracks the options for the current vote so OnVoteEnd can act on the winner.
static bool g_voteHasExtend = false;
static char g_voteSelectedMaps[VOTE_MAP_SLOTS][128];
static int  g_voteSelectedCount = 0;

// Simple LCG for random map selection (no stdlib rand needed)
static uint32_t g_lcgSeed = 12345;
static uint32_t LCGRand()
{
    g_lcgSeed = g_lcgSeed * 1664525u + 1013904223u;
    return g_lcgSeed;
}

static void TriggerMapVote(const char *reason, bool allowExtend)
{
    if (!Workshop_IsReady())
    {
        META_CONPRINTF("[MapChooser] Vote skipped: workshop collection not ready yet.\n");
        return;
    }
    if (g_voteTriggered)
    {
        PrintChatAll("\x02[Vote]\x01 A vote has already been triggered this map.");
        return;
    }
    if (IsVoteActive())
        return;

    g_voteTriggered = true;
    g_voteHasExtend  = allowExtend;

    // Collect up to VOTE_MAP_SLOTS map names:
    // 1) highest-nominated maps first
    // 2) fill remaining with random maps from pool (no duplicates)
    char selectedMaps[VOTE_MAP_SLOTS][128];
    int selectedCount = 0;

    const char *currentMap = GetCurrentMapName();

    // Step 1: nominations
    char nomMaps[MAX_NOMINATIONS][128];
    int nomCount = BuildNominationList(nomMaps, MAX_NOMINATIONS);
    for (int i = 0; i < nomCount && selectedCount < VOTE_MAP_SLOTS; i++)
    {
        if (V_stricmp(nomMaps[i], currentMap) == 0)
            continue; // skip current map
        strncpy(selectedMaps[selectedCount], nomMaps[i], 127);
        selectedMaps[selectedCount][127] = '\0';
        selectedCount++;
    }

    // Step 2: random fill from pool for remaining slots
    if (g_mapPoolCount > 0 && selectedCount < VOTE_MAP_SLOTS)
    {
        // Build a shuffled index list (Fisher-Yates on a local copy)
        static int indices[MAX_MAP_POOL];
        for (int i = 0; i < g_mapPoolCount; i++) indices[i] = i;
        for (int i = g_mapPoolCount - 1; i > 0; i--)
        {
            int j = (int)(LCGRand() % (uint32_t)(i + 1));
            int tmp = indices[i]; indices[i] = indices[j]; indices[j] = tmp;
        }

        for (int i = 0; i < g_mapPoolCount && selectedCount < VOTE_MAP_SLOTS; i++)
        {
            const char *candidate = g_mapPool[indices[i]];

            // Skip current map or already selected
            if (V_stricmp(candidate, currentMap) == 0) continue;
            bool dup = false;
            for (int j = 0; j < selectedCount; j++)
                if (V_stricmp(selectedMaps[j], candidate) == 0) { dup = true; break; }
            if (dup) continue;

            strncpy(selectedMaps[selectedCount], candidate, 127);
            selectedMaps[selectedCount][127] = '\0';
            selectedCount++;
        }
    }

    // Copy selected maps for OnVoteEnd
    g_voteSelectedCount = selectedCount;
    for (int i = 0; i < selectedCount; i++)
    {
        strncpy(g_voteSelectedMaps[i], selectedMaps[i], 127);
        g_voteSelectedMaps[i][127] = '\0';
    }

    // Build final VoteOption list: [Extend |] map1..mapN | No Vote
    VoteOption options[MAX_VOTE_OPTIONS];
    int numOptions = 0;

    if (allowExtend)
    {
        strncpy(options[numOptions].name, "Extend Current Map", sizeof(options[0].name));
        numOptions++;
    }

    for (int i = 0; i < selectedCount; i++)
    {
        strncpy(options[numOptions].name, selectedMaps[i], sizeof(options[0].name));
        options[numOptions].name[sizeof(options[0].name) - 1] = '\0';
        numOptions++;
    }

    strncpy(options[numOptions].name, "No Vote / Abstain", sizeof(options[0].name));
    numOptions++;

    StartVote(reason, options, numOptions, 30.0f, OnVoteEnd);
}

static void OnVoteEnd(int winnerIdx, int *voteCounts, int numOptions)
{
    // Helper: when an RTV vote ends with no map change, reset voter state and
    // re-arm after RTV_FAILED_COOLDOWN so players can try again.
    auto HandleRtvFailed = [&]() {
        if (g_voteHasExtend)
            return; // end-of-map vote — nothing to re-arm
        g_rtvCount = 0;
        for (int i = 0; i < MAX_PLAYERS; i++) g_rtvVoters[i] = false;
        PrintChatAll("\x04[RTV]\x01 A new RTV will be possible in 5 minutes.");
        StartTimer([]() -> double {
            g_voteTriggered = false;
            return -1.0;
        }, RTV_FAILED_COOLDOWN, false);
    };

    if (winnerIdx < 0)
    {
        PrintChatAll("\x04[Vote]\x01 No map was chosen.");
        HandleRtvFailed();
        return;
    }

    // Last option is always "No Vote / Abstain"
    if (winnerIdx == numOptions - 1)
    {
        PrintChatAll("\x04[Vote]\x01 No vote won \xe2\x80\x94 map will proceed as normal.");
        HandleRtvFailed();
        return;
    }

    // Extend option is index 0 when present
    if (g_voteHasExtend && winnerIdx == 0)
    {
        float newLimit = mp_timelimit.GetFloat() + 15.0f;
        mp_timelimit.Set(newLimit);
        PrintChatAll("\x04[Vote]\x01 Map extended by 15 minutes!");
        g_voteTriggered = false;
        return;
    }

    // Map winner: adjust index to account for optional Extend slot
    int mapIdx = g_voteHasExtend ? (winnerIdx - 1) : winnerIdx;
    if (mapIdx >= 0 && mapIdx < g_voteSelectedCount)
    {
        strncpy(g_nextMap, g_voteSelectedMaps[mapIdx], 127);
        g_nextMap[127] = '\0';

        char msg[256];
        snprintf(msg, sizeof(msg), "\x04[Vote]\x01 \x05%s\x01 will be the next map!", g_nextMap);
        PrintChatAll(msg);

        if (!g_voteHasExtend)
        {
            // RTV: change map after 5 seconds
            StartTimer([]() -> double {
                if (g_nextMap[0] != '\0')
                {
                    uint64_t workshopID = Workshop_GetMapID(g_nextMap);
                    char cmd[160];
                    if (workshopID)
                        snprintf(cmd, sizeof(cmd), "host_workshop_map %llu", workshopID);
                    else
                        snprintf(cmd, sizeof(cmd), "changelevel %s", g_nextMap);
                    g_pEngineServer->ServerCommand(cmd);
                    g_nextMap[0] = '\0';
                }
                return -1.0;
            }, 5.0, false);
        }
        else
        {
            // End-of-map: set nextlevel and let the changelevel block in OnGameFrame handle it
            nextlevel.Set(g_nextMap);
        }
    }
}

// ============================================================
// Commands
// ============================================================

CMD_NAMED(cmd_rtv, "rtv")
{
    if (!g_rtvAllowed)
    {
        PrintChatToSlot(controller_id, "\x02[RTV]\x01 Rock the vote is not available yet.");
        return MRES_SUPERCEDE;
    }
    if (g_voteTriggered)
    {
        PrintChatToSlot(controller_id, "\x02[RTV]\x01 A vote has already been triggered this map.");
        return MRES_SUPERCEDE;
    }
    if (g_rtvVoters[controller_id])
    {
        char msg[128];
        int needed = GetRtvThreshold() - g_rtvCount;
        snprintf(msg, sizeof(msg), "\x04[RTV]\x01 You already voted. Need \x05%d\x01 more vote(s).", needed);
        PrintChatToSlot(controller_id, msg);
        return MRES_SUPERCEDE;
    }

    g_rtvVoters[controller_id] = true;
    g_rtvCount++;

    int threshold = GetRtvThreshold();
    char msg[256];
    if (g_rtvCount >= threshold)
    {
        snprintf(msg, sizeof(msg), "\x04[RTV]\x01 (%d/%d) Starting vote!", g_rtvCount, threshold);
        PrintChatAll(msg);
        TriggerMapVote("Rock The Vote!", false);
    }
    else
    {
        snprintf(msg, sizeof(msg), "\x04[RTV]\x01 (%d/%d) — need \x05%d\x01 more vote(s).",
            g_rtvCount, threshold, threshold - g_rtvCount);
        PrintChatAll(msg);
    }
    return MRES_SUPERCEDE;
}

CMD_NAMED(cmd_nominate, "nominate")
{
    if (args->ArgC() < 2)
    {
        PrintChatToSlot(controller_id, "\x04[Nominate]\x01 Usage: !nominate <mapname>");
        return MRES_SUPERCEDE;
    }

    const char *query = args->Arg(1);

    // Find the map: exact match first, then first substring match
    const char *mapName = nullptr;
    const char *currentMap = GetCurrentMapName();
    for (int i = 0; i < g_mapPoolCount; i++)
    {
        if (V_stricmp(g_mapPool[i], query) == 0)
        {
            mapName = g_mapPool[i];
            break;
        }
    }
    if (!mapName)
    {
        for (int i = 0; i < g_mapPoolCount; i++)
        {
            if (V_stristr(g_mapPool[i], query))
            {
                mapName = g_mapPool[i];
                break;
            }
        }
    }

    if (!mapName)
    {
        char msg[256];
        snprintf(msg, sizeof(msg), "\x02[Nominate]\x01 No map matching \x05%s\x01 found in the pool.", query);
        PrintChatToSlot(controller_id, msg);
        return MRES_SUPERCEDE;
    }

    if (V_stricmp(mapName, currentMap) == 0)
    {
        PrintChatToSlot(controller_id, "\x02[Nominate]\x01 You cannot nominate the current map.");
        return MRES_SUPERCEDE;
    }

    strncpy(g_nominations[controller_id], mapName, 127);
    g_nominations[controller_id][127] = '\0';

    char msg[256];
    snprintf(msg, sizeof(msg), "\x04[Nominate]\x01 \x05%s\x01 has been nominated.", mapName);
    PrintChatAll(msg);
    return MRES_SUPERCEDE;
}

CMD_WILDCARD(digit_cmd)
{
    const char *name = (*args)[0] + 1; // skip ! or /
    if (name[0] >= '0' && name[0] <= '9' && name[1] == '\0')
    {
        int digit = name[0] - '0';
        if (IsVoteActive())
            Vote_HandleDigit(controller_id, digit);
        return MRES_SUPERCEDE;
    }
    return MRES_IGNORED;
}

// ============================================================
// Client lifecycle (called from plugin.cpp)
// ============================================================

void MC_OnClientConnected(int slot, bool isFakePlayer)
{
    Players_OnClientConnected(slot, isFakePlayer);
    if (!isFakePlayer)
        g_rtvVoters[slot] = false;
}

void MC_OnClientDisconnect(int slot)
{
    // Vote bookkeeping before players.h marks them gone
    Vote_OnClientDisconnect(slot);
    Players_OnClientDisconnect(slot);

    if (g_rtvVoters[slot])
    {
        g_rtvVoters[slot] = false;
        if (g_rtvCount > 0) g_rtvCount--;
    }
    g_nominations[slot][0] = '\0';
}

// ============================================================
// Game frame
// ============================================================

static bool g_wasInIntermission = false;

void OnGameFrame()
{
    CCSGameRules *rules = GetGameRules();

    bool inIntermission = rules
        && rules->m_flIntermissionStartTime().GetTime() > 0.0f;

    // ---- Pre-intermission vote: fire when time remaining hits the threshold ----
    if (!g_voteTriggered && !IsVoteActive() && !inIntermission)
    {
        CGlobalVars *globals = g_pEngineServer ? g_pEngineServer->GetServerGlobals() : nullptr;
        if (rules && globals)
        {
            float timelimit = mp_timelimit.GetFloat();
            if (timelimit > 0.0f)
            {
                float mapEnd   = rules->m_flGameStartTime().GetTime() + timelimit * 60.0f;
                float remaining = mapEnd - globals->curtime;
                if (remaining <= VOTE_PRE_TIME_SECONDS)
                    TriggerMapVote("Map Ending Soon \xe2\x80\x94 Pick the next map!", true);
            }
        }
    }

    // ---- Manual changelevel 1s before intermission ends ----
    if (inIntermission && g_nextMap[0] != '\0')
    {
        CGlobalVars *globals = g_pEngineServer ? g_pEngineServer->GetServerGlobals() : nullptr;
        if (globals)
        {
            float intermissionEnd = rules->m_flIntermissionStartTime().GetTime()
                                    + mp_match_restart_delay.GetFloat();
            if (globals->curtime >= intermissionEnd - 1.0f)
            {
                uint64_t workshopID = Workshop_GetMapID(g_nextMap);
                char cmd[160];
                if (workshopID)
                    snprintf(cmd, sizeof(cmd), "host_workshop_map %llu", workshopID);
                else
                    snprintf(cmd, sizeof(cmd), "changelevel %s", g_nextMap);
                g_pEngineServer->ServerCommand(cmd);
                g_nextMap[0] = '\0'; // prevent re-firing
            }
        }
    }

    // ---- Fallback: intermission started without a pre-vote (e.g. mp_timelimit 0) ----
    if (inIntermission && !g_wasInIntermission)
    {
        if (!g_voteTriggered && !IsVoteActive())
            TriggerMapVote("End of Map \xe2\x80\x94 Pick the next map!", true);
    }

    // ---- Reset all state when a new map begins ----
    if (!inIntermission && g_wasInIntermission)
        ResetRtvState();

    g_wasInIntermission = inIntermission;
}

// ============================================================
// Init (called from plugin Load)
// ============================================================

void MC_Init()
{
    // Map pool is populated asynchronously via Workshop_TryLoad() once Steam API is active.
    // Workshop_OnSteamAPIActivated() will call Workshop_TryLoad() at the right moment.
    Workshop_TryLoad();
    ResetRtvState();

    // Allow RTV after a delay so players can't RTV immediately on spawn
    StartTimer([]() -> double {
        g_rtvAllowed = true;
        return -1.0;
    }, RTV_DELAY_SECONDS, false);
}
