#include "CombatCommander.h"
#include "Util.h"
#include "CCBot.h"
#include <list>

const size_t IdlePriority = 0;
const size_t BackupPriority = 1;
const size_t HarassPriority = 2;
const size_t AttackPriority = 2;
const size_t ClearExpandPriority = 3;
const size_t ScoutPriority = 3;
const size_t BaseDefensePriority = 4;
const size_t ScoutDefensePriority = 5;
const size_t WorkerFleePriority = 5;
const size_t DropPriority = 5;

const float DefaultOrderRadius = 25;			//Order radius is the threat awareness range of units in the squad
const float WorkerRushDefenseOrderRadius = 250;
const float MainAttackOrderRadius = 15;
const float HarassOrderRadius = 25;
const float ScoutOrderRadius = 6;				//Small number to prevent the scout from targeting far units instead of going to the next base location
const float MainAttackMaxDistance = 20;			//Distance from the center of the Main Attack Squad for a unit to be considered in it
const float MainAttackMaxRegroupDuration = 100; //Max number of frames allowed for a regroup order
const float MainAttackRegroupCooldown = 200;	//Min number of frames required to wait between regroup orders
const float MainAttackMinRetreatDuration = 50;	//Max number of frames allowed for a regroup order

const size_t BLOCKED_TILES_UPDATE_FREQUENCY = 24;
const uint32_t WORKER_RUSH_DETECTION_COOLDOWN = 30 * 24;
const size_t MAX_DISTANCE_FROM_CLOSEST_BASE_FOR_WORKER_FLEE = 15;
const int ACTION_REEXECUTION_FREQUENCY = 50;

CombatCommander::CombatCommander(CCBot & bot)
    : m_bot(bot)
    , m_squadData(bot)
    , m_initialized(false)
    , m_attackStarted(false)
	, m_currentBaseExplorationIndex(0)
	, m_currentBaseScoutingIndex(0)
{
}

void CombatCommander::onStart()
{
	for (auto& ability : m_bot.Observation()->GetAbilityData())
	{
		m_abilityCastingRanges[ability.ability_id] = ability.cast_range;
	}

    m_squadData.clearSquadData();

	// the squad that consists of units waiting for the squad to be big enough to begin the main attack
	SquadOrder idleOrder(SquadOrderTypes::Idle, CCPosition(), DefaultOrderRadius, "Prepare for battle");
	m_squadData.addSquad("Idle", Squad("Idle", idleOrder, IdlePriority, m_bot));

	// the squad that consists of fleeing workers
	SquadOrder fleeOrder(SquadOrderTypes::Retreat, CCPosition(), DefaultOrderRadius, "Worker flee");
	m_squadData.addSquad("WorkerFlee", Squad("WorkerFlee", fleeOrder, WorkerFleePriority, m_bot));

	// the harass attack squad that will pressure the enemy's main base workers
	SquadOrder harassOrder(SquadOrderTypes::Harass, CCPosition(0, 0), HarassOrderRadius, "Harass");
	m_squadData.addSquad("Harass1", Squad("Harass1", harassOrder, HarassPriority, m_bot));

    // the main attack squad that will pressure the enemy's closest base location
    SquadOrder mainAttackOrder(SquadOrderTypes::Attack, CCPosition(0, 0), MainAttackOrderRadius, "Attack");
    m_squadData.addSquad("MainAttack", Squad("MainAttack", mainAttackOrder, MainAttackMaxRegroupDuration, MainAttackRegroupCooldown, MainAttackMinRetreatDuration, MainAttackMaxDistance, AttackPriority, m_bot));

    // the backup squad that will send reinforcements to the main attack squad
    SquadOrder backupSquadOrder(SquadOrderTypes::Attack, CCPosition(0, 0), DefaultOrderRadius, "Send backups");
    m_squadData.addSquad("Backup", Squad("Backup", backupSquadOrder, BackupPriority, m_bot));

    // the scout defense squad will handle chasing the enemy worker scout
	// the -5 is to prevent enemy workers (during worker rush) to get outside the base defense range
    SquadOrder enemyScoutDefense(SquadOrderTypes::Defend, m_bot.GetStartLocation(), DefaultOrderRadius - 5, "Chase scout");
    m_squadData.addSquad("ScoutDefense", Squad("ScoutDefense", enemyScoutDefense, ScoutDefensePriority, m_bot));

	SquadOrder scoutOrder(SquadOrderTypes::Scout, CCPosition(), ScoutOrderRadius, "Scouting for new bases");
	m_squadData.addSquad("Scout", Squad("Scout", scoutOrder, ScoutPriority, m_bot));

	//The influence maps are initialised earlier so we can use the blocked tiles influence map to place the turrets
}

bool CombatCommander::isSquadUpdateFrame()
{
    return true;
}

void CombatCommander::clearYamatoTargets()
{
	for(auto it = m_yamatoTargets.begin(); it != m_yamatoTargets.end();)
	{
		auto & targetPair = *it;
		const auto targetTag = targetPair.first;
		const auto target = m_bot.Observation()->GetUnit(targetTag);
		if (!target || !target->is_alive)
		{
			it = m_yamatoTargets.erase(it);
			continue;
		}

		auto & battlecruiserPairs = targetPair.second;
		for (auto it2 = battlecruiserPairs.begin(); it2 != battlecruiserPairs.end();)
		{
			const auto & battlecruiserPair = *it2;
			const auto battlecruiserTag = battlecruiserPair.first;
			const auto battlecruiser = m_bot.Observation()->GetUnit(battlecruiserTag);
			const auto finishFrame = battlecruiserPair.second;
			if(!battlecruiser || !battlecruiser->is_alive || m_bot.GetCurrentFrame() >= finishFrame)
			{
				it2 = battlecruiserPairs.erase(it2);
				continue;
			}
			
			++it2;
		}

		if (battlecruiserPairs.empty())
		{
			it = m_yamatoTargets.erase(it);
			continue;
		}

		++it;
	}
}

void CombatCommander::onFrame(const std::vector<Unit> & combatUnits)
{
    if (!m_attackStarted)
    {
        m_attackStarted = shouldWeStartAttacking();
    }

	m_logVikingActions = false;

	CleanActions(combatUnits);

	clearYamatoTargets();

    m_combatUnits = combatUnits;

	sc2::Units units;
	Util::CCUnitsToSc2Units(combatUnits, units);
	m_unitsAbilities = m_bot.Query()->GetAbilitiesForUnits(units);

	m_bot.StartProfiling("0.10.4.0    updateInfluenceMaps");
	updateInfluenceMaps();
	m_bot.StopProfiling("0.10.4.0    updateInfluenceMaps");

	m_bot.StartProfiling("0.10.4.1    CalcBestFlyingCycloneHelpers");
	CalcBestFlyingCycloneHelpers();
	m_bot.StopProfiling("0.10.4.1    CalcBestFlyingCycloneHelpers");

	updateIdlePosition();

	m_bot.StartProfiling("0.10.4.2    updateSquads");
    if (isSquadUpdateFrame())
    {
		updateIdleSquad();
		updateBackupSquads();
		updateWorkerFleeSquad();
        updateScoutDefenseSquad();
		m_bot.StartProfiling("0.10.4.2.1    updateDefenseBuildings");
		updateDefenseBuildings();
		m_bot.StopProfiling("0.10.4.2.1    updateDefenseBuildings");
		m_bot.StartProfiling("0.10.4.2.2    updateDefenseSquads");
        updateDefenseSquads();
		m_bot.StopProfiling("0.10.4.2.2    updateDefenseSquads");
		updateClearExpandSquads();
		updateScoutSquad();
		m_bot.StartProfiling("0.10.4.2.3    updateHarassSquads");
		updateHarassSquads();
		m_bot.StopProfiling("0.10.4.2.3    updateHarassSquads");
		updateAttackSquads();
    }
	drawCombatInformation();
	m_bot.StopProfiling("0.10.4.2    updateSquads");

	m_bot.StartProfiling("0.10.4.3    m_squadData.onFrame");
	m_squadData.onFrame();
	m_bot.StopProfiling("0.10.4.3    m_squadData.onFrame");

	ExecuteActions();

	m_bot.StartProfiling("0.10.4.4    lowPriorityCheck");
	lowPriorityCheck();
	m_bot.StopProfiling("0.10.4.4    lowPriorityCheck");
}

void CombatCommander::lowPriorityCheck()
{
	auto frame = m_bot.GetGameLoop();
	if (frame - m_lastLowPriorityFrame < 5)
	{
		return;
	}
	m_lastLowPriorityFrame = frame;

	std::vector<Unit> toRemove;
	for (auto sighting : m_invisibleSighting)
	{
		if (frame + FRAME_BEFORE_SIGHTING_INVALIDATED < sighting.second.second)
		{
			toRemove.push_back(sighting.first);
		}
	}
	for (auto unit : toRemove)
	{
		m_invisibleSighting.erase(unit);
	}
}

bool CombatCommander::shouldWeStartAttacking()
{
    //return m_bot.Strategy().getCurrentStrategy().m_attackCondition.eval();
	return true;
}

void CombatCommander::initInfluenceMaps()
{
	const size_t mapWidth = m_bot.Map().totalWidth();
	const size_t mapHeight = m_bot.Map().totalHeight();
	m_groundFromGroundCombatInfluenceMap.resize(mapWidth);
	m_groundFromAirCombatInfluenceMap.resize(mapWidth);
	m_airFromGroundCombatInfluenceMap.resize(mapWidth);
	m_airFromAirCombatInfluenceMap.resize(mapWidth);
	m_groundEffectInfluenceMap.resize(mapWidth);
	m_airEffectInfluenceMap.resize(mapWidth);
	m_groundFromGroundCloakedCombatInfluenceMap.resize(mapWidth);
	m_blockedTiles.resize(mapWidth);
	for(size_t x = 0; x < mapWidth; ++x)
	{
		auto& groundFromGroundInfluenceMapRow = m_groundFromGroundCombatInfluenceMap[x];
		auto& groundFromAirInfluenceMapRow = m_groundFromAirCombatInfluenceMap[x];
		auto& airFromGroundInfluenceMapRow = m_airFromGroundCombatInfluenceMap[x];
		auto& airFromAirInfluenceMapRow = m_airFromAirCombatInfluenceMap[x];
		auto& groundEffectInfluenceMapRow = m_groundEffectInfluenceMap[x];
		auto& airEffectInfluenceMapRow = m_airEffectInfluenceMap[x];
		auto& groundFromGroundCloakedCombatInfluenceMapRow = m_groundFromGroundCloakedCombatInfluenceMap[x];
		auto& blockedTilesRow = m_blockedTiles[x];
		groundFromGroundInfluenceMapRow.resize(mapHeight);
		groundFromAirInfluenceMapRow.resize(mapHeight);
		airFromGroundInfluenceMapRow.resize(mapHeight);
		airFromAirInfluenceMapRow.resize(mapHeight);
		groundEffectInfluenceMapRow.resize(mapHeight);
		airEffectInfluenceMapRow.resize(mapHeight);
		groundFromGroundCloakedCombatInfluenceMapRow.resize(mapHeight);
		blockedTilesRow.resize(mapHeight);
		for (size_t y = 0; y < mapHeight; ++y)
		{
			groundFromGroundInfluenceMapRow[y] = 0;
			groundFromAirInfluenceMapRow[y] = 0;
			airFromGroundInfluenceMapRow[y] = 0;
			airFromAirInfluenceMapRow[y] = 0;
			groundEffectInfluenceMapRow[y] = 0;
			airEffectInfluenceMapRow[y] = 0;
			groundFromGroundCloakedCombatInfluenceMapRow[y] = 0;
			blockedTilesRow[y] = false;
		}
	}
}

void CombatCommander::resetInfluenceMaps()
{
	const size_t mapWidth = m_bot.Map().totalWidth();
	const size_t mapHeight = m_bot.Map().totalHeight();
	const bool resetBlockedTiles = m_bot.GetGameLoop() - m_lastBlockedTilesResetFrame >= BLOCKED_TILES_UPDATE_FREQUENCY;
	if (resetBlockedTiles)
		m_lastBlockedTilesResetFrame = m_bot.GetGameLoop();
	for (size_t x = 0; x < mapWidth; ++x)
	{
		std::vector<float> & groundFromGroundInfluenceMap = m_groundFromGroundCombatInfluenceMap[x];
		std::vector<float> & groundFromAirInfluenceMap = m_groundFromAirCombatInfluenceMap[x];
		std::vector<float> & airFromGroundInfluenceMap = m_airFromGroundCombatInfluenceMap[x];
		std::vector<float> & airFromAirInfluenceMap = m_airFromAirCombatInfluenceMap[x];
		std::vector<float> & groundEffectInfluenceMap = m_groundEffectInfluenceMap[x];
		std::vector<float> & airEffectInfluenceMap = m_airEffectInfluenceMap[x];
		std::vector<float> & groundFromGroundCloakedCombatInfluenceMap = m_groundFromGroundCloakedCombatInfluenceMap[x];
		std::fill(groundFromGroundInfluenceMap.begin(), groundFromGroundInfluenceMap.end(), 0.f);
		std::fill(groundFromAirInfluenceMap.begin(), groundFromAirInfluenceMap.end(), 0.f);
		std::fill(airFromGroundInfluenceMap.begin(), airFromGroundInfluenceMap.end(), 0.f);
		std::fill(airFromAirInfluenceMap.begin(), airFromAirInfluenceMap.end(), 0.f);
		std::fill(groundEffectInfluenceMap.begin(), groundEffectInfluenceMap.end(), 0.f);
		std::fill(airEffectInfluenceMap.begin(), airEffectInfluenceMap.end(), 0.f);
		std::fill(groundFromGroundCloakedCombatInfluenceMap.begin(), groundFromGroundCloakedCombatInfluenceMap.end(), 0.f);

		if (resetBlockedTiles)
		{
			auto& blockedTilesRow = m_blockedTiles[x];
			for (size_t y = 0; y < mapHeight; ++y)
			{
				blockedTilesRow[y] = false;
			}
		}
	}
}

void CombatCommander::updateInfluenceMaps()
{
	m_bot.StartProfiling("0.10.4.0.1      resetInfluenceMaps");
	resetInfluenceMaps();
	m_bot.StopProfiling("0.10.4.0.1      resetInfluenceMaps");
	m_bot.StartProfiling("0.10.4.0.2      updateInfluenceMapsWithUnits");
	updateInfluenceMapsWithUnits();
	m_bot.StopProfiling("0.10.4.0.2      updateInfluenceMapsWithUnits");
	m_bot.StartProfiling("0.10.4.0.3      updateInfluenceMapsWithEffects");
	updateInfluenceMapsWithEffects();
	m_bot.StopProfiling("0.10.4.0.3      updateInfluenceMapsWithEffects");
	
	drawInfluenceMaps();	
	drawBlockedTiles();
}

void CombatCommander::updateInfluenceMapsWithUnits()
{
	const bool updateBlockedTiles = m_bot.GetGameLoop() - m_lastBlockedTilesUpdateFrame >= BLOCKED_TILES_UPDATE_FREQUENCY;
	if (updateBlockedTiles)
		m_lastBlockedTilesUpdateFrame = m_bot.GetGameLoop();
	for (auto& enemyUnit : m_bot.GetKnownEnemyUnits())
	{
		auto& enemyUnitType = enemyUnit.getType();
		if (enemyUnitType.isCombatUnit() || enemyUnitType.isWorker() || (enemyUnitType.isAttackingBuilding() && enemyUnit.getUnitPtr()->build_progress >= 1.f))
		{
			if (enemyUnit.getAPIUnitType() != sc2::UNIT_TYPEID::PROTOSS_PHOTONCANNON || enemyUnit.isPowered())
			{
				// Ignore influence of SCVs that are building
				if (enemyUnitType.getAPIUnitType() == sc2::UNIT_TYPEID::TERRAN_SCV && Util::Contains(enemyUnit.getUnitPtr(), m_bot.GetEnemySCVBuilders()))
					continue;
				if (enemyUnit.getAPIUnitType() == sc2::UNIT_TYPEID::TERRAN_KD8CHARGE || enemyUnit.getAPIUnitType() == sc2::UNIT_TYPEID::PROTOSS_DISRUPTORPHASED)
				{
					const float dps = Util::GetSpecialCaseDps(enemyUnit.getUnitPtr(), m_bot, sc2::Weapon::TargetType::Ground);
					const float radius = Util::GetSpecialCaseRange(enemyUnit.getAPIUnitType(), sc2::Weapon::TargetType::Ground);
					updateInfluenceMap(dps, radius, 1.f, enemyUnit.getPosition(), true, true, true, false);
				}
				else
				{
					updateGroundInfluenceMapForUnit(enemyUnit);
					updateAirInfluenceMapForUnit(enemyUnit);
				}
			}
		}
		if(updateBlockedTiles && enemyUnitType.isBuilding() && !enemyUnit.isFlying() && enemyUnit.getUnitPtr()->unit_type != sc2::UNIT_TYPEID::TERRAN_SUPPLYDEPOTLOWERED)
		{
			updateBlockedTilesWithUnit(enemyUnit);
		}
	}
	if(updateBlockedTiles)
	{
		for (auto& allyUnitPair : m_bot.GetAllyUnits())
		{
			auto& allyUnit = allyUnitPair.second;
			if (allyUnit.getType().isBuilding() && !allyUnit.isFlying() && allyUnit.getUnitPtr()->unit_type != sc2::UNIT_TYPEID::TERRAN_SUPPLYDEPOTLOWERED)
			{
				updateBlockedTilesWithUnit(allyUnit);
			}
		}
		updateBlockedTilesWithNeutral();
	}
}

void CombatCommander::updateInfluenceMapsWithEffects()
{
	m_enemyScans.clear();
	auto & effectDataVector = m_bot.Observation()->GetEffectData();
	for (auto & effect : m_bot.Observation()->GetEffects())
	{
		float radius, dps;
		sc2::Weapon::TargetType targetType;
		auto & effectData = effectDataVector[effect.effect_id];
		switch(effect.effect_id)
		{
			case 0: // Nothing
				continue;
			case 1:	// Psi Storm
				radius = effectData.radius;
				dps = 28.07f;	// 80 dmg over 2.85 sec
				targetType = sc2::Weapon::TargetType::Any;
				break;
			case 2:	// Guardian Shield
				radius = effectData.radius;
				//TODO consider it in power calculation
				continue;
			case 3:	// Temporal Field Growing (doesn't exist anymore)
			case 4:	// Temporal Field (doesn't exist anymore)
				continue;
			case 5:	// Thermal Lance (Colossus beams)
				radius = effectData.radius;
				dps = 18.7f;
				targetType = sc2::Weapon::TargetType::Ground;
				break;
			case 6:	// Scanner Sweep
				for (const auto & pos : effect.positions)
					m_enemyScans.push_back(pos);
				continue;
			case 7: // Nuke Dot
				radius = 8.f;
				dps = 300.f;	// 300 dmg one shot
				targetType = sc2::Weapon::TargetType::Any;
				break;
			case 8: // Liberator Defender Zone Setup
			case 9: // Liberator Defender Zone
				radius = effectData.radius;
				dps = 65.8f;
				targetType = sc2::Weapon::TargetType::Ground;
				break;
			case 10: // Blinding Cloud
				radius = effectData.radius;
				dps = 25.f;		// this effect does no damage, but we still want to go avoid it so we set a high dps
				targetType = sc2::Weapon::TargetType::Ground;
				break;
			case 11: // Corrosive Bile
				radius = effectData.radius;
				dps = 60.f;		// 60 dmg one shot
				targetType = sc2::Weapon::TargetType::Any;
				break;
			case 12: // Lurker Spines
				radius = effectData.radius;
				dps = 20.f;		// 20 dmg one shot
				targetType = sc2::Weapon::TargetType::Ground;
				break;
			default:
				continue;
			//TODO The following effects are not part of the list and should be managed elsewhere if possible
			/*case static_cast<const unsigned>(sc2::ABILITY_ID::EFFECT_PARASITICBOMB) :
				radius = 3.f;
				dps = 17.14f;	// 120 dmg over 7 sec
				targetType = sc2::Weapon::TargetType::Air;
				break;
			case static_cast<const unsigned>(sc2::ABILITY_ID::EFFECT_PURIFICATIONNOVA) :
				radius = 1.5f;
				dps = 145.f;	// 145 dmg on shot
				targetType = sc2::Weapon::TargetType::Ground;
				break;
			case static_cast<const unsigned>(sc2::ABILITY_ID::EFFECT_TIMEWARP) :
				radius = 3.5f;
				dps = 25.f;		// this effect does no damage, but we still want to go avoid it so we set a high dps
				targetType = sc2::Weapon::TargetType::Ground;
				break;
			case static_cast<const unsigned>(sc2::ABILITY_ID::EFFECT_WIDOWMINEATTACK) :
				radius = 1.5f;
				dps = 40.f;
				targetType = sc2::Weapon::TargetType::Any;
				break;*/
		}
		if (radius > 0)
		{
			radius += 1;	// just a buffer to prevent our units to push the others into the effect's range
			for (auto & pos : effect.positions)
			{
				if (targetType == sc2::Weapon::TargetType::Any || targetType == sc2::Weapon::TargetType::Air)
					updateInfluenceMap(dps, radius, 1.f, pos, false, true, true, false);
				if (targetType == sc2::Weapon::TargetType::Any || targetType == sc2::Weapon::TargetType::Ground)
					updateInfluenceMap(dps, radius, 1.f, pos, true, true, true, false);
			}
		}
	}
}

void CombatCommander::updateGroundInfluenceMapForUnit(const Unit& enemyUnit)
{
	updateInfluenceMapForUnit(enemyUnit, true);
}

void CombatCommander::updateAirInfluenceMapForUnit(const Unit& enemyUnit)
{
	updateInfluenceMapForUnit(enemyUnit, false);
}

void CombatCommander::updateInfluenceMapForUnit(const Unit& enemyUnit, const bool ground)
{
	const float dps = ground ? Util::GetGroundDps(enemyUnit.getUnitPtr(), m_bot) : Util::GetAirDps(enemyUnit.getUnitPtr(), m_bot);
	if (dps == 0.f)
		return;
	float range = ground ? Util::GetGroundAttackRange(enemyUnit.getUnitPtr(), m_bot) : Util::GetAirAttackRange(enemyUnit.getUnitPtr(), m_bot);
	if (range == 0.f)
		return;
	if (!ground && enemyUnit.getAPIUnitType() == sc2::UNIT_TYPEID::PROTOSS_TEMPEST)
		range += 2;
	const float speed = std::max(2.5f, Util::getSpeedOfUnit(enemyUnit.getUnitPtr(), m_bot));
	updateInfluenceMap(dps, range, speed, enemyUnit.getPosition(), ground, !enemyUnit.isFlying(), false, enemyUnit.getUnitPtr()->cloak == sc2::Unit::Cloaked);
}

void CombatCommander::updateInfluenceMap(float dps, float range, float speed, const CCPosition & position, bool ground, bool fromGround, bool effect, bool cloaked)
{
	const float totalRange = range + speed;

	const float fminX = floor(position.x - totalRange);
	const float fmaxX = ceil(position.x + totalRange);
	const float fminY = floor(position.y - totalRange);
	const float fmaxY = ceil(position.y + totalRange);
	const float minMapX = m_bot.Map().mapMin().x;
	const float minMapY = m_bot.Map().mapMin().y;
	const float maxMapX = m_bot.Map().mapMax().x;
	const float maxMapY = m_bot.Map().mapMax().y;
	const int minX = std::max(minMapX, fminX);
	const int maxX = std::min(maxMapX, fmaxX);
	const int minY = std::max(minMapY, fminY);
	const int maxY = std::min(maxMapY, fmaxY);
	auto& influenceMap = ground ? (effect ? m_groundEffectInfluenceMap : (fromGround ? m_groundFromGroundCombatInfluenceMap : m_groundFromAirCombatInfluenceMap)) : (effect ? m_airEffectInfluenceMap : (fromGround ? m_airFromGroundCombatInfluenceMap : m_airFromAirCombatInfluenceMap));
	//loop for a square of size equal to the diameter of the influence circle
	for (int x = minX; x < maxX; ++x)
	{
		for (int y = minY; y < maxY; ++y)
		{
			const float distance = Util::Dist(position, CCPosition(x + 0.5f, y + 0.5f));
			float multiplier = 1.f;
			if (distance > range)
				multiplier = std::max(0.f, (speed - (distance - range)) / speed);	//value is linearly interpolated in the speed buffer zone
			influenceMap[x][y] += dps * multiplier;
			if (fromGround && cloaked)
				m_groundFromGroundCloakedCombatInfluenceMap[x][y] += dps * multiplier;
		}
	}
}

void CombatCommander::updateBlockedTilesWithUnit(const Unit& unit)
{
	CCTilePosition bottomLeft;
	CCTilePosition topRight;
	unit.getBuildingLimits(bottomLeft, topRight);

	for(int x = bottomLeft.x; x < topRight.x; ++x)
	{
		for(int y = bottomLeft.y; y < topRight.y; ++y)
		{
			m_blockedTiles[x][y] = true;
		}
	}
}

void CombatCommander::updateBlockedTilesWithNeutral()
{
	for (auto& neutralUnitPair : m_bot.GetNeutralUnits())
	{
		auto& neutralUnit = neutralUnitPair.second;
		updateBlockedTilesWithUnit(neutralUnit);
	}
}

void CombatCommander::drawInfluenceMaps()
{
#ifdef PUBLIC_RELEASE
	return;
#endif
	if (m_bot.Config().DrawInfluenceMaps)
	{
		m_bot.StartProfiling("0.10.4.0.4      drawInfluenceMaps");
		const size_t mapWidth = m_bot.Map().totalWidth();
		const size_t mapHeight = m_bot.Map().totalHeight();
		for (size_t x = 0; x < mapWidth; ++x)
		{
			auto& groundFromGroundInfluenceMapRow = m_groundFromGroundCombatInfluenceMap[x];
			auto& groundFromAirInfluenceMapRow = m_groundFromAirCombatInfluenceMap[x];
			auto& airFromGroundInfluenceMapRow = m_airFromGroundCombatInfluenceMap[x];
			auto& airFromAirInfluenceMapRow = m_airFromAirCombatInfluenceMap[x];
			auto& groundEffectInfluenceMapRow = m_groundEffectInfluenceMap[x];
			auto& airEffectInfluenceMapRow = m_airEffectInfluenceMap[x];
			for (size_t y = 0; y < mapHeight; ++y)
			{
				const float groundInfluence = groundFromGroundInfluenceMapRow[y] + groundFromAirInfluenceMapRow[y];
				const float airInfluence = airFromGroundInfluenceMapRow[y] + airFromAirInfluenceMapRow[y];
				if (groundInfluence > 0.f)
				{
					const float value = std::min(255.f, std::max(0.f, groundInfluence * 5));
					m_bot.Map().drawTile(x, y, CCColor(255, 255 - value, 0));	//yellow to red
				}
				if (airInfluence > 0.f)
				{
					const float value = std::min(255.f, std::max(0.f, airInfluence * 5));
					m_bot.Map().drawTile(x, y, CCColor(255, 255 - value, 0), 0.5f);	//yellow to red
				}
				if (groundEffectInfluenceMapRow[y] > 0.f)
				{
					const float value = std::min(255.f, std::max(0.f, groundEffectInfluenceMapRow[y] * 5));
					m_bot.Map().drawTile(x, y, CCColor(255 - value, value, 255), 0.7f);	//cyan to purple
				}
				if (airEffectInfluenceMapRow[y] > 0.f)
				{
					const float value = std::min(255.f, std::max(0.f, airEffectInfluenceMapRow[y] * 5));
					m_bot.Map().drawTile(x, y, CCColor(255 - value, value, 255), 0.4f);	//cyan to purple
				}
			}
		}
		m_bot.StopProfiling("0.10.4.0.4      drawInfluenceMaps");
	}
}

void CombatCommander::drawBlockedTiles()
{
#ifdef PUBLIC_RELEASE
	return;
#endif
	if (m_bot.Config().DrawBlockedTiles)
	{
		m_bot.StartProfiling("0.10.4.0.5      drawBlockedTiles");
		const size_t mapWidth = m_bot.Map().totalWidth();
		const size_t mapHeight = m_bot.Map().totalHeight();
		for (size_t x = 0; x < mapWidth; ++x)
		{
			auto& blockedTilesRow = m_blockedTiles[x];
			for (size_t y = 0; y < mapHeight; ++y)
			{
				if (blockedTilesRow[y])
					m_bot.Map().drawTile(x, y, sc2::Colors::Red);
			}
		}
		m_bot.StopProfiling("0.10.4.0.5      drawBlockedTiles");
	}
}

void CombatCommander::updateIdlePosition()
{
	if (m_bot.GetCurrentFrame() - m_lastIdlePositionUpdateFrame >= 24)	// Every second
	{
		m_lastIdlePositionUpdateFrame = m_bot.GetCurrentFrame();
		auto idlePosition = m_bot.GetStartLocation();
		const BaseLocation* farthestBase = m_bot.Bases().getFarthestOccupiedBaseLocation();
		if (farthestBase)
		{
			const auto vectorAwayFromBase = Util::Normalized(farthestBase->getResourceDepot().getPosition() - Util::GetPosition(farthestBase->getCenterOfMinerals()));
			idlePosition = farthestBase->getResourceDepot().getPosition() + vectorAwayFromBase * 5.f;
		}
		m_idlePosition = idlePosition;
	}
}

void CombatCommander::updateIdleSquad()
{
    Squad & idleSquad = m_squadData.getSquad("Idle");
    for (auto & unit : m_combatUnits)
    {
		if (unit.getAPIUnitType() == sc2::UNIT_TYPEID::TERRAN_BARRACKSFLYING)
			continue;
        // if it hasn't been assigned to a squad yet, put it in the low priority idle squad
        if (m_squadData.canAssignUnitToSquad(unit, idleSquad))
        {
            idleSquad.addUnit(unit);
        }
    }

	if (idleSquad.getUnits().empty())
		return;

	CleanActions(idleSquad.getUnits());

	for (auto & combatUnit : idleSquad.getUnits())
	{
		if (Util::DistSq(combatUnit, m_idlePosition) > 5.f * 5.f)
		{
			const auto action = RangedUnitAction(MicroActionType::Move, m_idlePosition, false, 0, "IdleMove");
			PlanAction(combatUnit.getUnitPtr(), action);
		}
	}
}

void CombatCommander::updateWorkerFleeSquad()
{
	Squad & workerFleeSquad = m_squadData.getSquad("WorkerFlee");
	for (auto & worker : m_bot.Workers().getWorkers())
	{
		const CCTilePosition tile = Util::GetTilePosition(worker.getPosition());
		const float groundInfluence = Util::PathFinding::GetCombatInfluenceOnTile(tile, worker.isFlying(), m_bot);
		bool fleeFromSlowThreats = false;
		if (groundInfluence > 0.f && !m_bot.Strategy().isWorkerRushed())
		{
			const auto & enemyUnits = m_bot.GetKnownEnemyUnits();
			fleeFromSlowThreats = !WorkerHasFastEnemyThreat(worker.getUnitPtr(), enemyUnits);
		}
		const bool flyingThreat = Util::PathFinding::HasCombatInfluenceOnTile(tile, worker.isFlying(), false, m_bot);
		const bool groundCloakedThreat = Util::PathFinding::HasGroundFromGroundCloakedInfluenceOnTile(tile, m_bot);
		const bool groundThreat = Util::PathFinding::HasCombatInfluenceOnTile(tile, worker.isFlying(), true, m_bot);
		const bool injured = worker.getHitPointsPercentage() < m_bot.Workers().MIN_HP_PERCENTAGE_TO_FIGHT * 100;
		const auto job = m_bot.Workers().getWorkerData().getWorkerJob(worker);
		const auto isProxyWorker = m_bot.Workers().getWorkerData().isProxyWorker(worker);
		// Check if the worker needs to flee (the last part is bad because workers sometimes need to mineral walk)
		if ((((flyingThreat && !groundThreat) || fleeFromSlowThreats || groundCloakedThreat) && job != WorkerJobs::Build && job != WorkerJobs::Repair)
			|| Util::PathFinding::HasEffectInfluenceOnTile(tile, worker.isFlying(), m_bot)
			|| (groundThreat && (injured || isProxyWorker) && job != WorkerJobs::Build && Util::DistSq(worker, Util::GetPosition(m_bot.Bases().getClosestBasePosition(worker.getUnitPtr(), Players::Self))) < MAX_DISTANCE_FROM_CLOSEST_BASE_FOR_WORKER_FLEE * MAX_DISTANCE_FROM_CLOSEST_BASE_FOR_WORKER_FLEE))
		{
			// Put it in the squad if it is not defending or already in the squad
			if (m_squadData.canAssignUnitToSquad(worker, workerFleeSquad))
			{
				m_bot.Workers().setCombatWorker(worker);
				workerFleeSquad.addUnit(worker);
			}
		}
		else
		{
			const auto squad = m_squadData.getUnitSquad(worker);
			if(squad != nullptr && squad == &workerFleeSquad)
			{
				m_bot.Workers().finishedWithWorker(worker);
				workerFleeSquad.removeUnit(worker);
			}
		}
	}
}

void CombatCommander::updateBackupSquads()
{
    if (!m_attackStarted)
    {
        return;
    }

    Squad & mainAttackSquad = m_squadData.getSquad("MainAttack");
    Squad & backupSquad = m_squadData.getSquad("Backup");
	std::vector<Unit*> idleHellions;
	std::vector<Unit*> idleMarines;
	std::vector<Unit*> idleVikings;
	std::vector<Unit*> idleCyclones;
    for (auto & unit : m_combatUnits)
    {
        BOT_ASSERT(unit.isValid(), "null unit in combat units");

		const sc2::UnitTypeID unitTypeId = unit.getType().getAPIUnitType();
		if ((unitTypeId == sc2::UNIT_TYPEID::TERRAN_MARINE
			|| unitTypeId == sc2::UNIT_TYPEID::TERRAN_MARAUDER
			|| unitTypeId == sc2::UNIT_TYPEID::TERRAN_MEDIVAC
			|| unitTypeId == sc2::UNIT_TYPEID::TERRAN_REAPER
			|| unitTypeId == sc2::UNIT_TYPEID::TERRAN_HELLION
			|| unitTypeId == sc2::UNIT_TYPEID::TERRAN_CYCLONE
			|| unitTypeId == sc2::UNIT_TYPEID::TERRAN_VIKINGFIGHTER
			|| unitTypeId == sc2::UNIT_TYPEID::TERRAN_VIKINGASSAULT
			//|| unitTypeId == sc2::UNIT_TYPEID::TERRAN_BANSHEE
			|| unitTypeId == sc2::UNIT_TYPEID::TERRAN_RAVEN
			|| unitTypeId == sc2::UNIT_TYPEID::TERRAN_BATTLECRUISER
			|| unitTypeId == sc2::UNIT_TYPEID::TERRAN_THOR
			|| unitTypeId == sc2::UNIT_TYPEID::TERRAN_THORAP
			|| (unitTypeId == sc2::UNIT_TYPEID::TERRAN_BARRACKSFLYING && m_bot.Strategy().getStartingStrategy() == PROXY_CYCLONES && m_bot.UnitInfo().getUnitTypeCount(Players::Self, MetaTypeEnum::Reaper.getUnitType(), true) + m_bot.GetDeadAllyUnitsCount(sc2::UNIT_TYPEID::TERRAN_REAPER) >= 2))
            && m_squadData.canAssignUnitToSquad(unit, backupSquad))
        {
			if (unitTypeId == sc2::UNIT_TYPEID::TERRAN_HELLION)
				idleHellions.push_back(&unit);
			else if (unitTypeId == sc2::UNIT_TYPEID::TERRAN_MARINE)
				idleMarines.push_back(&unit);
			else if (unitTypeId == sc2::UNIT_TYPEID::TERRAN_VIKINGFIGHTER || unitTypeId == sc2::UNIT_TYPEID::TERRAN_VIKINGASSAULT)
				idleVikings.push_back(&unit);
			else if (unitTypeId == sc2::UNIT_TYPEID::TERRAN_CYCLONE)
				idleCyclones.push_back(&unit);
			else
                m_squadData.assignUnitToSquad(unit, backupSquad);
        }
    }
    
	if (idleHellions.size() >= Util::HELLION_SQUAD_COUNT)
	{
		for (auto hellion : idleHellions)
		{
			m_squadData.assignUnitToSquad(*hellion, backupSquad);
		}
	}
	const auto battlecruisers = m_bot.UnitInfo().getUnitTypeCount(Players::Self, MetaTypeEnum::Battlecruiser.getUnitType(), true, true);
	if (idleMarines.size() >= 10 && battlecruisers > 0)
	{
		for (auto marine : idleMarines)
		{
			m_squadData.assignUnitToSquad(*marine, backupSquad);
		}
	}
	const auto tempestCount = m_bot.GetEnemyUnits(sc2::UNIT_TYPEID::PROTOSS_TEMPEST).size();
	const auto VIKING_TEMPEST_RATIO = 2.5f;
	const auto vikingsCount = m_bot.UnitInfo().getUnitTypeCount(Players::Self, MetaTypeEnum::Viking.getUnitType(), true, true);
	if (vikingsCount >= tempestCount * VIKING_TEMPEST_RATIO)
	{
		for (auto viking : idleVikings)
		{
			m_squadData.assignUnitToSquad(*viking, backupSquad);
		}
		m_hasEnoughVikingsAgainstTempests = true;
	}
	else
	{
		m_hasEnoughVikingsAgainstTempests = false;
		auto vikings = backupSquad.getUnitsOfType(sc2::UNIT_TYPEID::TERRAN_VIKINGFIGHTER);
		auto vikingsAssault = backupSquad.getUnitsOfType(sc2::UNIT_TYPEID::TERRAN_VIKINGASSAULT);
		vikings.insert(vikings.end(), vikingsAssault.begin(), vikingsAssault.end());
		// Otherwise we remove our Vikings from the Backup Squad when they are close to our base
		for (const auto & viking : vikings)
		{
			if (Util::DistSq(viking, m_bot.GetStartLocation()) < 10.f * 10.f)
			{
				backupSquad.removeUnit(viking);
			}
		}
	}
	if (!idleCyclones.empty())
	{
		bool addCyclones = false;
		if (m_bot.Strategy().getStartingStrategy() == PROXY_MARAUDERS)
		{
			addCyclones = true;
		}
		else
		{
			for (const auto & unit : backupSquad.getUnits())
			{
				if (unit.isFlying())
				{
					addCyclones = true;
					break;
				}
			}
			if (!addCyclones)
			{
				for (const auto & unit : mainAttackSquad.getUnits())
				{
					if (unit.isFlying())
					{
						addCyclones = true;
						break;
					}
				}
			}
		}
		if (addCyclones)
		{
			for (auto cyclone : idleCyclones)
			{
				m_squadData.assignUnitToSquad(*cyclone, backupSquad);
			}
		}
	}

	if (mainAttackSquad.isSuiciding())
	{
		SquadOrder retreatOrder(SquadOrderTypes::Retreat, m_bot.GetStartLocation(), 25, "Retreat");
		backupSquad.setSquadOrder(retreatOrder);
	}
	else
	{
		SquadOrder sendBackupsOrder(SquadOrderTypes::Attack, mainAttackSquad.calcCenter(), 25, "Send backups");
		backupSquad.setSquadOrder(sendBackupsOrder);
	}
}

void CombatCommander::updateClearExpandSquads()
{
	// reset clear expand squads
	for (const auto & kv : m_squadData.getSquads())
	{
		const Squad & squad = kv.second;
		if (squad.getName().find("Clear Expand") != std::string::npos)
		{
			m_squadData.getSquad(squad.getName()).clear();
		}
	}

	for(const auto baseLocation : m_bot.Bases().getBaseLocations())
	{
		if(baseLocation->isBlocked())
		{
			const auto basePosition = baseLocation->getPosition();
			std::stringstream squadName;
			squadName << "Clear Expand " << basePosition.x << " " << basePosition.y;

			const SquadOrder clearExpand(SquadOrderTypes::Attack, basePosition, DefaultOrderRadius, "Clear Blocked Expand");
			// if we don't have a squad assigned to this blocked expand already, create one
			if (!m_squadData.squadExists(squadName.str()))
			{
				m_squadData.addSquad(squadName.str(), Squad(squadName.str(), clearExpand, ClearExpandPriority, m_bot));
			}

			// assign units to the squad
			Squad & clearExpandSquad = m_squadData.getSquad(squadName.str());
			clearExpandSquad.setSquadOrder(clearExpand);

			// add closest unit in squad
			Unit closestUnit;
			float distance = 0.f;
			for (auto & unitPair : m_bot.GetAllyUnits())
			{
				const auto & unit = unitPair.second;

				if (!unit.isValid())
					continue;
				if (unit.getType().isBuilding())
					continue;
				if (unit.getType().isWorker())
					continue;
				if (!m_squadData.canAssignUnitToSquad(unit, clearExpandSquad))
					continue;

				if(Util::CanUnitAttackGround(unit.getUnitPtr(), m_bot))
				{
					const float dist = Util::DistSq(unit, basePosition);
					if(!closestUnit.isValid() || dist < distance)
					{
						distance = dist;
						closestUnit = unit;
					}
				}
			}

			if(closestUnit.isValid())
			{
				m_squadData.assignUnitToSquad(closestUnit.getUnitPtr(), clearExpandSquad);
			}
		}
	}
}

void CombatCommander::updateScoutSquad()
{
	if (!m_bot.Strategy().enemyHasMassZerglings() && m_bot.GetCurrentFrame() < 4704)	//around 3:30, or as soon as enemy has a lot of lings
		return;

	Squad & scoutSquad = m_squadData.getSquad("Scout");
	if (scoutSquad.getUnits().empty())
	{
		Unit bestCandidate;
		float distanceFromBase = 0.f;
		for (auto & unit : m_combatUnits)
		{
			BOT_ASSERT(unit.isValid(), "null unit in combat units");
			if (unit.getUnitPtr()->unit_type == sc2::UNIT_TYPEID::TERRAN_REAPER)
			{
				const auto base = m_bot.Bases().getPlayerStartingBaseLocation(Players::Self);
				if(base)
				{
					const float dist = Util::DistSq(unit, base->getPosition());
					if (!bestCandidate.isValid() || dist < distanceFromBase)
					{
						if (m_squadData.canAssignUnitToSquad(unit, scoutSquad))
						{
							bestCandidate = unit;
							distanceFromBase = dist;
						}
					}
				}
			}
		}
		if(bestCandidate.isValid())
		{
			m_squadData.assignUnitToSquad(bestCandidate, scoutSquad);
		}
	}

	if (scoutSquad.getUnits().empty())
	{
		return;
	}

	const SquadOrder scoutOrder(SquadOrderTypes::Scout, GetNextBaseLocationToScout(), ScoutOrderRadius, "Scout");
	scoutSquad.setSquadOrder(scoutOrder);
}

void CombatCommander::updateHarassSquads()
{
	std::vector<Unit> harassUnits;
	int i = 1;
	while (m_squadData.squadExists("Harass" + std::to_string(i)))
	{
		Squad & squad = m_squadData.getSquad("Harass" + std::to_string(i));
		const auto & squadUnits = squad.getUnits();
		harassUnits.insert(harassUnits.end(), squadUnits.begin(), squadUnits.end());
		squad.clear();
		++i;
	}
	std::vector<const BaseLocation *> baseLocations;
	const auto & occupiedBaseLocations = m_bot.Bases().getOccupiedBaseLocations(Players::Enemy);
	baseLocations.insert(baseLocations.end(), occupiedBaseLocations.begin(), occupiedBaseLocations.end());
	if (baseLocations.empty())
	{
		const BaseLocation* enemyStartingBase = m_bot.Bases().getPlayerStartingBaseLocation(Players::Enemy);
		if (!enemyStartingBase)
			return;
		baseLocations.push_back(enemyStartingBase);
	}
	
	i = 0;
	for (auto baseLocation : baseLocations)
	{
		++i;
		std::string squadName = "Harass" + std::to_string(i);
		const SquadOrder harassOrder(SquadOrderTypes::Harass, baseLocation->getPosition(), HarassOrderRadius, squadName);
		if (!m_squadData.squadExists(squadName))
		{
			Squad harassSquad(squadName, harassOrder, HarassPriority, m_bot);
			m_squadData.addSquad(squadName, harassSquad);
		}
		else
		{
			Squad & squad = m_squadData.getSquad(squadName);
			squad.setSquadOrder(harassOrder);
		}
	}
	
	Squad & idleSquad = m_squadData.getSquad("Idle");
	for (auto & idleUnit : idleSquad.getUnits())
	{
		if (idleUnit.getAPIUnitType() == sc2::UNIT_TYPEID::TERRAN_BANSHEE)
		{
			harassUnits.push_back(idleUnit);
		}
	}

	if (harassUnits.empty())
		return;

	i = 0;
	for (auto & harassUnit : harassUnits)
	{
		++i;
		std::string squadName = "Harass" + std::to_string(i);
		if (!m_squadData.squadExists(squadName))
		{
			i = 1;
			squadName = "Harass" + std::to_string(i);
		}
		auto & harassSquad = m_squadData.getSquad(squadName);
		m_squadData.assignUnitToSquad(harassUnit, harassSquad);
	}
}

void CombatCommander::updateAttackSquads()
{
    /*if (!m_attackStarted)
    {
        return;
    }*/

    Squad & mainAttackSquad = m_squadData.getSquad("MainAttack");

	// Worker rush strategy is not used anymore
	/*if (m_bot.Strategy().getStartingStrategy() == WORKER_RUSH && m_bot.GetCurrentFrame() >= 224)
	{
		for (auto & scv : m_bot.GetAllyUnits(sc2::UNIT_TYPEID::TERRAN_SCV))
		{
			if (!scv.isReturningCargo() && mainAttackSquad.getUnits().size() < 11 && m_squadData.canAssignUnitToSquad(scv, mainAttackSquad, true))
			{
				m_bot.Workers().getWorkerData().setWorkerJob(scv, WorkerJobs::Combat);
				m_squadData.assignUnitToSquad(scv, mainAttackSquad);
			}
		}
		
		const SquadOrder mainAttackOrder(SquadOrderTypes::Attack, getMainAttackLocation(), MainAttackOrderRadius, "Attack");
		mainAttackSquad.setSquadOrder(mainAttackOrder);
	}*/

	const auto squadCenter = mainAttackSquad.calcCenter();
	std::vector<Unit> unitsToTransfer;
	Squad & backupSquad = m_squadData.getSquad("Backup");

	for (auto & unit : mainAttackSquad.getUnits())
	{
		const auto dist = Util::DistSq(unit, squadCenter);
		const auto radius = mainAttackSquad.getSquadOrder().getRadius();
		if (dist > radius * radius)
			unitsToTransfer.push_back(unit);
	}

	for (const auto & unit : unitsToTransfer)
	{
		mainAttackSquad.removeUnit(unit);
		m_squadData.assignUnitToSquad(unit, backupSquad);
	}
	unitsToTransfer.clear();
	
	for (auto & backupUnit : backupSquad.getUnits())
	{
		bool closeEnough = mainAttackSquad.getUnits().empty();
		if (!closeEnough)
		{
			auto dist = Util::DistSq(backupUnit, squadCenter);
			const auto radius = mainAttackSquad.getSquadOrder().getRadius();
			if (dist <= radius * radius)
				closeEnough = true;
			else
			{
				for (auto & unit : mainAttackSquad.getUnits())
				{
					dist = Util::DistSq(unit, backupUnit);
					if (dist <= 10.f * 10.f)
					{
						closeEnough = true;
						break;
					}
				}
			}
		}
		if (closeEnough && m_squadData.canAssignUnitToSquad(backupUnit, mainAttackSquad))
			unitsToTransfer.push_back(backupUnit);
	}
	
	for (const auto & unit : unitsToTransfer)
		m_squadData.assignUnitToSquad(unit, mainAttackSquad);

	if (mainAttackSquad.getUnits().empty())
		return;

	CCPosition orderPosition = GetClosestEnemyBaseLocation();
	
	// A retreat must last at least 5 seconds
	if (m_bot.GetCurrentFrame() >= m_lastRetreatFrame + 5 * 22.4)
	{
		bool earlyCycloneRush = false;
		if (m_bot.Strategy().getStartingStrategy() == PROXY_CYCLONES)
		{
			for (const auto & unit : m_combatUnits)
			{
				if (unit.getAPIUnitType() == sc2::UNIT_TYPEID::TERRAN_BARRACKSFLYING)
				{
					earlyCycloneRush = true;
					break;
				}
			}
		}

		// We don't want to stop the offensive with the proxy Cyclones strategy when we still have our flying Barracks
		if (!earlyCycloneRush)
		{
			sc2::Units allyUnits;
			Util::CCUnitsToSc2Units(mainAttackSquad.getUnits(), allyUnits);
			bool hasGround = false;
			bool hasAir = false;
			for (const auto ally : allyUnits)
			{
				if (hasGround && hasAir)
					break;
				hasGround = hasGround || !ally->is_flying;
				hasAir = hasGround || !ally->is_flying;
			}
			m_bot.StartProfiling("0.10.4.2.3.0     calcEnemies");
			sc2::Units enemyUnits;
			for (const auto & enemyUnitPair : m_bot.GetEnemyUnits())
			{
				const auto & enemyUnit = enemyUnitPair.second;
				if (enemyUnit.getType().isCombatUnit() && !enemyUnit.getType().isBuilding())
				{
					const bool canAttack = (hasGround && Util::CanUnitAttackGround(enemyUnit.getUnitPtr(), m_bot)) || (hasAir && Util::CanUnitAttackAir(enemyUnit.getUnitPtr(), m_bot));
					if (canAttack)
						enemyUnits.push_back(enemyUnit.getUnitPtr());
				}
			}
			m_bot.StopProfiling("0.10.4.2.3.0     calcEnemies");
			m_bot.StartProfiling("0.10.4.2.3.1     simulateCombat");
			const float simulationResult = Util::SimulateCombat(allyUnits, enemyUnits, m_bot);
			m_bot.StopProfiling("0.10.4.2.3.1     simulateCombat");
			if (m_winAttackSimulation)
			{
				m_winAttackSimulation = simulationResult > 0.f;
				if (!m_winAttackSimulation)
				{
					m_bot.Actions()->SendChat("Cancel offensive", sc2::ChatChannel::Team);
					m_lastRetreatFrame = m_bot.GetCurrentFrame();
				}
			}
			else
			{
				m_winAttackSimulation = simulationResult > 0.5f;
				if (m_winAttackSimulation)
					m_bot.Actions()->SendChat("Relaunch offensive", sc2::ChatChannel::Team);
			}
			if (!m_winAttackSimulation)
				orderPosition = m_bot.Strategy().isProxyStartingStrategy() ? Util::GetPosition(m_bot.Buildings().getProxyLocation()) : m_idlePosition;
		}
	}

	const SquadOrder mainAttackOrder(SquadOrderTypes::Attack, orderPosition, HarassOrderRadius, "Attack");
	mainAttackSquad.setSquadOrder(mainAttackOrder);

    /*if (mainAttackSquad.needsToRetreat())
    {
        SquadOrder retreatOrder(SquadOrderTypes::Retreat, getMainAttackLocation(), DefaultOrderRadius, "Retreat!!");
        mainAttackSquad.setSquadOrder(retreatOrder);
    }
    //regroup only after retreat
    else if (mainAttackSquad.needsToRegroup())
    {
        SquadOrder regroupOrder(SquadOrderTypes::Regroup, getMainAttackLocation(), DefaultOrderRadius, "Regroup");
        mainAttackSquad.setSquadOrder(regroupOrder);
    }
    else
    {
        SquadOrder mainAttackOrder(SquadOrderTypes::Attack, getMainAttackLocation(), MainAttackOrderRadius, "Attack");
        mainAttackSquad.setSquadOrder(mainAttackOrder);
    }*/
}

void CombatCommander::updateScoutDefenseSquad()
{
    // if the current squad has units in it then we can ignore this
    Squad & scoutDefenseSquad = m_squadData.getSquad("ScoutDefense");

    // get the region that our base is located in
    const BaseLocation * myBaseLocation = m_bot.Bases().getPlayerStartingBaseLocation(Players::Self);
    if (!myBaseLocation)
        return;

    // get all of the enemy units in this region
    std::vector<Unit> enemyUnitsInRegion;
	for (auto & unit : m_bot.GetKnownEnemyUnits())
	{
		if (myBaseLocation->containsPosition(unit.getPosition()) && unit.getType().isWorker())
		{
			enemyUnitsInRegion.push_back(unit);
			if (enemyUnitsInRegion.size() > 1)
				break;
		}
	}

    // if there's an enemy worker in our region
    if (enemyUnitsInRegion.size() == 1)
    {
		// and there is an injured worker in the squad, remove it
		if (!scoutDefenseSquad.isEmpty())
		{
			auto & units = scoutDefenseSquad.getUnits();
			for (auto & unit : units)
			{
				if (unit.getUnitPtr()->health < unit.getUnitPtr()->health_max * m_bot.Workers().MIN_HP_PERCENTAGE_TO_FIGHT)
				{
					m_bot.Workers().finishedWithWorker(unit);
					scoutDefenseSquad.removeUnit(unit);
				}
			}
		}

		// if our the squad is empty, assign a worker
		if(scoutDefenseSquad.isEmpty())
		{
			// the enemy worker that is attacking us
			Unit enemyWorkerUnit = *enemyUnitsInRegion.begin();
			BOT_ASSERT(enemyWorkerUnit.isValid(), "null enemy worker unit");

			Unit workerDefender = findWorkerToAssignToSquad(scoutDefenseSquad, enemyWorkerUnit.getPosition(), enemyWorkerUnit, enemyUnitsInRegion);
			if (workerDefender.isValid())
			{
				m_squadData.assignUnitToSquad(workerDefender, scoutDefenseSquad);
			}
		}
    }
    // if our squad is not empty and we shouldn't have a worker chasing then take him out of the squad
    else if (!scoutDefenseSquad.isEmpty())
    {
        for (auto & unit : scoutDefenseSquad.getUnits())
        {
            BOT_ASSERT(unit.isValid(), "null unit in scoutDefenseSquad");

            if (unit.getType().isWorker())
            {
                m_bot.Workers().finishedWithWorker(unit);
            }
        }

        scoutDefenseSquad.clear();
    }
}

void CombatCommander::updateDefenseBuildings()
{
	handleWall();
	lowerSupplyDepots();
}

void CombatCommander::handleWall()
{
	int SUPPLYDEPOT_DISTANCE = 10 * 10;	// 10 tiles ^ 2, because we use DistSq

	auto wallCenter = m_bot.Buildings().getWallPosition();
	auto & enemies = m_bot.GetKnownEnemyUnits();

	for (auto & enemy : enemies)
	{
		if (enemy.isFlying() || enemy.getType().isBuilding())
			continue;
		CCTilePosition enemyPosition = enemy.getTilePosition();
		int distance = Util::DistSq(enemyPosition, wallCenter);
		if (distance < SUPPLYDEPOT_DISTANCE)
		{//Raise wall
			for (auto & building : m_bot.Buildings().getWallBuildings())
			{
				if (building.getAPIUnitType() == sc2::UNIT_TYPEID::TERRAN_SUPPLYDEPOTLOWERED)
				{
					bool willRaise = true;
					for (auto & unit : m_bot.GetAllyUnits())
					{
						if (unit.second.getType().isBuilding() || unit.second.isFlying())
						{
							continue;
						}
						//If the unit is on the depot, dont try to raise. Otherwise it forces the unit to move which can cause micro issues.
						if (Util::DistSq(Util::GetPosition(building.getTilePosition()), unit.second.getPosition()) <= pow(building.getUnitPtr()->radius + unit.second.getUnitPtr()->radius, 2))
						{
							willRaise = false;
							break;
						}
					}
					if (willRaise)
					{
						building.useAbility(sc2::ABILITY_ID::MORPH_SUPPLYDEPOT_RAISE);
					}
				}
			}
			return;
		}
	}

	//Lower wall
	for (auto & building : m_bot.Buildings().getWallBuildings())
	{
		if (building.getAPIUnitType() == sc2::UNIT_TYPEID::TERRAN_SUPPLYDEPOT)
		{
			building.useAbility(sc2::ABILITY_ID::MORPH_SUPPLYDEPOT_LOWER);
		}
	}
}

void CombatCommander::lowerSupplyDepots()
{
	for(auto & supplyDepot : m_bot.GetAllyUnits(sc2::UNIT_TYPEID::TERRAN_SUPPLYDEPOT))
	{
		if(supplyDepot.isCompleted() && !Util::Contains(supplyDepot, m_bot.Buildings().getWallBuildings()))
		{
			supplyDepot.useAbility(sc2::ABILITY_ID::MORPH_SUPPLYDEPOT_LOWER);
		}
	}
}

struct RegionArmyInformation
{
	const BaseLocation* baseLocation;
	CCBot& bot;
	std::vector<Unit> enemyUnits;
	std::vector<const sc2::Unit*> affectedAllyUnits;
	std::unordered_map<const sc2::Unit*, float> unitGroundScores;
	std::unordered_map<const sc2::Unit*, float> unitAirScores;
	std::unordered_map<const sc2::Unit*, float> unitDetectionScores;
	float airEnemyPower;
	float groundEnemyPower;
	bool invisEnemies;
	float antiAirAllyPower;
	float antiGroundAllyPower;
	bool antiInvis;
	Squad* squad;
	Unit closestEnemyUnit;

	RegionArmyInformation(const BaseLocation* baseLocation, CCBot& bot)
		: baseLocation(baseLocation)
		, bot(bot)
		, airEnemyPower(0)
		, groundEnemyPower(0)
		, invisEnemies(false)
		, antiAirAllyPower(0)
		, antiGroundAllyPower(0)
		, antiInvis(false)
		, squad(nullptr)
		, closestEnemyUnit({})
	{}

	std::unordered_map<const sc2::Unit*, float> & getScores(std::string type)
	{
		if (type == "detection")
			return unitDetectionScores;
		if (type == "ground")
			return unitGroundScores;
		return unitAirScores;
	}

	void calcEnemyPower()
	{
		airEnemyPower = 0;
		groundEnemyPower = 0;
		invisEnemies = false;
		for (auto& unit : enemyUnits)
		{
			auto power = Util::GetUnitPower(unit.getUnitPtr(), nullptr, bot);
			if (power == 0.f)
				power = Util::GetSpecialCasePower(unit);
			if (unit.isFlying())
				airEnemyPower += power;
			else
				groundEnemyPower += power;
			if (unit.isCloaked() || unit.isBurrowed())
				invisEnemies = true;
		}
	}

	void calcClosestEnemy()
	{
		float minDist = 0.f;
		for(auto & enemyUnit : enemyUnits)
		{
			const float dist = Util::DistSq(enemyUnit, baseLocation->getPosition());
			if(!closestEnemyUnit.isValid() || dist < minDist)
			{
				minDist = dist;
				closestEnemyUnit = enemyUnit;
			}
		}
	}

	void affectAllyUnit(const sc2::Unit* unit)
	{
		affectedAllyUnits.push_back(unit);
		const float power = Util::GetUnitPower(unit, nullptr, bot);
		const bool canAttackGround = Util::CanUnitAttackGround(unit, bot);
		const bool canAttackAir = Util::CanUnitAttackAir(unit, bot);
		const bool isDetector = UnitType(unit->unit_type, bot).isDetector();
		if(canAttackGround)
		{
			if(canAttackAir)
			{
				if(airEnemyPower - antiAirAllyPower > groundEnemyPower - antiGroundAllyPower)
				{
					antiAirAllyPower += power;
				}
				else
				{
					antiGroundAllyPower += power;
				}
			}
			else
			{
				antiGroundAllyPower += power;
			}
		}
		else
		{
			antiAirAllyPower += power;
		}
		if (isDetector)
		{
			antiInvis = true;
		}
	}

	float antiGroundPowerNeeded() const
	{
		return groundEnemyPower * 1.5f - antiGroundAllyPower;
	}

	float antiAirPowerNeeded() const
	{
		return airEnemyPower * 1.5f - antiAirAllyPower;
	}

	bool antiInvisNeeded() const
	{
		return invisEnemies && !antiInvis;
	}

	bool needsMoreAntiGround() const
	{
		return antiGroundPowerNeeded() > 0;
	}

	bool needsMoreAntiAir() const
	{
		return antiAirPowerNeeded() > 0;
	}

	bool needsMoreSupport() const
	{
		return needsMoreAntiGround() || needsMoreAntiAir();
	}

	float getTotalPowerNeeded() const
	{
		return antiGroundPowerNeeded() + antiAirPowerNeeded();
	}

	bool operator<(const RegionArmyInformation& ref) const
	{
		return getTotalPowerNeeded() > ref.getTotalPowerNeeded();	// greater is used to have a decreasing order
	}
};

void CombatCommander::updateDefenseSquads()
{
	// reset defense squads
	for (const auto & kv : m_squadData.getSquads())
	{
		const Squad & squad = kv.second;
		const SquadOrder & order = squad.getSquadOrder();

		if (order.getType() != SquadOrderTypes::Defend || squad.getName() == "ScoutDefense")
		{
			continue;
		}

		m_squadData.getSquad(squad.getName()).clear();
	}

	bool workerRushed = false;
	bool earlyRushed = false;
	// TODO instead of separing by bases, we should separate by clusters
	std::list<RegionArmyInformation> regions;
	// for each of our occupied regions
	const BaseLocation * enemyBaseLocation = m_bot.Bases().getPlayerStartingBaseLocation(Players::Enemy);
	const auto & ourBases = m_bot.Bases().getOccupiedBaseLocations(Players::Self);
	auto nextExpansion = m_bot.Bases().getNextExpansion(Players::Self, false, false);
	std::set<BaseLocation*> bases;
	bases.insert(ourBases.begin(), ourBases.end());
	if (nextExpansion)
		bases.insert(nextExpansion);
	for (BaseLocation * myBaseLocation : bases)
	{
		// don't defend inside the enemy region, this will end badly when we are stealing gas or cannon rushing
		if (myBaseLocation == enemyBaseLocation)
		{
			continue;
		}

		const auto proxyBase = m_bot.Strategy().isProxyStartingStrategy() && myBaseLocation->containsPositionApproximative(Util::GetPosition(m_bot.Buildings().getProxyLocation()));

		m_bot.StartProfiling("0.10.4.2.2.1      detectEnemiesInRegions");
		auto region = RegionArmyInformation(myBaseLocation, m_bot);

		const CCPosition basePosition = Util::GetPosition(myBaseLocation->getDepotTilePosition());

		// calculate how many units are flying / ground units
		bool unitOtherThanWorker = false;
		float minEnemyDistance = 0;
		Unit closestEnemy;
		int enemyWorkers = 0;
		for (auto & unit : m_bot.GetKnownEnemyUnits())
		//for (auto & unit : m_bot.UnitInfo().getUnits(Players::Enemy))
		{
			// if it's an overlord, don't worry about it for defense, we don't care what they see
			if (unit.getType().isOverlord())
			{
				continue;
			}

			// if the unit is not targetable, we do not need to defend against it (shade, kd8 charge, disruptor's ball, etc.)
			if (!UnitType::isTargetable(unit.getAPIUnitType()))
				continue;

			if (myBaseLocation->containsUnitApproximative(unit, m_bot.Strategy().isWorkerRushed() ? WorkerRushDefenseOrderRadius : 0))
			{
				if (!workerRushed && unit.getType().isWorker() && !unitOtherThanWorker && m_bot.GetGameLoop() < 4392 && myBaseLocation == m_bot.Bases().getPlayerStartingBaseLocation(Players::Self))	// first 3 minutes
				{
					// Need at least 3 workers for a worker rush (or 1 if the previous frame was a worker rush)
					if (!m_bot.Strategy().isWorkerRushed() && enemyWorkers < 3)
						++enemyWorkers;
					else
						workerRushed = true;
				}
				else if (!earlyRushed && !proxyBase && m_bot.GetGameLoop() < 7320)	// first 5 minutes
				{
					earlyRushed = true;
				}

				if (!unit.getType().isWorker())
				{
					unitOtherThanWorker = true;
					workerRushed = false;
				}

				const float enemyDistance = Util::DistSq(unit.getPosition(), basePosition);
				if (!closestEnemy.isValid() || enemyDistance < minEnemyDistance)
				{
					minEnemyDistance = enemyDistance;
					closestEnemy = unit;
				}

				region.enemyUnits.push_back(unit);
			}
		}

		// We can ignore a single enemy worker in our region since we assume it is a scout (handled by scout defense)
		if (region.enemyUnits.size() == 1 && enemyWorkers == 1)
			region.enemyUnits.clear();

		std::stringstream squadName;
		squadName << "Base Defense " << basePosition.x << " " << basePosition.y;

		myBaseLocation->setIsUnderAttack(!region.enemyUnits.empty());
		m_bot.StopProfiling("0.10.4.2.2.1      detectEnemiesInRegions");
		if (region.enemyUnits.empty())
		{
			m_bot.StartProfiling("0.10.4.2.2.3      clearRegion");
			// if a defense squad for this region exists, remove it
			if (m_squadData.squadExists(squadName.str()))
			{
				m_squadData.getSquad(squadName.str()).clear();
			}

			if (Util::IsTerran(m_bot.GetSelfRace()))
			{
				Unit base = m_bot.Buildings().getClosestResourceDepot(basePosition);
				if (base.isValid())
				{
					if (base.isFlying())
					{
						Micro::SmartAbility(base.getUnitPtr(), sc2::ABILITY_ID::LAND, basePosition, m_bot);
					}
					else if (base.getUnitPtr()->cargo_space_taken > 0)
					{
						Micro::SmartAbility(base.getUnitPtr(), sc2::ABILITY_ID::UNLOADALL, m_bot);

						for (auto & worker : m_bot.Workers().getWorkers())
						{
							if (m_bot.Workers().getWorkerData().getWorkerJob(worker) != WorkerJobs::Scout)
							{
								m_bot.Workers().finishedWithWorker(worker);
							}
						}
					}
				}
			}
			m_bot.StopProfiling("0.10.4.2.2.3      clearRegion");

			// and return, nothing to defend here
			continue;
		}

		m_bot.StartProfiling("0.10.4.2.2.3      createSquad");
		const SquadOrder defendRegion(SquadOrderTypes::Defend, closestEnemy.getPosition(), m_bot.Strategy().isWorkerRushed() ? WorkerRushDefenseOrderRadius : DefaultOrderRadius, "Defend Region!");
		// if we don't have a squad assigned to this region already, create one
		if (!m_squadData.squadExists(squadName.str()))
		{
			m_squadData.addSquad(squadName.str(), Squad(squadName.str(), defendRegion, BaseDefensePriority, m_bot));
		}

		// assign units to the squad
		if (m_squadData.squadExists(squadName.str()))
		{
			Squad & defenseSquad = m_squadData.getSquad(squadName.str());
			defenseSquad.setSquadOrder(defendRegion);
			region.squad = &defenseSquad;
		}
		else
		{
			BOT_ASSERT(false, "Squad should have existed: %s", squadName.str().c_str());
		}
		m_bot.StopProfiling("0.10.4.2.2.3      createSquad");

		m_bot.StartProfiling("0.10.4.2.2.4      calculateRegionInformation");
		region.calcEnemyPower();
		region.calcClosestEnemy();
		regions.push_back(region);
		m_bot.StopProfiling("0.10.4.2.2.4      calculateRegionInformation");

		//Protect our SCVs and lift our base
		if (Util::IsTerran(m_bot.GetSelfRace()))
		{
			const Unit& base = myBaseLocation->getResourceDepot();
			if (base.isValid())
			{
				if (base.getUnitPtr()->cargo_space_taken == 0 && m_bot.Workers().getNumWorkers() > 0)
				{
					// Hide our last SCVs (should be 5, but is higher because some workers may end up dying on the way)
					if (m_bot.Workers().getNumWorkers() <= 7)
						Micro::SmartAbility(base.getUnitPtr(), sc2::ABILITY_ID::LOADALL, m_bot);
				}
				else if (!base.isFlying() && base.getUnitPtr()->health < base.getUnitPtr()->health_max * 0.5f)
					Micro::SmartAbility(base.getUnitPtr(), sc2::ABILITY_ID::LIFT, m_bot);
			}
		}
	}

	if (workerRushed)
	{
		m_lastWorkerRushDetectionFrame = m_bot.GetCurrentFrame();
		m_bot.Strategy().setIsWorkerRushed(true);
	}
	else if (m_bot.GetCurrentFrame() > m_lastWorkerRushDetectionFrame + WORKER_RUSH_DETECTION_COOLDOWN)
	{
		m_bot.Strategy().setIsWorkerRushed(false);
	}
	m_bot.Strategy().setIsEarlyRushed(earlyRushed);

	// Find our Reaper that is the closest to the enemy base
	const sc2::Unit * offensiveReaper = nullptr;
	if (earlyRushed)
	{
		if (enemyBaseLocation)
		{
			for (auto & unitPair : m_bot.GetAllyUnits())
			{
				float minDist = 0.f;
				if (unitPair.second.getAPIUnitType() == sc2::UNIT_TYPEID::TERRAN_REAPER)
				{
					const auto dist = Util::DistSq(unitPair.second, enemyBaseLocation->getPosition());
					if (!offensiveReaper || dist < minDist)
					{
						minDist = dist;
						offensiveReaper = unitPair.second.getUnitPtr();
					}
				}
			}
		}
	}

	// If we have at least one region under attack
	if(!regions.empty())
	{
		m_bot.StartProfiling("0.10.4.2.2.5      calculateRegionsScores");
		// We sort them (the one with the strongest enemy force is first)
		regions.sort();

		// We check each of our units to determine how useful they would be for defending each of our attacked regions
		for (auto & unitPair : m_bot.GetAllyUnits())
		{
			auto & unit = unitPair.second;
			if (unit.getType().isWorker() || unit.getType().isBuilding())
				continue;	// We don't want to consider our workers and defensive buildings (they will automatically defend their region)

			if (unit.getUnitPtr() == offensiveReaper)
				continue;	// We want to keep at least one Reaper in the Harass squad (defined only when early rushed)

			// We check how useful our unit would be for anti ground and anti air for each of our regions
			for (auto & region : regions)
			{
				const float distance = Util::Dist(unit, region.baseLocation->getPosition());
				bool weakUnitAgainstOnlyBuildings = unit.getAPIUnitType() == sc2::UNIT_TYPEID::TERRAN_REAPER;
				bool immune = true;
				bool detectionUseful = false;
				float maxGroundDps = 0.f;
				float maxAirDps = 0.f;
				const bool workerScout = region.enemyUnits.size() == 1 && region.enemyUnits[0].getType().isWorker();
				if (workerScout)
					continue;	// We do not want to send a combat unit against an enemy scout
				for (auto & enemyUnit : region.enemyUnits)
				{
					// As soon as there is a non building unit that the weak unit can attack, we consider that the weak unit can be useful
					if (weakUnitAgainstOnlyBuildings)
					{
						if (enemyUnit.getType().isBuilding())
							continue;
						if (Util::GetDpsForTarget(unit.getUnitPtr(), enemyUnit.getUnitPtr(), m_bot) > 0.f)
							weakUnitAgainstOnlyBuildings = false;
					}
					// We check if our unit is immune to the enemy unit (as soon as one enemy unit can attack our unit, we stop checking)
					if (immune)
					{
						if (unit.isFlying())
						{
							immune = !Util::CanUnitAttackAir(enemyUnit.getUnitPtr(), m_bot);
						}
						else
						{
							immune = !Util::CanUnitAttackGround(enemyUnit.getUnitPtr(), m_bot);
						}
					}
					if (!detectionUseful && unit.getType().isDetector() && (enemyUnit.isCloaked() || enemyUnit.isBurrowed()))
						detectionUseful = true;
					// We check the max ground and air dps that our unit would do in that region
					const float dps = Util::GetDpsForTarget(unit.getUnitPtr(), enemyUnit.getUnitPtr(), m_bot);
					if (enemyUnit.isFlying())
					{
						if (dps > maxAirDps)
						{
							maxAirDps = dps;
						}
					}
					else
					{
						if (dps > maxGroundDps)
						{
							maxGroundDps = dps;
						}
					}
				}
				// The weak unit would not be useful against buildings, it should harass instead of defend
				if (weakUnitAgainstOnlyBuildings)
					continue;
				// If our unit would have a valid ground target, we calculate the score (usefulness in that region) and add it to the list
				if (maxGroundDps > 0.f)
				{
					float regionScore = immune * 50 + maxGroundDps - distance;
					region.unitGroundScores[unit.getUnitPtr()] = regionScore;
				}
				// If our unit would have a valid air target, we calculate the score (usefulness in that region) and add it to the list
				if (maxAirDps > 0.f)
				{
					float regionScore = immune * 50 + maxAirDps - distance;
					region.unitAirScores[unit.getUnitPtr()] = regionScore;
				}
				if (detectionUseful)
				{
					float regionScore = 100 - distance;
					region.unitDetectionScores[unit.getUnitPtr()] = regionScore;
				}
			}
		}
		m_bot.StopProfiling("0.10.4.2.2.5      calculateRegionsScores");

		m_bot.StartProfiling("0.10.4.2.2.6      affectUnits");
		while (true)
		{
			Unit unit;
			const sc2::Unit* unitptr = nullptr;

			// We take the region that needs the most support
			auto regionIterator = regions.begin();
			auto & squad = *regionIterator->squad;
			bool stopCheckingForGroundSupport = false;
			bool stopCheckingForAirSupport = false;
			bool stopCheckingForDetectionSupport = false;

			do
			{
				auto & region = *regionIterator;

				// We find the unit that is the most interested to defend that region
				float bestScore = 0.f;
				std::string support;
				if (region.antiInvisNeeded() && !stopCheckingForDetectionSupport)
				{
					support = "detection";
				}
				else if(region.groundEnemyPower > 0.f && (stopCheckingForAirSupport || region.airEnemyPower == 0.f))
				{
					support = "ground";
				}
				else if(region.airEnemyPower > 0.f && (stopCheckingForGroundSupport || region.groundEnemyPower == 0.f))
				{
					support = "air";
				}
				else
				{
					support = region.antiGroundPowerNeeded() >= region.antiAirPowerNeeded() ? "ground" : "air";
				}
				bool needsMoreSupport = true;
				if (support == "ground")
					needsMoreSupport = region.needsMoreAntiGround();
				else if (support == "air")
					needsMoreSupport = region.needsMoreAntiAir();
				auto& scores = region.getScores(support);
				for (auto & scorePair : scores)
				{
					// If the base already has enough defense
					if (!needsMoreSupport)
					{
						// We check if the unit is in an offensive squad
						const auto scoredUnit = Unit(scorePair.first, m_bot);
						const auto unitSquad = m_squadData.getUnitSquad(scoredUnit);
						if (unitSquad)
						{
							const auto & squadOrder = unitSquad->getSquadOrder();
							if (squadOrder.getType() == SquadOrderTypes::Attack || squadOrder.getType() == SquadOrderTypes::Harass)
							{
								// If the unit is closer to its squad order objective than the base to defend, we won't send back that unit to defend
								if (Util::DistSq(scoredUnit, squadOrder.getPosition()) < Util::DistSq(scoredUnit, region.squad->getSquadOrder().getPosition()))
								{
									continue;
								}
							}
						}
					}
					if (!unitptr || scorePair.second > bestScore)
					{
						bestScore = scorePair.second;
						unitptr = scorePair.first;
					}
				}

				// If we have a unit that can defend
				if (unitptr)
				{
					unit = Unit(unitptr, m_bot);
				}
				// If we have no more unit to defend we check for the workers
				else if(support == "ground" && needsMoreSupport)
				{
					unit = findWorkerToAssignToSquad(*region.squad, region.baseLocation->getPosition(), region.closestEnemyUnit, region.enemyUnits);
				}

				// If no support is available
				if (!unit.isValid())
				{
					// If we were checking for detection support, stop checking for detection support
					if (support == "detection" && !stopCheckingForDetectionSupport)
					{
						stopCheckingForDetectionSupport = true;
						continue;	// Continue to check if the same region needs the other type of support
					}
					// If we were checking for ground support, stop checking for ground support
					if(support == "ground" && !stopCheckingForGroundSupport)
					{
						stopCheckingForGroundSupport = true;
						continue;	// Continue to check if the same region needs the other type of support
					}
					// If we were checking for air support, stop checking for air support
					if(support == "air" && !stopCheckingForAirSupport)
					{
						stopCheckingForAirSupport = true;
						continue;	// Continue to check if the same region needs the other type of support
					}
					// Otherwise, check next region
					++regionIterator;
					// If this was the last region, exit
					if (regionIterator == regions.end())
						break;
					// Reset the support checks because it will now be for another region
					stopCheckingForGroundSupport = false;
					stopCheckingForAirSupport = false;
					stopCheckingForDetectionSupport = false;
				}
			} while (!unit.isValid());

			// BREAK CONDITION : there is no more unit to be affected to the defense squads
			if(!unit.isValid())
			{
				break;
			}

			// Assign it to the squad
			if(m_squadData.canAssignUnitToSquad(unit, squad))
			{
				// We affect that unit to the region
				regionIterator->affectAllyUnit(unit.getUnitPtr());
				m_squadData.assignUnitToSquad(unit.getUnitPtr(), squad);	// we cannot give a reference of the Unit because it doesn't have a big scope
			}
			else
			{
				Util::Log(__FUNCTION__, "Cannot assign unit of type " + unit.getType().getName() + " to squad " + squad.getName() + ", it is already in squad " + m_squadData.getUnitSquad(unit)->getName(), m_bot);
			}

			// We remove that unit from the score maps of all regions
			if (unitptr)
			{
				for (auto & regionToRemoveUnit : regions)
				{
					regionToRemoveUnit.unitGroundScores.erase(unitptr);
					regionToRemoveUnit.unitAirScores.erase(unitptr);
					regionToRemoveUnit.unitDetectionScores.erase(unitptr);
				}
			}

			// We sort the regions so the one that needs the most support comes back first
			regions.sort();
		}
		m_bot.StopProfiling("0.10.4.2.2.6      affectUnits");
	}
}

/*
 * This method is not used anymore.
 */
void CombatCommander::updateDefenseSquadUnits(Squad & defenseSquad, bool flyingDefendersNeeded, bool groundDefendersNeeded, Unit & closestEnemy)
{
    auto & squadUnits = defenseSquad.getUnits();

    for (auto & unit : squadUnits)
    {
		// Let injured worker return mining, no need to sacrifice it
		if (unit.getType().isWorker())
		{
			if (unit.getUnitPtr()->health < unit.getUnitPtr()->health_max * m_bot.Workers().MIN_HP_PERCENTAGE_TO_FIGHT ||
				!ShouldWorkerDefend(unit, defenseSquad, defenseSquad.getSquadOrder().getPosition(), closestEnemy, defenseSquad.getTargets()))
			{
				m_bot.Workers().finishedWithWorker(unit);
				defenseSquad.removeUnit(unit);
			}
		}
        else if (unit.isAlive())
        {
			bool isUseful = (flyingDefendersNeeded && unit.canAttackAir()) || (groundDefendersNeeded && unit.canAttackGround());
			if(!isUseful)
				defenseSquad.removeUnit(unit);
        }
    }

	if (flyingDefendersNeeded)
	{
		Unit defenderToAdd = findClosestDefender(defenseSquad, defenseSquad.getSquadOrder().getPosition(), closestEnemy, "air");

		while(defenderToAdd.isValid())
		{
			m_squadData.assignUnitToSquad(defenderToAdd, defenseSquad);
			defenderToAdd = findClosestDefender(defenseSquad, defenseSquad.getSquadOrder().getPosition(), closestEnemy, "air");
		}
	}

	if (groundDefendersNeeded)
	{
		Unit defenderToAdd = findClosestDefender(defenseSquad, defenseSquad.getSquadOrder().getPosition(), closestEnemy, "ground");

		while (defenderToAdd.isValid())
		{
			m_squadData.assignUnitToSquad(defenderToAdd, defenseSquad);
			defenderToAdd = findClosestDefender(defenseSquad, defenseSquad.getSquadOrder().getPosition(), closestEnemy, "ground");
		}
	}
}

Unit CombatCommander::findClosestDefender(const Squad & defenseSquad, const CCPosition & pos, Unit & closestEnemy, std::string type)
{
    Unit closestDefender;
    float minDistance = std::numeric_limits<float>::max();

    for (auto & unit : m_combatUnits)
    {
        BOT_ASSERT(unit.isValid(), "null combat unit");

		if (type == "air" && !unit.canAttackAir())
			continue;
		if (type == "ground" && !unit.canAttackGround())
			continue;

        if (!m_squadData.canAssignUnitToSquad(unit, defenseSquad))
        {
            continue;
        }

        const float dist = Util::DistSq(unit, closestEnemy);
        Squad *unitSquad = m_squadData.getUnitSquad(unit);
        if (unitSquad && (unitSquad->getName() == "MainAttack" || Util::StringStartsWith(unitSquad->getName(), "Harass")) && Util::DistSq(unit.getPosition(), unitSquad->getSquadOrder().getPosition()) < dist)
        {
            //We do not want to bring back the main attackers when they are closer to their objective than our base
            continue;
        }

        if (!closestDefender.isValid() || dist < minDistance)
        {
            closestDefender = unit;
            minDistance = dist;
        }
    }

    if (!closestDefender.isValid() && type == "ground")
    {
        // we search for worker to defend.
        closestDefender = findWorkerToAssignToSquad(defenseSquad, pos, closestEnemy, defenseSquad.getTargets());
    }

    return closestDefender;
}

Unit CombatCommander::findWorkerToAssignToSquad(const Squad & defenseSquad, const CCPosition & pos, Unit & closestEnemy, const std::vector<Unit> & enemyUnits)
{
    // get our worker unit that is mining that is closest to it
    Unit workerDefender = m_bot.Workers().getClosestMineralWorkerTo(closestEnemy.getPosition(), m_bot.Workers().MIN_HP_PERCENTAGE_TO_FIGHT);

	if(ShouldWorkerDefend(workerDefender, defenseSquad, pos, closestEnemy, enemyUnits))
	{
        m_bot.Workers().setCombatWorker(workerDefender);
    }
    else
    {
        workerDefender = {};
    }
    return workerDefender;
}

bool CombatCommander::ShouldWorkerDefend(const Unit & worker, const Squad & defenseSquad, const CCPosition & pos, Unit & closestEnemy, const std::vector<Unit> & enemyUnits) const
{
	if (!worker.isValid())
		return false;
	if (m_bot.Workers().getWorkerData().isProxyWorker(worker))
		return false;
	if (!m_squadData.canAssignUnitToSquad(worker, defenseSquad))
		return false;
	if (closestEnemy.isFlying())
		return false;
	// do not check distances if it is to protect against a scout
	if (defenseSquad.getName() == "ScoutDefense")
		return true;
	// do not check min distance if worker rushed
	if (m_bot.Strategy().isWorkerRushed())
		return true;
	// worker can fight buildings somewhat close to the base
	const auto isBuilding = closestEnemy.getType().isBuilding();
	const auto enemyDistanceToBase = Util::DistSq(closestEnemy, pos);
	const auto maxEnemyDistance = closestEnemy.getAPIUnitType() == sc2::UNIT_TYPEID::ZERG_NYDUSCANAL ? 30.f : 15.f;
	const auto enemyDistanceToWorker = Util::DistSq(worker, closestEnemy);
	if (isBuilding && enemyDistanceToBase < maxEnemyDistance * maxEnemyDistance && enemyDistanceToWorker < maxEnemyDistance * maxEnemyDistance)
		return true;
	// Worker should not defend against slow enemies, it should flee
	if (!WorkerHasFastEnemyThreat(worker.getUnitPtr(), enemyUnits))
		return false;
	// worker should not get too far from base and can fight only units close to it
	if (Util::DistSq(worker, pos) < 15.f * 15.f && enemyDistanceToWorker < 7.f * 7.f)
		return true;
	return false;
}

bool CombatCommander::WorkerHasFastEnemyThreat(const sc2::Unit * worker, const std::vector<Unit> & enemyUnits) const
{
	const auto workerSpeed = Util::getSpeedOfUnit(worker, m_bot);
	bool onlyWorkerThreats = true;
	const auto threats = Util::getThreats(worker, enemyUnits, m_bot);
	for (const auto threat : threats)
	{
		if (onlyWorkerThreats && !Util::IsWorker(threat->unit_type))
			onlyWorkerThreats = false;
		const auto threatSpeed = Util::getSpeedOfUnit(threat, m_bot);
		if (threatSpeed / workerSpeed >= 1.15f || threat->unit_type == sc2::UNIT_TYPEID::ZERG_ZERGLING)	// Workers shouldn't flee against Zerglings
			return true;
	}
	return onlyWorkerThreats;
}

std::map<Unit, std::pair<CCPosition, uint32_t>> & CombatCommander::GetInvisibleSighting()
{
	return m_invisibleSighting;
}

float CombatCommander::getTotalGroundInfluence(CCTilePosition tilePosition) const
{
	if (!m_bot.Map().isValidTile(tilePosition))
		return 0.f;
	return m_groundFromGroundCombatInfluenceMap[tilePosition.x][tilePosition.y] + m_groundFromAirCombatInfluenceMap[tilePosition.x][tilePosition.y] + m_groundEffectInfluenceMap[tilePosition.x][tilePosition.y];
}

float CombatCommander::getTotalAirInfluence(CCTilePosition tilePosition) const
{
	if (!m_bot.Map().isValidTile(tilePosition))
		return 0.f;
	return m_airFromGroundCombatInfluenceMap[tilePosition.x][tilePosition.y] + m_airFromAirCombatInfluenceMap[tilePosition.x][tilePosition.y] + m_airEffectInfluenceMap[tilePosition.x][tilePosition.y];
}

float CombatCommander::getGroundCombatInfluence(CCTilePosition tilePosition) const
{
	if (!m_bot.Map().isValidTile(tilePosition))
		return 0.f;
	return m_groundFromGroundCombatInfluenceMap[tilePosition.x][tilePosition.y] + m_groundFromAirCombatInfluenceMap[tilePosition.x][tilePosition.y];
}

float CombatCommander::getAirCombatInfluence(CCTilePosition tilePosition) const
{
	if (!m_bot.Map().isValidTile(tilePosition))
		return 0.f;
	return m_airFromGroundCombatInfluenceMap[tilePosition.x][tilePosition.y] + m_airFromAirCombatInfluenceMap[tilePosition.x][tilePosition.y];
}

float CombatCommander::getGroundFromGroundCombatInfluence(CCTilePosition tilePosition) const
{
	if (!m_bot.Map().isValidTile(tilePosition))
		return 0.f;
	return m_groundFromGroundCombatInfluenceMap[tilePosition.x][tilePosition.y];
}

float CombatCommander::getGroundFromAirCombatInfluence(CCTilePosition tilePosition) const
{
	if (!m_bot.Map().isValidTile(tilePosition))
		return 0.f;
	return m_groundFromAirCombatInfluenceMap[tilePosition.x][tilePosition.y];
}

float CombatCommander::getAirFromGroundCombatInfluence(CCTilePosition tilePosition) const
{
	if (!m_bot.Map().isValidTile(tilePosition))
		return 0.f;
	return m_airFromGroundCombatInfluenceMap[tilePosition.x][tilePosition.y];
}

float CombatCommander::getAirFromAirCombatInfluence(CCTilePosition tilePosition) const
{
	if (!m_bot.Map().isValidTile(tilePosition))
		return 0.f;
	return m_airFromAirCombatInfluenceMap[tilePosition.x][tilePosition.y];
}

float CombatCommander::getGroundEffectInfluence(CCTilePosition tilePosition) const
{
	if (!m_bot.Map().isValidTile(tilePosition))
		return 0.f;
	return m_groundEffectInfluenceMap[tilePosition.x][tilePosition.y];
}

float CombatCommander::getAirEffectInfluence(CCTilePosition tilePosition) const
{
	if (!m_bot.Map().isValidTile(tilePosition))
		return 0.f;
	return m_airEffectInfluenceMap[tilePosition.x][tilePosition.y];
}

float CombatCommander::getGroundFromGroundCloakedCombatInfluence(CCTilePosition tilePosition) const
{
	if (!m_bot.Map().isValidTile(tilePosition))
		return 0.f;
	return m_groundFromGroundCloakedCombatInfluenceMap[tilePosition.x][tilePosition.y];
}

bool CombatCommander::isTileBlocked(int x, int y)
{
	if (m_blockedTiles.size() <= 0)
	{
		return false;
	}
	return m_blockedTiles[x][y];
}

void CombatCommander::drawCombatInformation()
{
    if (m_bot.Config().DrawCombatInformation)
    {
		const auto str = "Win simulation: " + std::to_string(m_winAttackSimulation);
		const auto color = m_winAttackSimulation ? sc2::Colors::Green : sc2::Colors::Red;
		m_bot.Map().drawTextScreen(0.25f, 0.01f, str, color);
    }
}

CCPosition CombatCommander::getMainAttackLocation()
{
    const BaseLocation * enemyBaseLocation = m_bot.Bases().getPlayerStartingBaseLocation(Players::Enemy);

    // First choice: Attack an enemy region if we can see units inside it
    if (enemyBaseLocation)
    {
        CCPosition enemyBasePosition = enemyBaseLocation->getPosition();
        // If the enemy base hasn't been seen yet, go there.
        if (!m_bot.Map().isExplored(enemyBasePosition))
        {
            return enemyBasePosition;
        }
        else
        {
            // if it has been explored, go there if there are any visible enemy units there
            for (auto & enemyUnit : m_bot.UnitInfo().getUnits(Players::Enemy))
            {
                if (enemyUnit.getType().isBuilding() && Util::DistSq(enemyUnit, enemyBasePosition) < 6.f * 6.f)
                {
                    return enemyBasePosition;
                }
            }
        }

		if (!m_bot.Strategy().shouldFocusBuildings())
		{
			m_bot.Actions()->SendChat("Looks like you lost your main base, time to concede? :)");
			m_bot.Strategy().setFocusBuildings(true);
		}
    }

	const CCPosition mainAttackSquadCenter = m_squadData.getSquad("MainAttack").calcCenter();
	float lowestDistance = -1.f;
	CCPosition closestEnemyPosition;

    // Second choice: Attack known enemy buildings
	Squad& mainAttackSquad = m_squadData.getSquad("MainAttack");
    for (const auto & enemyUnit : mainAttackSquad.getTargets())
    {
        if (enemyUnit.getType().isBuilding() && enemyUnit.isAlive() && enemyUnit.getUnitPtr()->display_type != sc2::Unit::Hidden)
        {
			if (enemyUnit.getType().isCreepTumor())
				continue;
			float dist = Util::DistSq(enemyUnit, mainAttackSquadCenter);
			if(lowestDistance < 0 || dist < lowestDistance)
			{
				lowestDistance = dist;
				closestEnemyPosition = enemyUnit.getPosition();
			}
        }
    }
	if (lowestDistance >= 0.f)
	{
		return closestEnemyPosition;
	}

    // Third choice: Attack visible enemy units that aren't overlords
	for (const auto & enemyUnit : mainAttackSquad.getTargets())
	{
        if (!enemyUnit.getType().isOverlord() && enemyUnit.isAlive() && enemyUnit.getUnitPtr()->display_type != sc2::Unit::Hidden)
        {
			if (enemyUnit.getType().isCreepTumor())
				continue;
			float dist = Util::DistSq(enemyUnit, mainAttackSquadCenter);
			if (lowestDistance < 0 || dist < lowestDistance)
			{
				lowestDistance = dist;
				closestEnemyPosition = enemyUnit.getPosition();
			}
        }
    }
	if (lowestDistance >= 0.f)
	{
		return closestEnemyPosition;
	}

    // Fourth choice: We can't see anything so explore the map attacking along the way
	return exploreMap();
}

CCPosition CombatCommander::exploreMap()
{
	// Hack to prevent our units from hugging observers that they can't kill 
	if (!m_bot.Strategy().shouldProduceAntiAirOffense())
		m_bot.Strategy().setShouldProduceAntiAirOffense(true);

	const CCPosition basePosition = Util::GetPosition(m_bot.Bases().getBasePosition(Players::Enemy, m_currentBaseExplorationIndex));
	for (auto & unit : m_combatUnits)
	{
		if (Util::DistSq(unit.getPosition(), basePosition) < 3.f * 3.f)
		{
			m_currentBaseExplorationIndex = (m_currentBaseExplorationIndex + 1) % m_bot.Bases().getBaseLocations().size();
			return Util::GetPosition(m_bot.Bases().getBasePosition(Players::Enemy, m_currentBaseExplorationIndex));
		}
	}
	return basePosition;
}

CCPosition CombatCommander::GetClosestEnemyBaseLocation()
{
	const auto base = m_bot.Bases().getPlayerStartingBaseLocation(Players::Self);
	if (!base)
		return getMainAttackLocation();

	BaseLocation * closestEnemyBase = nullptr;
	float closestDistance = 0.f;
	const auto & baseLocations = m_bot.Bases().getOccupiedBaseLocations(Players::Enemy);
	for(const auto baseLocation : baseLocations)
	{
		const auto dist = Util::DistSq(baseLocation->getPosition(), base->getPosition());
		if(!closestEnemyBase || dist < closestDistance)
		{
			closestEnemyBase = baseLocation;
			closestDistance = dist;
		}
	}

	if (!closestEnemyBase)
		return getMainAttackLocation();

	const auto depotPosition = Util::GetPosition(closestEnemyBase->getDepotTilePosition());
	const auto position = depotPosition + Util::Normalized(depotPosition - closestEnemyBase->getPosition()) * 3.f;
	return position;
}

CCPosition CombatCommander::GetNextBaseLocationToScout()
{
	const auto & baseLocations = m_bot.Bases().getBaseLocations();

	if(baseLocations.size() == m_visitedBaseLocations.size())
	{
		m_visitedBaseLocations.clear();
	}

	CCPosition targetBasePosition;
	auto & squad = m_squadData.getSquad("Scout");
	if (!squad.getUnits().empty())
	{
		float minDistance = 0.f;
		const auto & scoutUnit = squad.getUnits()[0];
		for(auto baseLocation : baseLocations)
		{
			const bool visited = Util::Contains(baseLocation, m_visitedBaseLocations);
			if(visited)
			{
				continue;
			}
			if (baseLocation->isOccupiedByPlayer(Players::Enemy) ||
				baseLocation->isOccupiedByPlayer(Players::Self) ||
				Util::DistSq(scoutUnit, baseLocation->getPosition()) < 5.f * 5.f)
			{
				m_visitedBaseLocations.push_back(baseLocation);
				continue;
			}
			const float distance = Util::DistSq(scoutUnit, baseLocation->getPosition());
			if(targetBasePosition == CCPosition() || distance < minDistance)
			{
				minDistance = distance;
				targetBasePosition = baseLocation->getPosition();
			}
		}
	}
	if(targetBasePosition == CCPosition())
	{
		targetBasePosition = GetClosestEnemyBaseLocation();
	}
	return targetBasePosition;
}

void CombatCommander::SetLogVikingActions(bool log)
{
	m_logVikingActions = log;
}

bool CombatCommander::ShouldSkipFrame(const sc2::Unit * rangedUnit) const
{
	const uint32_t availableFrame = nextCommandFrameForUnit.find(rangedUnit) != nextCommandFrameForUnit.end() ? nextCommandFrameForUnit.at(rangedUnit) : 0;
	return m_bot.GetGameLoop() < availableFrame;
}

bool CombatCommander::PlanAction(const sc2::Unit* rangedUnit, RangedUnitAction action)
{
	auto & currentAction = unitActions[rangedUnit];
	// If the unit is already performing the same action, we do nothing
	if (currentAction == action)
	{
		// Just reset the priority
		currentAction.prioritized = action.prioritized;
		return false;
	}

	// If the unit is performing a priorized action
	if (currentAction.prioritized && !action.prioritized)
	{
		return false;
	}

	// The current action is not yet finished and the new one is not prioritized
	if (currentAction.executed && !currentAction.finished && !action.prioritized)
	{
		return false;
	}

	unitActions[rangedUnit] = action;
	return true;
}

void CombatCommander::CleanActions(const std::vector<Unit> &combatUnits)
{
	sc2::Units units;
	Util::CCUnitsToSc2Units(combatUnits, units);
	sc2::Units unitsToClear;
	for (auto & unitAction : unitActions)
	{
		const auto rangedUnit = unitAction.first;
		auto & action = unitAction.second;

		// If the unit is dead, we will need to remove it from the map
		if (!rangedUnit->is_alive)
		{
			unitsToClear.push_back(rangedUnit);
			continue;
		}

		// If the unit is no longer in this squad
		if (!Util::Contains(rangedUnit, units))
		{
			unitsToClear.push_back(rangedUnit);
			continue;
		}

		// Sometimes want to give an action only every few frames to allow slow attacks to occur and cliff jumps
		if (ShouldSkipFrame(rangedUnit))
			continue;
		
		// Reset the priority of the action because it is finished
		action.prioritized = false;
		action.finished = true;
	}

	for (auto unit : unitsToClear)
	{
		unitActions.erase(unit);
	}
}

void CombatCommander::ExecuteActions()
{
	for (auto & unitAction : unitActions)
	{
		const auto rangedUnit = unitAction.first;
		auto & action = unitAction.second;

		// Uninitialized action
		if (action.description.empty())
			continue;

		if (m_logVikingActions && rangedUnit->unit_type == sc2::UNIT_TYPEID::TERRAN_VIKINGFIGHTER)
		{
			std::stringstream ss;
			ss << sc2::UnitTypeToName(rangedUnit->unit_type) << " has action " << action.description;
			Util::Log(__FUNCTION__, ss.str(), m_bot);
		}

#ifndef PUBLIC_RELEASE
		if (m_bot.Config().DrawRangedUnitActions)
		{
			const std::string actionString = MicroActionTypeAccronyms[action.microActionType] + (action.prioritized ? "!" : "");
			m_bot.Map().drawText(rangedUnit->pos, actionString, sc2::Colors::Teal);
		}
#endif

		// If the action has already been executed and it is not time to reexecute it
		if (action.executed && (action.duration >= ACTION_REEXECUTION_FREQUENCY || m_bot.GetGameLoop() - action.executionFrame < ACTION_REEXECUTION_FREQUENCY))
			continue;

		std::stringstream ss;
		bool skip = false;
		m_bot.GetCommandMutex().lock();
		switch (action.microActionType)
		{
		case MicroActionType::AttackMove:
			if (!rangedUnit->orders.empty() && rangedUnit->orders[0].ability_id == sc2::ABILITY_ID::ATTACK && rangedUnit->orders[0].target_unit_tag == 0)
			{
				const auto orderPos = rangedUnit->orders[0].target_pos;
				const auto orderDirection = Util::Normalized(orderPos - rangedUnit->pos);
				const auto actionDirection = Util::Normalized(action.position - rangedUnit->pos);
				const auto sameDirection = sc2::Dot2D(orderDirection, actionDirection) > 0.95f;
				const auto dist = Util::DistSq(orderPos, action.position);
				if (sameDirection && dist < 1)
					skip = true;
			}
			if (!skip)
			{
				if (rangedUnit->unit_type == sc2::UNIT_TYPEID::TERRAN_BATTLECRUISER)
					Micro::SmartMove(rangedUnit, action.position, m_bot);
				else
					Micro::SmartAttackMove(rangedUnit, action.position, m_bot);
			}
			break;
		case MicroActionType::AttackUnit:
			if (!rangedUnit->orders.empty() && rangedUnit->orders[0].ability_id == sc2::ABILITY_ID::ATTACK && rangedUnit->orders[0].target_unit_tag == action.target->tag)
				skip = true;
			if (!skip)
				Micro::SmartAttackUnit(rangedUnit, action.target, m_bot);
			break;
		case MicroActionType::Move:
			if (!rangedUnit->orders.empty() && rangedUnit->orders[0].ability_id == sc2::ABILITY_ID::MOVE)
			{
				const auto orderPos = rangedUnit->orders[0].target_pos;
				const auto orderDirection = Util::Normalized(orderPos - rangedUnit->pos);
				const auto actionDirection = Util::Normalized(action.position - rangedUnit->pos);
				const auto sameDirection = sc2::Dot2D(orderDirection, actionDirection) > 0.95f;
				const auto dist = Util::DistSq(orderPos, action.position);
				if (sameDirection && dist < 1)
					skip = true;
			}
			if (!skip)
				Micro::SmartMove(rangedUnit, action.position, m_bot);
			break;
		case MicroActionType::Ability:
			Micro::SmartAbility(rangedUnit, action.abilityID, m_bot);
			break;
		case MicroActionType::AbilityPosition:
			Micro::SmartAbility(rangedUnit, action.abilityID, action.position, m_bot);
			break;
		case MicroActionType::AbilityTarget:
			Micro::SmartAbility(rangedUnit, action.abilityID, action.target, m_bot);
			break;
		case MicroActionType::Stop:
			//Micro::SmartStop(rangedUnit, m_bot);
			ss << "MicroAction of type STOP with " << action.description << " sent to a " << sc2::UnitTypeToName(rangedUnit->unit_type);
			Util::Log(__FUNCTION__, ss.str(), m_bot);
			break;
		case MicroActionType::RightClick:
			Micro::SmartRightClick(rangedUnit, action.target, m_bot);
			break;
		case MicroActionType::ToggleAbility:
			Micro::SmartToggleAutoCast(rangedUnit, action.abilityID, m_bot);
			break;
		default:
			const int type = action.microActionType;
			Util::Log(__FUNCTION__, "Unknown MicroActionType: " + std::to_string(type), m_bot);
			break;
		}
		m_bot.GetCommandMutex().unlock();

		if (!skip)
		{
			action.executed = true;
			action.executionFrame = m_bot.GetGameLoop();
			if (action.duration > 0)
			{
				nextCommandFrameForUnit[rangedUnit] = m_bot.GetGameLoop() + action.duration;
			}
		}
	}
}

RangedUnitAction& CombatCommander::GetRangedUnitAction(const sc2::Unit * combatUnit)
{
	return unitActions[combatUnit];
}

void CombatCommander::CleanLockOnTargets() const
{
	auto & lockOnTargets = m_bot.Commander().Combat().getLockOnTargets();
	for (auto it = lockOnTargets.cbegin(), next_it = it; it != lockOnTargets.cend(); it = next_it)
	{
		++next_it;
		if (!it->first->is_alive)
		{
			lockOnTargets.erase(it);
		}
	}
}

void CombatCommander::CalcBestFlyingCycloneHelpers()
{
	m_cycloneFlyingHelpers.clear();
	m_cyclonesWithHelper.clear();
	CleanLockOnTargets();

	// Get the Cyclones and their potential flying helpers in the squad
	std::set<const sc2::Unit *> cyclones;
	std::set<const sc2::Unit *> potentialFlyingCycloneHelpers;
	for (const auto unit : m_combatUnits)
	{
		const auto unitPtr = unit.getUnitPtr();
		const auto type = unitPtr->unit_type;

		if (ShouldUnitHeal(unitPtr) || Util::isUnitLockedOn(unitPtr))
			continue;

		if (type == sc2::UNIT_TYPEID::TERRAN_CYCLONE)
		{
			cyclones.insert(unitPtr);
		}
		else if (unit.isFlying())
		{
			auto squad = m_squadData.getUnitSquad(unit);
			if (squad && !Util::StringStartsWith(squad->getName(), "Harass"))
			{
				potentialFlyingCycloneHelpers.insert(unitPtr);
			}
		}
	}

	if (cyclones.empty())
		return;

	// Gather Cyclones' targets
	const auto & lockOnTargets = m_bot.Commander().Combat().getLockOnTargets();
	std::set<const sc2::Unit *> targets;
	for (const auto & cyclone : lockOnTargets)
	{
		// The Cyclone's target will need to be followed
		targets.insert(cyclone.second.first);
		// Cyclone does not need to be followed anymore because it has a target
		cyclones.erase(cyclone.first);
	}

	// Find clusters of targets to use less potential helpers
	sc2::Units targetsVector;
	for (const auto target : targets)
		targetsVector.push_back(target);
	const auto targetClusters = Util::GetUnitClusters(targetsVector, {}, true, m_bot);

	// Choose the best air unit to keep vision of Cyclone's targets
	for (const auto & targetCluster : targetClusters)
	{
		const sc2::Unit * closestHelper = nullptr;
		float smallestDistSq = 0.f;
		for (const auto potentialHelper : potentialFlyingCycloneHelpers)
		{
			const float distSq = Util::DistSq(targetCluster.m_center, potentialHelper->pos);
			if (!closestHelper || distSq < smallestDistSq)
			{
				closestHelper = potentialHelper;
				smallestDistSq = distSq;
			}
		}
		if (closestHelper)
		{
			// Remove the helper from the set because it is now taken
			potentialFlyingCycloneHelpers.erase(closestHelper);
			// Save the helper
			m_cycloneFlyingHelpers[closestHelper] = FlyingHelperMission(TRACK, targetCluster.m_center);
			// Associate the helper with every Cyclone that had a target in that target cluster
			for (const auto target : targetCluster.m_units)
			{
				for (const auto & cyclone : lockOnTargets)
				{
					if (target == cyclone.second.first)
					{
						m_cyclonesWithHelper[cyclone.first] = closestHelper;
					}
				}
			}
		}
	}

	// If there are Cyclones without target, follow them to give them vision
	if (!cyclones.empty() && !potentialFlyingCycloneHelpers.empty())
	{
		// Do not consider Cyclones without target that are already near a helper
		sc2::Units cyclonesVector;
		for (const auto cyclone : cyclones)
		{
			bool covered = false;
			for (const auto & helper : m_cycloneFlyingHelpers)
			{
				if (Util::DistSq(helper.first->pos, cyclone->pos) < CYCLONE_PREFERRED_MAX_DISTANCE_TO_HELPER * CYCLONE_PREFERRED_MAX_DISTANCE_TO_HELPER)
				{
					// that cyclone doesn't need help from another flying unit
					covered = true;
					m_cyclonesWithHelper[cyclone] = helper.first;
					break;
				}
			}
			if (!covered)
				cyclonesVector.push_back(cyclone);
		}

		// Find clusters of Cyclones without target to use less potential helpers
		const auto cycloneClustersVector = Util::GetUnitClusters(cyclonesVector, { sc2::UNIT_TYPEID::TERRAN_CYCLONE }, false, m_bot);
		std::list<Util::UnitCluster> cycloneClusters(cycloneClustersVector.begin(), cycloneClustersVector.end());

		if (potentialFlyingCycloneHelpers.size() <= cycloneClusters.size())
		{
			// Choose the best air unit to give vision to Cyclones without target
			for (const auto potentialHelper : potentialFlyingCycloneHelpers)
			{
				Util::UnitCluster closestCluster;
				float smallestDistSq = 0.f;
				for (const auto & cycloneCluster : cycloneClusters)
				{
					const float distSq = Util::DistSq(potentialHelper->pos, cycloneCluster.m_center);
					if (closestCluster.m_units.empty() || distSq < smallestDistSq)
					{
						closestCluster = cycloneCluster;
						smallestDistSq = distSq;
					}
				}
				if (!closestCluster.m_units.empty())
				{
					// Remove the helper from the set because it is now taken
					cycloneClusters.remove(closestCluster);
					// Save the helper
					m_cycloneFlyingHelpers[potentialHelper] = FlyingHelperMission(ESCORT, closestCluster.m_center);
					// Associate the helper with every Cyclone in that Cyclone cluster
					for (const auto cyclone : closestCluster.m_units)
					{
						m_cyclonesWithHelper[cyclone] = potentialHelper;
					}
				}
			}
		}
		else
		{
			// Choose the best air unit to give vision to Cyclones without target
			for (const auto & cycloneCluster : cycloneClusters)
			{
				const sc2::Unit * closestHelper = nullptr;
				float smallestDistSq = 0.f;
				for (const auto potentialHelper : potentialFlyingCycloneHelpers)
				{
					const float distSq = Util::DistSq(potentialHelper->pos, cycloneCluster.m_center);
					if (!closestHelper || distSq < smallestDistSq)
					{
						closestHelper = potentialHelper;
						smallestDistSq = distSq;
					}
				}
				if (closestHelper)
				{
					// Remove the helper from the set because it is now taken
					potentialFlyingCycloneHelpers.erase(closestHelper);
					// Save the helper
					m_cycloneFlyingHelpers[closestHelper] = FlyingHelperMission(ESCORT, cycloneCluster.m_center);
					// Associate the helper with every Cyclone in that Cyclone cluster
					for (const auto cyclone : cycloneCluster.m_units)
					{
						m_cyclonesWithHelper[cyclone] = closestHelper;
					}
				}
			}
		}
	}
}

bool CombatCommander::ShouldUnitHeal(const sc2::Unit * unit) const
{
	auto & unitsBeingRepaired = m_bot.Commander().Combat().getUnitsBeingRepaired();
	const UnitType unitType(unit->unit_type, m_bot);
	const bool hasBaseOrMinerals = m_bot.Bases().getBaseCount(Players::Self, false) > 0 || m_bot.GetFreeMinerals() >= 450;
	if (unitType.isRepairable() && !unitType.isBuilding() && hasBaseOrMinerals)
	{
		const auto it = unitsBeingRepaired.find(unit);
		//If unit is being repaired
		if (it != unitsBeingRepaired.end())
		{
			//and is not fully repaired
			if (unit->health != unit->health_max)
			{
				return true;
			}
			else
			{
				unitsBeingRepaired.erase(unit);
			}
		}
		//if unit is damaged enough to go back for repair
		else
		{
			float percentageMultiplier = 1.f;
			bool forceHeal = false;
			switch (unit->unit_type.ToType())
			{
			case sc2::UNIT_TYPEID::TERRAN_BATTLECRUISER:
				if (m_bot.Config().StarCraft2Version <= "4.10.4")
					percentageMultiplier = 0.5f;
				else if (m_bot.Analyzer().getUnitState(unit).GetRecentDamageTaken() * 1.8f >= unit->health
					&& Util::IsAbilityAvailable(sc2::ABILITY_ID::EFFECT_TACTICALJUMP, unit, m_unitsAbilities))
					forceHeal = true;	// After version 4.10.4, Tactical Jump has a 1 second vulnerability
				break;
			case sc2::UNIT_TYPEID::TERRAN_CYCLONE:
				if (m_bot.Commander().Combat().getLockOnTargets().find(unit) != m_bot.Commander().Combat().getLockOnTargets().end())
					return false;	// Cyclones with lock-on target should not go back to heal
				percentageMultiplier = 1.5f;
				break;
			default:
				break;
			}
			if (forceHeal || unit->health / unit->health_max < Util::HARASS_REPAIR_STATION_MAX_HEALTH_PERCENTAGE * percentageMultiplier)
			{
				unitsBeingRepaired.insert(unit);
				return true;
			}
		}
	}

	return unit->unit_type == sc2::UNIT_TYPEID::TERRAN_REAPER && unit->health / unit->health_max < 0.66f;
}

bool CombatCommander::GetUnitAbilities(const sc2::Unit * unit, sc2::AvailableAbilities & outUnitAbilities) const
{
	for (const auto & availableAbilitiesForUnit : m_unitsAbilities)
	{
		if (availableAbilitiesForUnit.unit_tag == unit->tag)
		{
			outUnitAbilities = availableAbilitiesForUnit;
			return true;
		}
	}
	return false;
}