#pragma once

#define MAX_VOTE_OPTIONS 10

struct VoteOption
{
    char name[128];
};

// Callback: winnerIdx is -1 on no-vote / all abstained, voteCounts[i] for each option.
typedef void (*VoteEndCallback_t)(int winnerIdx, int *voteCounts, int numOptions);

// Returns true if a vote is currently running.
bool IsVoteActive();

// Start a vote. options[i] maps to !i. Returns false if a vote is already active.
// duration is in seconds. onEnd is called when the vote finishes.
// firstIdx: first valid option index displayed to players (use 1 to reserve !0 for extend).
bool StartVote(const char *title, VoteOption *options, int numOptions, float duration, VoteEndCallback_t onEnd, int firstIdx = 0);

// Called by CMD_WILDCARD handler for digit commands.
void Vote_HandleDigit(int slot, int digit);

// Called by plugin when a client disconnects.
void Vote_OnClientDisconnect(int slot);
