#include "gamerules.h"
#include "entity2/entitysystem.h"

CCSGameRules *GetGameRules()
{
	EntityInstanceByClassIter_t iter("cs_gamerules");
	CCSGameRulesProxy *proxy = static_cast<CCSGameRulesProxy *>(iter.First());

	if (!proxy)
	{
		return nullptr;
	}

	return proxy->m_pGameRules();
}
