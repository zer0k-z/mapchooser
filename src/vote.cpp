#include "vote.h"
#include "print.h"
#include "players.h"
#include "ctimer.h"
#include <string.h>
#include <stdio.h>

static bool g_voteActive = false;
static VoteOption g_voteOptions[MAX_VOTE_OPTIONS];
static int g_numVoteOptions = 0;
static int g_firstIdx = 0; // first valid option index (!0 reserved for extend when firstIdx=1)
static int g_playerVote[MAX_PLAYERS]; // -1 = not voted
static int g_voteCounts[MAX_VOTE_OPTIONS];
static VoteEndCallback_t g_voteEndCallback = nullptr;
static CTimerBase *g_voteTimer = nullptr;

// Forward declaration
static void EndVote();

bool IsVoteActive()
{
    return g_voteActive;
}

static void PrintVoteToAll()
{
    char line[256];

    int totalVoted = 0;
    for (int i = 0; i < MAX_PLAYERS; i++)
        if (IsRealPlayer(i) && g_playerVote[i] >= 0) totalVoted++;
    int totalReal = GetRealPlayerCount();

    PrintChatAll(" \x04[Vote]\x01 (%d/%d voted) Options:", totalVoted, totalReal);
    for (int i = g_firstIdx; i < g_numVoteOptions; i++)
    {
        snprintf(line, sizeof(line), "  \x05!%d\x01 %s", i, g_voteOptions[i].name);
        PrintChatAll(line);
    }
}

static void TryFinishEarly()
{
    if (!g_voteActive)
        return;

    // Check if all real players have voted
    int totalReal = GetRealPlayerCount();
    if (totalReal == 0)
    {
        EndVote();
        return;
    }

    int voted = 0;
    for (int i = 0; i < MAX_PLAYERS; i++)
        if (IsRealPlayer(i) && g_playerVote[i] >= 0) voted++;

    if (voted >= totalReal)
        EndVote();
}

static void EndVote()
{
    if (!g_voteActive)
        return;

    g_voteActive = false;

    // Tally
    int winnerIdx = -1;
    int winnerCount = 0;
    for (int i = 0; i < g_numVoteOptions; i++)
    {
        if (g_voteCounts[i] > winnerCount)
        {
            winnerCount = g_voteCounts[i];
            winnerIdx = i;
        }
    }

    // Print results
    PrintChatAll("\x04[Vote]\x01 Results:");
    for (int i = g_firstIdx; i < g_numVoteOptions; i++)
    {
        char line[256];
        snprintf(line, sizeof(line), "  !%d %s - \x05%d\x01 vote(s)%s",
            i, g_voteOptions[i].name, g_voteCounts[i],
            (i == winnerIdx && winnerCount > 0) ? " \x06[WINNER]" : "");
        PrintChatAll(line);
    }

    if (winnerIdx >= 0 && winnerCount > 0)
    {
        char announce[256];
        snprintf(announce, sizeof(announce), "\x04[Vote]\x01 Winner: \x05%s\x01", g_voteOptions[winnerIdx].name);
        PrintChatAll(announce);
    }
    else
    {
        PrintChatAll("\x04[Vote]\x01 No winner (no votes cast).");
    }

    VoteEndCallback_t cb = g_voteEndCallback;
    g_voteEndCallback = nullptr;
    g_voteTimer = nullptr;

    if (cb)
        cb(winnerCount > 0 ? winnerIdx : -1, g_voteCounts, g_numVoteOptions);
}

bool StartVote(const char *title, VoteOption *options, int numOptions, float duration, VoteEndCallback_t onEnd, int firstIdx)
{
    if (g_voteActive)
        return false;

    if (numOptions < 1 || numOptions > MAX_VOTE_OPTIONS)
        return false;

    g_voteActive = true;
    g_numVoteOptions = numOptions;
    g_firstIdx = firstIdx;
    g_voteEndCallback = onEnd;

    for (int i = 0; i < numOptions; i++)
        g_voteOptions[i] = options[i];

    for (int i = 0; i < MAX_PLAYERS; i++)
        g_playerVote[i] = -1;

    for (int i = 0; i < MAX_VOTE_OPTIONS; i++)
        g_voteCounts[i] = 0;

    // Announce
    char header[256];
    snprintf(header, sizeof(header), "\x04[Vote]\x01 %s (%.0fs to vote)", title, duration);
    PrintChatAll(header);
    PrintVoteToAll();

    // Start timeout timer
    g_voteTimer = StartTimer([duration]() -> double {
        EndVote();
        g_voteTimer = nullptr;
        return -1.0;
    }, (double)duration, false);

    return true;
}

void Vote_HandleDigit(int slot, int digit)
{
    if (!g_voteActive)
        return;

    if (!IsRealPlayer(slot))
        return;

    if (digit < g_firstIdx || digit >= g_numVoteOptions)
    {
        PrintChatToSlot(slot, "\x02[Vote]\x01 Invalid option. Type !%d - !%d.", g_firstIdx, g_numVoteOptions - 1);
        return;
    }

    // Remove previous vote if any
    int prev = g_playerVote[slot];
    if (prev >= 0)
        g_voteCounts[prev]--;

    g_playerVote[slot] = digit;
    g_voteCounts[digit]++;

    char confirm[256];
    snprintf(confirm, sizeof(confirm), "\x04[Vote]\x01 Your vote: \x05%s\x01", g_voteOptions[digit].name);
    PrintChatToSlot(slot, confirm);

    TryFinishEarly();
}

void Vote_OnClientDisconnect(int slot)
{
    if (!g_voteActive)
        return;

    // Remove their vote
    int prev = g_playerVote[slot];
    if (prev >= 0)
    {
        g_voteCounts[prev]--;
        g_playerVote[slot] = -1;
    }

    // players.h will mark them as disconnected before we recheck
    TryFinishEarly();
}
