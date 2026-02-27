#include "ctimer.h"
#include <eiface.h>
#include <tier0/platform.h>

CUtlVector<CTimerBase *> g_NonPersistentTimers;
CUtlVector<CTimerBase *> g_PersistentTimers;

// Returns the current time for the given timer type, or -1 if unavailable.
static double GetTimerCurrentTime(bool useRealTime)
{
	if (useRealTime)
	{
		return Plat_FloatTime();
	}
	CGlobalVars *globals = g_pEngineServer ? g_pEngineServer->GetServerGlobals() : nullptr;
	return globals ? (double)globals->curtime : -1.0;
}

static void ProcessTimerList(CUtlVector<CTimerBase *> &timers)
{
	for (int i = timers.Count() - 1; i >= 0; i--)
	{
		CTimerBase *timer = timers[i];
		double currentTime = GetTimerCurrentTime(timer->useRealTime);

		// Game time unavailable (server not yet initialized), skip this frame.
		if (currentTime < 0)
			continue;

		if (timer->lastExecute == -1)
		{
			timer->lastExecute = currentTime;
		}

		if (timer->lastExecute + timer->interval <= currentTime)
		{
			if (!timer->Execute())
			{
				delete timer;
				timers.Remove(i);
			}
			else
			{
				timer->lastExecute = currentTime;
			}
		}
	}
}

void ProcessTimers()
{
	ProcessTimerList(g_PersistentTimers);
	ProcessTimerList(g_NonPersistentTimers);
}

void RemoveNonPersistentTimers()
{
	g_NonPersistentTimers.PurgeAndDeleteElements();
}
