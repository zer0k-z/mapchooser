#include "workshop.h"
#include "mapchooser.h"
#include "plugin.h"
#include "ctimer.h"
#include <steam/isteamhttp.h>
#include <steam/steam_gameserver.h>
#include <ISmmPlugin.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *KZ_API_URL  = "https://api.cs2kz.org/maps";
static const int   KZ_PAGE_LIMIT = 100;
static const int   MAX_MAPS      = 2000;

// ============================================================
// Module-level map pool (committed after a full successful fetch)
// ============================================================

static char     s_mapNames[MAX_MAPS][128];
static uint64_t s_mapIDs[MAX_MAPS];   // workshop_id
static int      s_mapCount = 0;

// Scratch buffers filled across pages before committing
static char     s_pendingNames[MAX_MAPS][128];
static uint64_t s_pendingIDs[MAX_MAPS];
static int      s_pendingCount  = 0;
static int      s_totalExpected = 0;   // "total" field from first page

static bool g_apiReady        = false;
static bool g_fetchInProgress = false;

// ============================================================
// Minimal JSON helpers
// ============================================================

// Reads a JSON string starting just after the opening '"'.
static bool ReadJsonString(const char *p, char *out, int outLen)
{
    int i = 0;
    while (*p && *p != '"' && i < outLen - 1)
    {
        if (*p == '\\') p++; // skip escape prefix
        if (*p) out[i++] = *p++;
    }
    out[i] = '\0';
    return *p == '"';
}

// Finds "key": at depth 0 inside a JSON object body, correctly skipping
// nested objects, arrays, and string literals.
// Returns pointer to the value (after colon + whitespace), or nullptr.
static const char *FindKeyAtDepth0(const char *body, const char *key)
{
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    int patLen = (int)strlen(pattern);

    const char *p = body;
    int depth = 0;

    while (*p)
    {
        if (*p == '{' || *p == '[') { depth++; p++; continue; }
        if (*p == '}' || *p == ']') { depth--; p++; continue; }

        if (*p == '"')
        {
            if (depth == 0 && strncmp(p, pattern, patLen) == 0)
            {
                const char *val = p + patLen;
                while (*val == ' ' || *val == '\t' || *val == '\n' || *val == '\r') val++;
                return val;
            }
            // Skip past the string literal
            p++;
            while (*p && *p != '"')
            {
                if (*p == '\\') p++;
                if (*p) p++;
            }
            if (*p) p++;
            continue;
        }
        p++;
    }
    return nullptr;
}

// Parses the "total" integer from the root response object.
static int ParseTotal(const char *body)
{
    const char *p = FindKeyAtDepth0(body, "total");
    if (!p) return 0;
    return atoi(p);
}

// Iterates the "values" array and appends approved maps to s_pending*.
static void ParseMapEntries(const char *body)
{
    const char *valuesKey = strstr(body, "\"values\":");
    if (!valuesKey) return;

    const char *arr = strchr(valuesKey + 9, '[');
    if (!arr) return;
    arr++;

    const char *p = arr;
    while (s_pendingCount < MAX_MAPS)
    {
        while (*p && *p != '{' && *p != ']') p++;
        if (!*p || *p == ']') break;

        const char *objStart = p + 1;

        // Capture the object body, tracking depth and skipping string literals
        char objBuf[8192];
        int  len   = 0;
        int  depth = 1;
        const char *q = objStart;
        while (*q && depth > 0 && len < (int)sizeof(objBuf) - 1)
        {
            if (*q == '{') depth++;
            else if (*q == '}') { depth--; if (depth == 0) break; }
            else if (*q == '"')
            {
                objBuf[len++] = *q++;
                while (*q && *q != '"' && len < (int)sizeof(objBuf) - 1)
                {
                    if (*q == '\\') { objBuf[len++] = *q++; if (!*q) break; }
                    objBuf[len++] = *q++;
                }
                if (*q && len < (int)sizeof(objBuf) - 1) objBuf[len++] = *q++;
                continue;
            }
            objBuf[len++] = *q++;
        }
        objBuf[len] = '\0';
        p = q;

        // Filter: keep only state == "approved"
        const char *stateP = FindKeyAtDepth0(objBuf, "state");
        if (!stateP || *stateP != '"') continue;
        stateP++;
        char stateVal[32];
        ReadJsonString(stateP, stateVal, sizeof(stateVal));
        if (strcmp(stateVal, "approved") != 0) continue;

        // Extract name
        const char *nameP = FindKeyAtDepth0(objBuf, "name");
        if (!nameP || *nameP != '"') continue;
        nameP++;
        char name[128];
        ReadJsonString(nameP, name, sizeof(name));
        if (name[0] == '\0') continue;

        // Extract workshop_id (stored as a number)
        const char *widP = FindKeyAtDepth0(objBuf, "workshop_id");
        if (!widP) continue;
        uint64_t workshopId = (uint64_t)strtoull(widP, nullptr, 10);

        strncpy(s_pendingNames[s_pendingCount], name, 127);
        s_pendingNames[s_pendingCount][127] = '\0';
        s_pendingIDs[s_pendingCount] = workshopId;
        s_pendingCount++;
    }
}

// Forward declaration
static void EnsureMapsDownloaded();

// ============================================================
// HTTP loader
// ============================================================

class CMapchooserLoader
{
public:
    void StartFetch();

private:
    void RequestPage(int offset);
    void OnHTTPResponse(HTTPRequestCompleted_t *pResult, bool bIOFailure);

    CCallResult<CMapchooserLoader, HTTPRequestCompleted_t> m_call;
    HTTPRequestHandle m_handle = INVALID_HTTPREQUEST_HANDLE;
    int m_offset = 0;
};

static CMapchooserLoader g_apiLoader;

void CMapchooserLoader::StartFetch()
{
    s_pendingCount  = 0;
    s_totalExpected = 0;
    m_offset        = 0;
    RequestPage(0);
}

void CMapchooserLoader::RequestPage(int offset)
{
    ISteamHTTP *pHTTP = SteamGameServerHTTP();
    if (!pHTTP)
    {
        META_CONPRINTF("[Mapchooser] SteamGameServerHTTP() unavailable\n");
        g_fetchInProgress = false;
        return;
    }

    m_handle = pHTTP->CreateHTTPRequest(k_EHTTPMethodGET, KZ_API_URL);
    if (m_handle == INVALID_HTTPREQUEST_HANDLE)
    {
        META_CONPRINTF("[Mapchooser] CreateHTTPRequest failed\n");
        g_fetchInProgress = false;
        return;
    }

    char offsetStr[32], limitStr[32];
    snprintf(offsetStr, sizeof(offsetStr), "%d", offset);
    snprintf(limitStr,  sizeof(limitStr),  "%d", KZ_PAGE_LIMIT);
    pHTTP->SetHTTPRequestGetOrPostParameter(m_handle, "offset", offsetStr);
    pHTTP->SetHTTPRequestGetOrPostParameter(m_handle, "limit",  limitStr);

    SteamAPICall_t call;
    if (!pHTTP->SendHTTPRequest(m_handle, &call))
    {
        META_CONPRINTF("[Mapchooser] SendHTTPRequest failed\n");
        pHTTP->ReleaseHTTPRequest(m_handle);
        m_handle = INVALID_HTTPREQUEST_HANDLE;
        g_fetchInProgress = false;
        return;
    }

    m_call.Set(call, this, &CMapchooserLoader::OnHTTPResponse);
    META_CONPRINTF("[Mapchooser] Fetching maps (offset=%d, limit=%d)...\n", offset, KZ_PAGE_LIMIT);
}

void CMapchooserLoader::OnHTTPResponse(HTTPRequestCompleted_t *pResult, bool bIOFailure)
{
    ISteamHTTP *pHTTP = SteamGameServerHTTP();

    auto scheduleRetry = [this]() {
        if (m_handle != INVALID_HTTPREQUEST_HANDLE)
        {
            if (ISteamHTTP *h = SteamGameServerHTTP()) h->ReleaseHTTPRequest(m_handle);
            m_handle = INVALID_HTTPREQUEST_HANDLE;
        }
        g_fetchInProgress = false;
        StartTimer([]() -> double {
            g_fetchInProgress = true;
            g_apiLoader.StartFetch();
            return -1.0;
        }, 30.0, false);
    };

    if (bIOFailure || !pResult->m_bRequestSuccessful || !pHTTP)
    {
        META_CONPRINTF("[Mapchooser] HTTP request failed (io=%d ok=%d), retrying in 30s...\n",
            (int)bIOFailure, (int)pResult->m_bRequestSuccessful);
        scheduleRetry();
        return;
    }

    if (pResult->m_eStatusCode < 200 || pResult->m_eStatusCode >= 300)
    {
        META_CONPRINTF("[Mapchooser] HTTP status %d, retrying in 30s...\n", (int)pResult->m_eStatusCode);
        scheduleRetry();
        return;
    }

    uint32 bodySize = 0;
    pHTTP->GetHTTPResponseBodySize(m_handle, &bodySize);

    char *body = new char[bodySize + 1];
    pHTTP->GetHTTPResponseBodyData(m_handle, (uint8 *)body, bodySize);
    body[bodySize] = '\0';

    pHTTP->ReleaseHTTPRequest(m_handle);
    m_handle = INVALID_HTTPREQUEST_HANDLE;

    if (m_offset == 0)
        s_totalExpected = ParseTotal(body);

    ParseMapEntries(body);
    delete[] body;

    META_CONPRINTF("[Mapchooser] Page offset=%d done: %d approved maps so far, total=%d entries\n",
        m_offset, s_pendingCount, s_totalExpected);

    m_offset += KZ_PAGE_LIMIT;

    // Fetch next page if there are more entries
    if (m_offset < s_totalExpected && s_pendingCount < MAX_MAPS)
    {
        RequestPage(m_offset);
        return;
    }

    // All pages received — commit results
    s_mapCount = s_pendingCount;
    for (int i = 0; i < s_mapCount; i++)
    {
        strncpy(s_mapNames[i], s_pendingNames[i], 127);
        s_mapNames[i][127] = '\0';
        s_mapIDs[i] = s_pendingIDs[i];
    }

    const char *ptrs[MAX_MAPS];
    for (int i = 0; i < s_mapCount; i++)
        ptrs[i] = s_mapNames[i];

    MapPool_SetFromWorkshop(ptrs, s_mapCount);
    META_CONPRINTF("[Mapchooser] Map pool set: %d approved maps from cs2kz API.\n", s_mapCount);

    EnsureMapsDownloaded();

    g_apiReady        = true;
    g_fetchInProgress = false;
}

// ============================================================
// Pre-download all maps in the pool
// ============================================================

static void EnsureMapsDownloaded()
{
    ISteamUGC *pUGC = SteamGameServerUGC();
    if (!pUGC)
        return;

    int started = 0;
    for (int i = 0; i < s_mapCount; i++)
    {
        PublishedFileId_t id = (PublishedFileId_t)s_mapIDs[i];
        if (!id)
            continue;

        uint32 state = pUGC->GetItemState(id);

        if (state & k_EItemStateInstalled)
            continue; // already on disk

        if (state & k_EItemStateDownloading)
            continue; // already in flight

        if (pUGC->DownloadItem(id, false))
            started++;
        else
            META_CONPRINTF("[Mapchooser] DownloadItem failed for map '%s' (%llu)\n",
                s_mapNames[i], (unsigned long long)id);
    }

    if (started > 0)
        META_CONPRINTF("[Mapchooser] Started background download for %d map(s).\n", started);
}

// ============================================================
// Workshop ID lookup
// ============================================================

uint64_t Workshop_GetMapID(const char *mapName)
{
    for (int i = 0; i < s_mapCount; i++)
        if (V_stricmp(s_mapNames[i], mapName) == 0)
            return s_mapIDs[i];
    return 0;
}

// ============================================================
// Public API
// ============================================================

bool Workshop_IsReady()
{
    return g_apiReady;
}

void Workshop_TryLoad()
{
    if (g_fetchInProgress)
        return;

    if (!SteamGameServerHTTP())
        return;

    g_fetchInProgress = true;
    g_apiLoader.StartFetch();
}

void Workshop_OnSteamAPIActivated()
{
    g_apiReady        = false;
    g_fetchInProgress = false;
    Workshop_TryLoad();

    // Refresh the map pool every 20 minutes
    StartTimer([]() -> double {
        if (!g_fetchInProgress)
        {
            g_fetchInProgress = true;
            g_apiLoader.StartFetch();
        }
        return 1200.0;
    }, 1200.0, true /*persistent*/, true /*useRealTime*/);
}
