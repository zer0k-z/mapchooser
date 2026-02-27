#pragma once

#include <ISmmPlugin.h>
#include <functional>
#include <tier1/utlvector.h>

// Credit to Szwagi (original implementation in cs2kz-metamod)

class CTimerBase
{
public:
	CTimerBase(double initialInterval, bool useRealTime)
		: interval(initialInterval), useRealTime(useRealTime) {}

	virtual bool Execute() = 0;

	double interval {};
	double lastExecute = -1;
	bool useRealTime {};
};

void ProcessTimers();
void RemoveNonPersistentTimers();

extern CUtlVector<CTimerBase *> g_NonPersistentTimers;
extern CUtlVector<CTimerBase *> g_PersistentTimers;

// CTimer takes a std::function returning the next interval in seconds.
// Return <= 0 to stop the timer.
class CTimer : public CTimerBase
{
public:
	using Fn = std::function<double()>;

	CTimer(double initialDelay, bool useRealTime, Fn fn)
		: CTimerBase(initialDelay, useRealTime), m_fn(fn) {}

	bool Execute() override
	{
		interval = m_fn();
		return interval > 0;
	}

private:
	Fn m_fn;
};

// Start a timer. Returns a pointer to the timer (do NOT delete it yourself).
// preserveMapChange = true: timer survives map changes
// useRealTime = true: uses wall clock time instead of game time
inline CTimer *StartTimer(CTimer::Fn fn, double initialDelay, bool preserveMapChange = true, bool useRealTime = false)
{
	auto *timer = new CTimer(initialDelay, useRealTime, fn);
	if (preserveMapChange)
		g_PersistentTimers.AddToTail(timer);
	else
		g_NonPersistentTimers.AddToTail(timer);
	return timer;
}

// Stop and destroy a timer early (optional — timers self-destruct when fn returns <= 0).
inline void KillTimer(CTimerBase *timer)
{
	int idx = g_PersistentTimers.Find(timer);
	if (idx != -1)
	{
		g_PersistentTimers.Remove(idx);
		delete timer;
		return;
	}
	idx = g_NonPersistentTimers.Find(timer);
	if (idx != -1)
	{
		g_NonPersistentTimers.Remove(idx);
		delete timer;
	}
}
