#include "workshop.h"
#include "mapchooser.h"
#include "plugin.h"
#include "ctimer.h"
#include <steam/isteamugc.h>
#include <steam/steam_gameserver.h>
#include <ISmmPlugin.h>
#include <tier1/convar.h>
#include <string.h>
#include <stdlib.h>

// ============================================================
// Convar
// ============================================================

static CConVar<uint64> mm_workshop_collection(
    "mm_workshop_collection",
    FCVAR_NONE,
    "Steam Workshop collection ID to use as the map pool (0 = disabled)",
    (uint64)0
);

// ============================================================
// Workshop loader
// ============================================================

static const int MAX_CHILDREN = 2000;

class CWorkshopLoader
{
public:
    void QueryCollection();

private:
    void OnCollectionQueryCompleted(SteamUGCQueryCompleted_t *pResult, bool bIOFailure);
    void OnItemsQueryCompleted(SteamUGCQueryCompleted_t *pResult, bool bIOFailure);

    CCallResult<CWorkshopLoader, SteamUGCQueryCompleted_t> m_collectionCall;
    CCallResult<CWorkshopLoader, SteamUGCQueryCompleted_t> m_itemsCall;

    // Scratch buffer for child IDs between the two queries
    PublishedFileId_t m_childIds[MAX_CHILDREN];
    uint32 m_numChildren = 0;
};

static CWorkshopLoader g_workshopLoader;
static bool g_workshopQueryStarted = false;
static bool g_workshopReady = false;

// Module-level workshop ID lookup table (populated after each successful query)
static char s_knownNames[MAX_CHILDREN][128];
static uint64_t s_knownIDs[MAX_CHILDREN];
static int s_knownCount = 0;

// ----------------------------------------------------------------

void CWorkshopLoader::QueryCollection()
{
    ISteamUGC *pUGC = SteamGameServerUGC();
    if (!pUGC)
    {
        META_CONPRINTF("[Workshop] SteamGameServerUGC() unavailable\n");
        return;
    }

    uint64 collectionId = mm_workshop_collection.Get();
    if (collectionId == 0)
        return;

    PublishedFileId_t id = (PublishedFileId_t)collectionId;
    UGCQueryHandle_t handle = pUGC->CreateQueryUGCDetailsRequest(&id, 1);
    if (handle == k_UGCQueryHandleInvalid)
    {
        META_CONPRINTF("[Workshop] CreateQueryUGCDetailsRequest failed\n");
        return;
    }

    pUGC->SetReturnChildren(handle, true);

    SteamAPICall_t call = pUGC->SendQueryUGCRequest(handle);
    m_collectionCall.Set(call, this, &CWorkshopLoader::OnCollectionQueryCompleted);

    META_CONPRINTF("[Workshop] Querying collection %llu...\n", collectionId);
}

void CWorkshopLoader::OnCollectionQueryCompleted(SteamUGCQueryCompleted_t *pResult, bool bIOFailure)
{
    ISteamUGC *pUGC = SteamGameServerUGC();

    if (bIOFailure || pResult->m_eResult != k_EResultOK || !pUGC)
    {
        META_CONPRINTF("[Workshop] Collection query failed (result=%d ioFailure=%d), retrying in 10s...\n",
            (int)pResult->m_eResult, (int)bIOFailure);
        if (pUGC) pUGC->ReleaseQueryUGCRequest(pResult->m_handle);
        StartTimer([]() -> double {
            g_workshopLoader.QueryCollection();
            return -1.0;
        }, 10.0, false);
        return;
    }

    SteamUGCDetails_t collDetails;
    if (!pUGC->GetQueryUGCResult(pResult->m_handle, 0, &collDetails)
        || collDetails.m_unNumChildren == 0)
    {
        META_CONPRINTF("[Workshop] Collection has no children or result unavailable, retrying in 10s...\n");
        pUGC->ReleaseQueryUGCRequest(pResult->m_handle);
        StartTimer([]() -> double {
            g_workshopLoader.QueryCollection();
            return -1.0;
        }, 10.0, false);
        return;
    }

    m_numChildren = collDetails.m_unNumChildren < MAX_CHILDREN
        ? collDetails.m_unNumChildren : MAX_CHILDREN;

    pUGC->GetQueryUGCChildren(pResult->m_handle, 0, m_childIds, m_numChildren);
    pUGC->ReleaseQueryUGCRequest(pResult->m_handle);

    META_CONPRINTF("[Workshop] Collection has %u items, fetching details...\n", m_numChildren);

    // Query details for all children to obtain titles + install state
    UGCQueryHandle_t itemHandle =
        pUGC->CreateQueryUGCDetailsRequest(m_childIds, m_numChildren);
    if (itemHandle == k_UGCQueryHandleInvalid)
    {
        META_CONPRINTF("[Workshop] CreateQueryUGCDetailsRequest for items failed\n");
        return;
    }

    SteamAPICall_t call = pUGC->SendQueryUGCRequest(itemHandle);
    m_itemsCall.Set(call, this, &CWorkshopLoader::OnItemsQueryCompleted);
}

void CWorkshopLoader::OnItemsQueryCompleted(SteamUGCQueryCompleted_t *pResult, bool bIOFailure)
{
    ISteamUGC *pUGC = SteamGameServerUGC();

    if (bIOFailure || pResult->m_eResult != k_EResultOK || !pUGC)
    {
        META_CONPRINTF("[Workshop] Items query failed (result=%d ioFailure=%d), retrying in 10s...\n",
            (int)pResult->m_eResult, (int)bIOFailure);
        if (pUGC) pUGC->ReleaseQueryUGCRequest(pResult->m_handle);
        StartTimer([]() -> double {
            g_workshopLoader.QueryCollection();
            return -1.0;
        }, 10.0, false);
        return;
    }

    // Static name buffer — lives as long as the process
    static char s_mapNames[MAX_CHILDREN][128];
    static uint64_t s_mapIDs[MAX_CHILDREN];
    const char *mapPtrs[MAX_CHILDREN];
    int mapCount = 0;

    for (uint32 i = 0; i < pResult->m_unNumResultsReturned; i++)
    {
        SteamUGCDetails_t details;
        if (!pUGC->GetQueryUGCResult(pResult->m_handle, i, &details))
            continue;

        if (details.m_eResult != k_EResultOK)
            continue;

        // Download the map if it isn't fully installed
        uint32 state = pUGC->GetItemState(details.m_nPublishedFileId);
        if (!(state & k_EItemStateInstalled))
        {
            META_CONPRINTF("[Workshop] Downloading %llu (%s)...\n",
                details.m_nPublishedFileId, details.m_rgchTitle);
            pUGC->DownloadItem(details.m_nPublishedFileId, true /*high priority*/);
        }

        strncpy(s_mapNames[mapCount], details.m_rgchTitle, 127);
        s_mapNames[mapCount][127] = '\0';
        s_mapIDs[mapCount] = (uint64_t)details.m_nPublishedFileId;
        mapPtrs[mapCount] = s_mapNames[mapCount];
        mapCount++;
    }

    pUGC->ReleaseQueryUGCRequest(pResult->m_handle);

    // Update module-level lookup table
    s_knownCount = mapCount;
    for (int i = 0; i < mapCount; i++)
    {
        strncpy(s_knownNames[i], s_mapNames[i], 127);
        s_knownNames[i][127] = '\0';
        s_knownIDs[i] = s_mapIDs[i];
    }

    MapPool_SetFromWorkshop(mapPtrs, mapCount);
    META_CONPRINTF("[Workshop] Map pool set: %d maps loaded from workshop.\n", mapCount);
    g_workshopReady = true;
}

// ============================================================
// Workshop ID lookup
// ============================================================

uint64_t Workshop_GetMapID(const char *mapName)
{
    for (int i = 0; i < s_knownCount; i++)
        if (V_stricmp(s_knownNames[i], mapName) == 0)
            return s_knownIDs[i];
    return 0;
}

// ============================================================
// Public API
// ============================================================

bool Workshop_IsReady()
{
    return g_workshopReady;
}

void Workshop_TryLoad()
{
    if (g_workshopQueryStarted)
        return;

    // Guard even if SteamUGC isn't ready yet — the hook will retry
    if (!SteamGameServerUGC())
        return;

    if (mm_workshop_collection.Get() == 0)
        return;

    g_workshopQueryStarted = true;
    g_workshopLoader.QueryCollection();
}

void Workshop_OnSteamAPIActivated()
{
    g_workshopQueryStarted = false; // allow a fresh query when Steam comes up
    g_workshopReady = false;
    Workshop_TryLoad();

    // Poll the collection every 10 real-time minutes so map pool stays up to date.
    StartTimer([]() -> double {
        g_workshopQueryStarted = false;
        g_workshopLoader.QueryCollection();
        return 600.0;
    }, 600.0, true /*persistent*/, true /*useRealTime*/);
}
