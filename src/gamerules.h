#pragma once

#include "schema.h"
#include "entity2/entitysystem.h"
#include "entityinstance.h"

// Minimal CBaseEntity - only what we need for gamerules access
class CBaseEntity : public CEntityInstance
{
public:
	DECLARE_SCHEMA_CLASS_ENTITY(CBaseEntity)
};

class CGameRules
{
public:
	DECLARE_SCHEMA_CLASS_ENTITY(CGameRules)
};

class CCSGameRules : public CGameRules
{
public:
	DECLARE_SCHEMA_CLASS_ENTITY(CCSGameRules)

	SCHEMA_FIELD(bool, m_bGameRestart)
	SCHEMA_FIELD(GameTime_t, m_fRoundStartTime)
	SCHEMA_FIELD(GameTime_t, m_flGameStartTime)
	SCHEMA_FIELD(int, m_iRoundWinStatus)
	SCHEMA_FIELD(int, m_iRoundTime)
	SCHEMA_FIELD(GameTime_t, m_flIntermissionStartTime)
};

class CCSGameRulesProxy : public CBaseEntity
{
public:
	DECLARE_SCHEMA_CLASS_ENTITY(CCSGameRulesProxy)

	SCHEMA_FIELD(CCSGameRules *, m_pGameRules)
};

// Returns the active CCSGameRules, or nullptr if unavailable
CCSGameRules *GetGameRules();
