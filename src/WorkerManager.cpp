#include <algorithm>
#include "WorkerManager.h"
#include "CCBot.h"
#include "Util.h"
#include "Building.h"
#include "Micro.h"

WorkerManager::WorkerManager(CCBot & bot)
    : m_bot         (bot)
    , m_workerData  (bot)
{

}

void WorkerManager::onStart()
{

}

void WorkerManager::onFrame(bool executeMacro)
{
	m_bot.StartProfiling("0.7.1   m_workerData.updateAllWorkerData");
    m_workerData.updateAllWorkerData();
	m_bot.StopProfiling("0.7.1   m_workerData.updateAllWorkerData");
	if (executeMacro)
	{
		m_bot.StartProfiling("0.7.2   handleMineralWorkers");
		handleMineralWorkers();
		m_bot.StopProfiling("0.7.2   handleMineralWorkers");
		m_bot.StartProfiling("0.7.3   handleGasWorkers");
		handleGasWorkers();
		m_bot.StopProfiling("0.7.3   handleGasWorkers");
		m_bot.StartProfiling("0.7.4   handleIdleWorkers");
		handleIdleWorkers();
		m_bot.StopProfiling("0.7.4   handleIdleWorkers");
		m_bot.StartProfiling("0.7.5   repairCombatBuildings");
		repairCombatBuildings();
		m_bot.StopProfiling("0.7.5   repairCombatBuildings");
		m_bot.StartProfiling("0.7.6   lowPriorityChecks");
		lowPriorityChecks();
		m_bot.StopProfiling("0.7.6   lowPriorityChecks");
		m_bot.StartProfiling("0.7.7   handleRepairWorkers");
		handleRepairWorkers();
		m_bot.StopProfiling("0.7.7   handleRepairWorkers");
	}
    drawResourceDebugInfo();
    drawWorkerInformation();

    m_workerData.drawDepotDebugInfo();
}

void WorkerManager::lowPriorityChecks()
{
	int currentFrame = m_bot.GetCurrentFrame();
	if (currentFrame - m_lastLowPriorityCheckFrame < 48)
	{
		return;
	}
	m_lastLowPriorityCheckFrame = currentFrame;

	//Detect depleted geysers
	for (auto & geyser : m_bot.GetAllyGeyserUnits())
	{
		//if Depleted
		if (geyser.getUnitPtr()->vespene_contents == 0)
		{
			//Salvage gas bunkers, if any
			auto base = m_bot.Bases().getBaseContainingPosition(geyser.getPosition(), Players::Self);
			auto gasBunkers = base->getGasBunkers();
			for (auto & bunker : gasBunkers)
			{
				bunker.useAbility(sc2::ABILITY_ID::EFFECT_SALVAGE);
			}
		}
	}

	//Worker split between bases (transfer worker)
	if (m_bot.Bases().getBaseCount(Players::Self, true) <= 1)
	{//No point trying to split workers
		return;
	}
	bool needTransfer = false;
	auto workers = getWorkers();
	WorkerData workerData = m_bot.Workers().getWorkerData();
	auto & bases = m_bot.Bases().getOccupiedBaseLocations(Players::Self);
	std::list<Unit> dispatchedWorkers;
	for (auto & base : bases)
	{
		auto basePosition = base->getPosition();
		int optimalWorkers = base->getOptimalMineralWorkerCount();
		auto depot = getDepotAtBasePosition(basePosition);
		int workerCount = m_workerData.getNumAssignedWorkers(depot);
		if (workerCount > optimalWorkers)
		{
			int extra = workerCount - optimalWorkers;
			for (auto & worker : workers)///TODO order by closest to the target base location
			{
				if (m_bot.Workers().isFree(worker))
				{
					const auto workerDepot = m_workerData.getWorkerDepot(worker);
					if (workerDepot.isValid() && workerDepot.getID() == depot.getID())
					{
						dispatchedWorkers.push_back(worker);
						extra--;
						if (extra <= 0)
						{
							break;
						}
					}
				}
			}
		}
		else
		{
			needTransfer = true;
		}
	}

	//Dispatch workers to bases missing some
	if (needTransfer)
	{
		for (auto & base : bases)
		{
			if (base->isUnderAttack())
			{
				continue;
			}

			auto depot = getDepotAtBasePosition(base->getPosition());
			if (!depot.isValid() || depot.isBeingConstructed())
			{
				continue;
			}

			int workerCount = m_workerData.getNumAssignedWorkers(depot);
			int optimalWorkers = base->getMinerals().size() * 2;
			if (workerCount < optimalWorkers)
			{
				std::vector<Unit> toRemove;
				int needed = optimalWorkers - workerCount;
				int moved = 0;
				for (auto it = dispatchedWorkers.begin(); it != dispatchedWorkers.end(); it++)
				{
					//Dont move workers if its not safe
					if (!Util::PathFinding::IsPathToGoalSafe(it->getUnitPtr(), base->getPosition(), true, m_bot))
					{
						continue;
					}

					m_workerData.setWorkerJob(*it, WorkerJobs::Minerals, depot, true);
					toRemove.push_back(*it);
					moved++;
					if (moved >= needed)
					{
						break;
					}
				}

				//remove already dispatched workers
				for (auto worker : toRemove)
				{
					auto it = find(dispatchedWorkers.begin(), dispatchedWorkers.end(), worker);
					if (it != dispatchedWorkers.end())
					{
						dispatchedWorkers.erase(it);
					}
				}
				if (dispatchedWorkers.empty())
				{
					break;
				}
			}
		}
	}

	workerData.validateRepairStationWorkers();
}

void WorkerManager::setRepairWorker(Unit worker, const Unit & unitToRepair)
{
    m_workerData.setWorkerJob(worker, WorkerJobs::Repair, unitToRepair);
}

void WorkerManager::stopRepairing(const Unit & worker)
{
    m_workerData.WorkerStoppedRepairing(worker);
	if (m_workerData.isProxyWorker(worker))
		return;
    finishedWithWorker(worker);
}

void WorkerManager::handleMineralWorkers()
{
	handleMules();

	Unit proxyWorker;
	// Send second proxy worker for proxy Marauders strategy
	if (m_bot.Strategy().getStartingStrategy() == PROXY_MARAUDERS 
		&& m_workerData.getProxyWorkers().size() == 1 
		&& m_bot.UnitInfo().getUnitTypeCount(Players::Self, MetaTypeEnum::SupplyDepot.getUnitType(), true, true) == 1
		&& m_bot.UnitInfo().getUnitTypeCount(Players::Self, MetaTypeEnum::Barracks.getUnitType(), true, true) < 1)
	{
		float minDist = 0.f;
		const auto & workers = getWorkers();
		const auto rampPosition = Util::GetPosition(m_bot.Buildings().getWallPosition());
		for (const auto & worker : workers)
		{
			const auto dist = Util::DistSq(worker, rampPosition);
			if (!proxyWorker.isValid() || dist < minDist)
			{
				minDist = dist;
				proxyWorker = worker;
			}
		}
		proxyWorker.move(m_bot.Buildings().getProxyLocation());
		m_workerData.setProxyWorker(proxyWorker);
	}

	//split workers on first frame
	//TODO can be improved by preventing the worker from retargetting a very far mineral patch
	if (!m_isFirstFrame || (int)m_bot.GetCurrentFrame() == 0)
	{
		return;
	}
	m_isFirstFrame = false;

	m_bot.StartProfiling("0.7.2.2     selectMinerals");
	std::list<Unit> minerals;
	std::map<Unit, int> mineralsUsage;
	for (auto& base : m_bot.Bases().getBaseLocations())
	{
		if (base->isOccupiedByPlayer(Players::Self))
		{
			auto mineralsVector = base->getMinerals();
			for (auto& t : mineralsVector)
			{
				minerals.push_back(t);
				mineralsUsage[t] = 0;
			}
		}
	}
	m_bot.StopProfiling("0.7.2.2     selectMinerals");

	if (m_bot.Strategy().isProxyStartingStrategy())
	{
		float minDist = 0.f;
		const auto & workers = getWorkers();
		const auto rampPosition = Util::GetPosition(m_bot.Buildings().getWallPosition());
		for (const auto & worker : workers)
		{
			const auto dist = Util::DistSq(worker, rampPosition);
			if (!proxyWorker.isValid() || dist < minDist)
			{
				minDist = dist;
				proxyWorker = worker;
			}
		}
		proxyWorker.move(m_bot.Buildings().getProxyLocation());
		m_workerData.setProxyWorker(proxyWorker);
	}

	m_bot.StartProfiling("0.7.2.3     orderedMineralWorkers");
	std::map<Unit, int> workerMineralDistance;
	for (auto & worker : getWorkers())
	{
		if (worker == proxyWorker)
			continue;
		CCPosition position = worker.getPosition();
		int maxDist = -1;
		for (auto& mineral : mineralsUsage)
		{
			int distance = Util::DistSq(position, mineral.first.getPosition());
			if (distance > maxDist)
			{
				maxDist = distance;
			}
		}
		workerMineralDistance[worker] = maxDist;
	}

	//Sorting the workers by furthest distance
	//Defining a lambda function to compare two pairs. It will compare two pairs using the value
	Comparator compFunctor = [](std::pair<Unit, int> elem1, std::pair<Unit, int> elem2)
	{
		return elem1.second > elem2.second;
	};

	//Declaring a set that will store the pairs using above comparision logic
	std::set<std::pair<Unit, int>, Comparator> orderedMineralWorkers(workerMineralDistance.begin(), workerMineralDistance.end(), compFunctor);
	m_bot.StopProfiling("0.7.2.3     orderedMineralWorkers");

	m_bot.StartProfiling("0.7.2.4     splitMineralWorkers");
	for (auto& mineralWorker : orderedMineralWorkers)
	{
		auto worker = mineralWorker.first;
		if (!worker.isValid())
		{
			continue;
		}

		int lowest = 200;
		std::list<Unit> lowestMinerals;
		for (auto& mineral : mineralsUsage)
		{
			if (mineral.second < lowest)
			{
				lowest = mineral.second;
				lowestMinerals.clear();
			}
			if (mineral.second == lowest)
			{
				lowestMinerals.push_back(mineral.first);
			}
		}

		auto closestMineral = getClosest(worker, minerals);
		mineralsUsage[closestMineral]++;
		if (mineralsUsage[closestMineral] >= 2)
		{
			minerals.remove(closestMineral);
		}

		worker.rightClick(closestMineral);
	}
	m_bot.StopProfiling("0.7.2.4     splitMineralWorkers");
}

void WorkerManager::handleMules()
{
	for (auto mule : m_bot.GetAllyUnits(sc2::UNIT_TYPEID::TERRAN_MULE))
	{
		if (!mule.isValid())
		{
			continue;
		}

		auto id = mule.getID();
		if (muleHarvests.find(id) == muleHarvests.end())
		{
			muleHarvests[id] = std::pair<bool, std::pair<int, sc2::Tag>>();
		}
		else
		{
			//Do not run on first frame of Mule existance, or we might assign it to the wrong mineral
			auto unit = mule.getUnitPtr();
			if (unit->orders.size() == 0)
			{
				auto mineral = m_bot.Buildings().getClosestMineral(unit->pos);
				muleHarvests[id].second.second = mineral->tag;
				mule.rightClick(mineral->pos);
			}
		}

		if (isReturningCargo(mule))
		{
			muleHarvests[id].first = true;
		}
		else
		{
			if (muleHarvests[id].first)//Cargo was returned
			{
				muleHarvests[id].first = false;
				muleHarvests[id].second.first++;
				if (muleHarvests[id].second.first == 9)//Maximum of 9 harvest per mule, the mules can't finish the 10th.
				{
					auto position = m_bot.Map().center();
					mule.move(position);	
					muleHarvests.erase(id);
				}
			}
		}
	}
}

void WorkerManager::handleGasWorkers()
{
	int mineral = m_bot.GetMinerals();
	int gas = m_bot.GetGas();
	const int ressourceTreshold = 300;
	int previousGasWorkersTarget = gasWorkersTarget;

	switch (gasWorkersTarget)
	{
		case 0:
			if (mineral > gas)
			{
				gasWorkersTarget = 1;
			}
			break;
		case 1:
			if (gas > mineral * 5 && mineral + gas > ressourceTreshold)
			{
				if (getNumMineralWorkers() <= 10)
				{
					gasWorkersTarget = 0;
				}
				else
				{
					gasWorkersTarget = 1;
				}
			}
			else if (mineral > gas * 1)
			{
				gasWorkersTarget = 2;
			}
			break;
		case 2:
			if (gas > mineral * 3 && mineral + gas > ressourceTreshold)
			{
				gasWorkersTarget = 1;
			}
			else if (mineral > gas * 1.5f)
			{
				gasWorkersTarget = 3;
			}
			break;
		case 3:
			if (gas > mineral * 2 && mineral + gas > ressourceTreshold)
			{
				gasWorkersTarget = 2;
			}
			break;
		default:
			gasWorkersTarget = 3;
	}

	if (m_bot.Strategy().isWorkerRushed())
	{
		gasWorkersTarget = 3;
	}

    // for each unit we have
	for (auto & geyser : m_bot.GetAllyGeyserUnits())
    {
        // if that unit is a refinery
        if (geyser.isCompleted() && geyser.getUnitPtr()->vespene_contents > 0)
        {
			auto geyserPosition = geyser.getPosition();
			auto base = m_bot.Bases().getBaseContainingPosition(geyserPosition, Players::Self);

            // get the number of workers currently assigned to it
            int numAssigned = m_workerData.getNumAssignedWorkers(geyser);

			//TODO doesn't handle split geysers if only one of the geysers has a bunker.
			//Bunker counts as a worker (for 2 and 3 only, we still want 1 worker at 1)
			int geyserGasWorkersTarget = (base->getGasBunkers().size() > 0 && gasWorkersTarget > 1 ? gasWorkersTarget - 1 : gasWorkersTarget);

			if (base == nullptr)
			{
				//if the base is destroyed, remove the gas workers
				for (int i = 0; i < numAssigned; i++)
				{
					auto gasWorker = getGasWorker(geyser, false, false);
					m_workerData.setWorkerJob(gasWorker, WorkerJobs::Idle);
				}
			}
			else
			{
				auto & depot = base->getResourceDepot();
				if (depot.isValid() && depot.isCompleted())
				{
					if (numAssigned < geyserGasWorkersTarget)
					{
						// if it's less than we want it to be, fill 'er up
						bool shouldAssignThisWorker = true;
						auto refineryWorkers = m_workerData.getAssignedWorkersRefinery(geyser);
						for (auto & worker : refineryWorkers)
						{
							if (!isInsideGeyser(worker) && !isReturningCargo(worker))
							{
								shouldAssignThisWorker = false;
								break;
							}
						}

						if (shouldAssignThisWorker)
						{
							auto mineralWorker = getMineralWorker(geyser);
							if (mineralWorker.isValid())
							{
								if (Util::PathFinding::IsPathToGoalSafe(mineralWorker.getUnitPtr(), geyserPosition, true, m_bot))
								{
									m_workerData.setWorkerJob(mineralWorker, WorkerJobs::Gas, geyser);
								}
							}
						}
					}
					else if (numAssigned > geyserGasWorkersTarget)
					{
						int mineralWorkerRoom = 26;//Number of free spaces for mineral workers
						int mineralWorkersCount = m_workerData.getNumAssignedWorkers(depot);
						int optimalWorkersCount = base->getOptimalMineralWorkerCount();
						mineralWorkerRoom = optimalWorkersCount - mineralWorkersCount;

						// if it's more than we want it to be, empty it up
						for (int i = 0; i<(numAssigned - geyserGasWorkersTarget); ++i)
						{
							//check if we have room for more mineral workers
							if (mineralWorkerRoom <= 0 && numAssigned < 3)
							{//Do not remove gas workers if we don't have room for an additional mineral worker, except if we are at 3 gas workers (exception for bunkers).
								break;
							}

							auto gasWorker = getGasWorker(geyser, true, true);
							if (gasWorker.isValid())
							{
								if (m_workerData.getWorkerJob(gasWorker) != WorkerJobs::Gas)
								{
									Util::DisplayError(__FUNCTION__, "Worker assigned to a refinery is not a gas worker.", m_bot);
								}

								gasWorker.stop();
							}

							mineralWorkerRoom--;
						}
					}
				}
			}
        }
    }

	std::vector<sc2::Tag> bunkerHasLoaded;
	for (auto & geyser : m_bot.GetAllyGeyserUnits())
	{
		auto base = m_bot.Bases().getBaseContainingPosition(geyser.getPosition(), Players::Self);
		auto depot = base->getResourceDepot();
		bool hasUsableDepot = true;
		if (!depot.isValid() || depot.getUnitPtr()->build_progress < 1)//Do not using the gas bunker with an unfinished or inexistant depot.
		{
			hasUsableDepot = false;
		}

		auto & workers = m_bot.Workers().m_workerData.getAssignedWorkersRefinery(geyser);
		for (auto & bunker : base->getGasBunkers())
		{
			if (!bunker.isCompleted())
			{
				continue;
			}

			if (!hasUsableDepot)//If there is no depot or if its not finished, empty the bunker.
			{
				Micro::SmartAbility(bunker.getUnitPtr(), sc2::ABILITY_ID::UNLOADALL, m_bot);
				continue;
			}

			auto hasUnload = false;
			if (!base->isGeyserSplit())
			{
				for (auto & worker : workers)//Handle workers inside
				{
					if (m_bot.Commander().isInside(worker.getTag()))
					{
						if (!hasUnload)
						{
							if (worker.isReturningCargo())//drop on CC side
							{
								bunker.rightClick(base->getDepotPosition());
								m_bot.Commander().AddDelayedSmartAbility(worker, 0, base->getDepotPosition());
							}
							else//drop on refinery side
							{
								bunker.rightClick(geyser.getPosition());
								m_bot.Commander().AddDelayedSmartAbility(worker, 0, geyser.getPosition());
							}
							Micro::SmartAbility(bunker.getUnitPtr(), sc2::ABILITY_ID::UNLOADALL, m_bot);
							hasUnload = true;
						}
					}
				}
				if (workers.size() == 0)//Empty bunkers if they they units inside that shouldn't be inside
				{
					auto passengers = bunker.getUnitPtr()->passengers;
					for (auto passenger : passengers)
					{
						switch ((sc2::UNIT_TYPEID) passenger.unit_type)
						{
							case sc2::UNIT_TYPEID::TERRAN_SCV:
							case sc2::UNIT_TYPEID::PROTOSS_PROBE:
							case sc2::UNIT_TYPEID::ZERG_DRONE:
								Micro::SmartAbility(bunker.getUnitPtr(), sc2::ABILITY_ID::UNLOADALL, m_bot);
						}
					}
				}

				bool hasReturningWorker = false;
				for (auto & worker : workers)//Handle workers outside
				{
					if (!m_bot.Commander().isInside(worker.getTag()))
					{
						auto distRefinery = Util::DistSq(worker.getPosition(), geyser.getPosition());
						auto distDepot = Util::DistSq(worker.getPosition(), base->getDepotPosition());
						if (worker.isReturningCargo())
						{
							if ((std::find(bunkerHasLoaded.begin(), bunkerHasLoaded.end(), bunker.getTag()) == bunkerHasLoaded.end() || hasReturningWorker)
								&& distRefinery < distDepot)//If the bunker is empty or if there is already a returning worker, click to enter bunker
							{
								worker.rightClick(bunker.getPosition());

								Micro::SmartAbility(bunker.getUnitPtr(), sc2::ABILITY_ID::LOAD, worker.getUnitPtr(), m_bot);
								bunkerHasLoaded.push_back(bunker.getTag());
								hasReturningWorker = true;
							}
							else//Click to drop resource
							{
								Micro::SmartAbility(worker.getUnitPtr(), sc2::ABILITY_ID::HARVEST_RETURN, m_bot);
							}
						}
						else
						{
							if (distRefinery < distDepot)//Click to enter refinery
							{
								Micro::SmartAbility(worker.getUnitPtr(), sc2::ABILITY_ID::HARVEST_GATHER,geyser.getUnitPtr(), m_bot);
							}
							else if(std::find(bunkerHasLoaded.begin(), bunkerHasLoaded.end(), bunker.getTag()) == bunkerHasLoaded.end())//Click to enter bunker
							{
								worker.rightClick(bunker.getPosition());
								Micro::SmartAbility(bunker.getUnitPtr(), sc2::ABILITY_ID::LOAD, worker.getUnitPtr(), m_bot);
								bunkerHasLoaded.push_back(bunker.getTag());
							}
						}
					}
				}
			}
			else
			{
				//UNHANDLED SINGLE GEYSER
			}
		}
	}
}

void WorkerManager::handleIdleWorkers()
{
    // for each of our workers
    for (auto & worker : m_workerData.getWorkers())
    {
        if (!worker.isValid()) { continue; }

		int workerJob = m_workerData.getWorkerJob(worker);
		bool idle = worker.isIdle();
        if (idle &&
            // We need to consider building worker because of builder finishing the job of another worker is not consider idle.
			//(m_workerData.getWorkerJob(worker) != WorkerJobs::Build) && 
            (workerJob != WorkerJobs::Move) &&
            (workerJob != WorkerJobs::Repair) &&
			(workerJob != WorkerJobs::Scout) &&
			(workerJob != WorkerJobs::Build))//Prevent premoved builder from going Idle if they lack the ressources, also prevents refinery builder from going Idle
		{
			m_workerData.setWorkerJob(worker, WorkerJobs::Idle);
			workerJob = WorkerJobs::Idle;
		}
		else if (workerJob == WorkerJobs::Build)
		{
			if (!worker.isConstructingAnything())
			{
				bool hasBuilding = false;
				bool isCloseToBuildingLocation = false;
				if (idle)
				{
					//Check if worker is waiting to build
					for (auto b : m_bot.Buildings().getBuildings())
					{
						if (!b.builderUnit.isValid()) continue;
						if (b.buildingUnit.isValid() && b.buildingUnit.isCompleted()) continue;
						if (b.builderUnit.getTag() == worker.getTag())
						{
							//Do not try to patrol on refinery, could slow down the building creation
							if (!b.type.isRefinery())
							{
								hasBuilding = true;
								if (Util::DistSq(b.builderUnit, Util::GetPosition(b.finalPosition)) < 1.f * 1.f)
								{
									isCloseToBuildingLocation = true;
								}
							}
							break;
						}
					}
				}
				if (hasBuilding)
				{
					if (isCloseToBuildingLocation)
					{
						//Is waiting for resources, so we patrol
						//Patrol requires at least a 1 tile distance, 0.707 is exactly what we need to have a diagonal distance of 1.
						auto patrolTarget = worker.getPosition();
						patrolTarget.x += 0.71f;
						patrolTarget.y += 0.71f;

						worker.patrol(patrolTarget);
					}
				}
				else
				{
					auto orders = worker.getUnitPtr()->orders;
					if (!orders.empty() && orders[0].ability_id != sc2::ABILITY_ID::PATROL)
					{
						//return mining
						m_workerData.setWorkerJob(worker, WorkerJobs::Idle);
						workerJob = WorkerJobs::Idle;
					}
				}
			}
		}

		if (workerJob == WorkerJobs::Idle)
		{
			if (!worker.isAlive())
			{
				worker.stop();
			}
			else
			{
				bool isBuilder = false;
				for(const auto & building : m_bot.Buildings().getBuildings())
				{
					if(building.builderUnit == worker)
					{
						m_workerData.setWorkerJob(worker, WorkerJobs::Build, building.buildingUnit);
						if(building.buildingUnit.isValid() && building.buildingUnit.getBuildPercentage() < 1.f)
						{
							Micro::SmartRightClick(worker.getUnitPtr(), building.buildingUnit.getUnitPtr(), m_bot);
						}
						isBuilder = true;
						break;
					}
				}
				
				if (!isBuilder && !m_workerData.isProxyWorker(worker))
				{
					setMineralWorker(worker);
				}
			}
		}
    }
}

void WorkerManager::handleRepairWorkers()
{
	const int REPAIR_STATION_MIN_MINERAL = 10;//Also affects the automatic building repairing

    // Only terran worker can repair
    if (!Util::IsTerran(m_bot.GetSelfRace()))
        return;

	int mineral = m_bot.GetFreeMinerals();
	int gas = m_bot.GetFreeGas();

    for (auto & worker : m_workerData.getWorkers())
    {
        if (!worker.isValid()) { continue; }

        if (m_workerData.getWorkerJob(worker) == WorkerJobs::Repair)
        {
			if (!worker.isAlive())
			{
				// We inform the manager that we are no longer repairing
				stopRepairing(worker);
			}
            Unit repairedUnit = m_workerData.getWorkerRepairTarget(worker);
			if (repairedUnit.isValid())
			{
				auto type = repairedUnit.getType();
				// We do not try to repair if we don't have the required ressources
				if ((type.mineralPrice() > 0 && mineral == 0) || (type.gasPrice() > 0 && gas == 0))
				{
					// We inform the manager that we are no longer repairing
					stopRepairing(worker);
				}
				// We do not try to repair dead units nor full health units
				else if (!repairedUnit.isAlive() || repairedUnit.getHitPoints() + std::numeric_limits<float>::epsilon() >= repairedUnit.getUnitPtr()->health_max)
				{
					stopRepairing(worker);
				}
				else if (worker.isIdle())
				{
					// Get back to repairing...
					worker.repair(repairedUnit);
				}
			}
        }

		// this has been commented out because it doesn't work well against worker rushes (less combat scvs and too much minerals are spent repairing)
        /*if (worker.isAlive() && worker.getHitPoints() < worker.getUnitPtr()->health_max)
        {
            const std::set<Unit> & repairedBy = m_workerData.getWorkerRepairingThatTargetC(worker);
            if (repairedBy.empty())
            {
                auto repairGuy = getClosestMineralWorkerTo(worker.getPosition(), worker.getID(), MIN_HP_PERCENTAGE_TO_FIGHT);
                 
                if (repairGuy.isValid() && Util::Dist(worker.getPosition(), repairGuy.getPosition()) <= m_bot.Config().MaxWorkerRepairDistance)
                {
                    setRepairWorker(repairGuy, worker);
                }
            }
        }*/
    }

	if (mineral < REPAIR_STATION_MIN_MINERAL)//Stop repairing if not enough minerals
	{
		return;
	}

	//Repair station (RepairStation)
	const int MIN_GAS_TO_REPAIR = 5;
	const int MAX_REPAIR_WORKER = 5;
	const int FULL_REPAIR_MINERAL = 100;
	const int FULL_REPAIR_GAS = 50;
	const int REPAIR_STATION_SIZE = 3;
	const int REPAIR_STATION_WORKER_ZONE_SIZE = 10;

	/* Chart of repair worker based on minerals
	0: 10 mineral-
	1: 10 mineral+
	2: 40 mineral+
	3: 60 mineral+
	4: 80 mineral+
	5: 100 mineral+
	*/
	int currentMaxRepairWorker = std::min(MAX_REPAIR_WORKER, (int)floor(MAX_REPAIR_WORKER * (mineral / (float)FULL_REPAIR_MINERAL)));

	auto bases = m_bot.Bases().getOccupiedBaseLocations(Players::Self);
	for (auto & base : bases)
	{
		std::vector<Unit> unitsToRepair;

		//Get all units to repair
		for (auto & tagUnit : m_bot.GetAllyUnits())
		{
			Unit unit = tagUnit.second;
			if (unit.getUnitPtr()->build_progress != 1.0f)
			{
				continue;
			}

			float healthPercentage = unit.getHitPointsPercentage();
			if (healthPercentage < 100 && !unit.isBeingConstructed())
			{
				//Is repairable
				if (!unit.getType().isRepairable())
					continue;
				//Check if we have the required gas to repair it
				if (unit.getType().gasPrice() > 0 && gas <= MIN_GAS_TO_REPAIR)
					continue;

				const float distanceSquare = Util::DistSq(unit, base->getPosition());
				if (distanceSquare < REPAIR_STATION_SIZE * REPAIR_STATION_SIZE)
				{
					unitsToRepair.push_back(unit);
				}
			}
		}

		if (unitsToRepair.size() > 0)
		{
			//Send workers to repair
			int repairWorkers = 0;
			auto & workerData = getWorkerData();
			for (auto & worker : getWorkers())
			{
				auto it = unitsToRepair.begin();
				if (repairWorkers >= currentMaxRepairWorker)
				{
					break;
				}

				if (workerData.getWorkerJob(worker) == WorkerJobs::Minerals || workerData.getWorkerJob(worker) == WorkerJobs::Repair)
				{
					const float distanceSquare = Util::DistSq(worker, base->getPosition());
					if (distanceSquare < REPAIR_STATION_WORKER_ZONE_SIZE * REPAIR_STATION_WORKER_ZONE_SIZE)
					{
						setRepairWorker(worker, *it);

						//Add worker to the list of repair station worker for this base
						auto & baseRepairWorkers = workerData.getRepairStationWorkers()[base];
						if ((std::find(baseRepairWorkers.begin(), baseRepairWorkers.end(), worker) == baseRepairWorkers.end()))
						{
							baseRepairWorkers.push_back(worker);
						}
						 
						repairWorkers++;
						++it;
						if (it == unitsToRepair.end())
						{
							it = unitsToRepair.begin();
						}
					}
				}
			}
		}
	}
	
	//Automatically repair low health buildings, maximum 1 worker
	const float MIN_HEALTH = 50.f;
	const float MAX_HEALTH = 100.f;
	const int MAX_WORKER_PER_BUILDING = 1;
	for (auto & building : m_bot.Buildings().getFinishedBuildings())
	{
		if (!building.isCompleted())
		{
			continue;
		}

		auto percentage = building.getHitPointsPercentage();
		auto repairCount = getWorkerData().getWorkerRepairingTargetCount(building);

		if (percentage <= MIN_HEALTH && repairCount < MAX_WORKER_PER_BUILDING)
		{
			auto position = building.getPosition();
			auto worker = getClosestMineralWorkerTo(position);
			if (worker.isValid() && Util::PathFinding::IsPathToGoalSafe(worker.getUnitPtr(), position, true, m_bot))
			{
				setRepairWorker(worker, building);
				buildingAutomaticallyRepaired.push_back(building);
			}
		}
		else if (percentage >= MAX_HEALTH)
		{
			if (std::find(buildingAutomaticallyRepaired.begin(), buildingAutomaticallyRepaired.end(), building) != buildingAutomaticallyRepaired.end())
			{
				buildingAutomaticallyRepaired.remove(building);
			}
		}
	}
}

void WorkerManager::repairCombatBuildings()//Ignores if the path or the area around the building is safe or not.
{
	const float repairAt = 0.95f; //95% health
	const int maxReparator = 5; //Turret and bunkers only

	if (m_bot.GetSelfRace() != CCRace::Terran)
	{
		return;
	}

	bool completeWall = false;
	bool checkedCompleteWall = false;
	auto buildings = m_bot.Buildings().getFinishedBuildings();
	for (auto building : buildings)
	{
		if (building.isBeingConstructed())
		{
			continue;
		}

		if (building.getHitPoints() > repairAt * building.getUnitPtr()->health_max)
		{
			continue;
		}

		int maxReparator = 5;
		int alreadyRepairing = 0;
		switch ((sc2::UNIT_TYPEID)building.getAPIUnitType())
		{
			case sc2::UNIT_TYPEID::TERRAN_PLANETARYFORTRESS://Always MAX repair Planetary fortress
				///TODO Doesn't use the gas workers
				for (auto & worker : getWorkers())///TODO order by closest to the target base location
				{
					auto depot = m_workerData.getWorkerDepot(worker);
					if (depot.isValid() && depot.getID() == building.getID())
					{
						setRepairWorker(worker, building);
					}
				}
				break;
			default:
				bool shouldRepair = false;
				bool onlyBaseWorker = false;
				switch ((sc2::UNIT_TYPEID)building.getAPIUnitType())
				{
					case sc2::UNIT_TYPEID::TERRAN_COMMANDCENTER://Always repair command center and orbital
					case sc2::UNIT_TYPEID::TERRAN_COMMANDCENTERFLYING:
					case sc2::UNIT_TYPEID::TERRAN_ORBITALCOMMAND:
					case sc2::UNIT_TYPEID::TERRAN_ORBITALCOMMANDFLYING:
						maxReparator = 3;
						shouldRepair = true;
						onlyBaseWorker = true;
						break;
					case sc2::UNIT_TYPEID::TERRAN_MISSILETURRET://Always repair MISSILETURRET and BUNKER
					case sc2::UNIT_TYPEID::TERRAN_BUNKER:
						shouldRepair = true;
						break;
					default:	// Allows to repair buildings in wall (repair wall)
						//NOTE: commented out because we don't currently have a good way to check if the enemy has ranged units that could hit our workers.
						//		We don't want to suicide all of our workers trying to repair. Should also only repair if its a full wall.
						maxReparator = 3;
						const auto buildingTilePos = building.getTilePosition();
						const auto buildingPos = Util::GetPosition(buildingTilePos);
						const auto buildingDistSq = Util::DistSq(buildingTilePos, m_bot.Buildings().getWallPosition());
						if (buildingDistSq < 5 * 5)	// Within 5 tiles of the wall
						{
							if (!checkedCompleteWall)
							{
								std::vector<Unit> possibleWallBuildings;
								//TODO maybe consider all types of buildings
								const auto & barracks = m_bot.GetAllyUnits(sc2::UNIT_TYPEID::TERRAN_BARRACKS);
								const auto & supplyDepots = m_bot.GetAllyUnits(sc2::UNIT_TYPEID::TERRAN_SUPPLYDEPOT);
								const auto & supplyDepotsLowered = m_bot.GetAllyUnits(sc2::UNIT_TYPEID::TERRAN_SUPPLYDEPOTLOWERED);
								possibleWallBuildings.insert(possibleWallBuildings.end(), barracks.begin(), barracks.end());
								possibleWallBuildings.insert(possibleWallBuildings.end(), supplyDepots.begin(), supplyDepots.end());
								possibleWallBuildings.insert(possibleWallBuildings.end(), supplyDepotsLowered.begin(), supplyDepotsLowered.end());
								int wallBuildings = 0;
								for (const auto & possibleWallBuilding : possibleWallBuildings)
								{
									const auto distSq = Util::DistSq(possibleWallBuilding, buildingPos);
									if (distSq < 5 * 5)
										wallBuildings += 1;
								}
								if (wallBuildings >= 3)
									completeWall = true;
								checkedCompleteWall = true;
							}
							if (!completeWall)
							{
								shouldRepair = false;
								break;
							}
							shouldRepair = true;

							for (const auto & enemyUnit : m_bot.GetKnownEnemyUnits())
							{
								const auto enemyDistSq = Util::DistSq(enemyUnit, buildingPos);
								const auto enemyAttackRange = Util::GetAttackRangeForTarget(enemyUnit.getUnitPtr(), building.getUnitPtr(), m_bot);
								if (enemyAttackRange > 3 && enemyDistSq < enemyAttackRange * enemyAttackRange)
								{
									shouldRepair = false;
									break;
								}
							}
						}
						break;
				}
				if (shouldRepair)
				{
					int reparator = maxReparator - floor(maxReparator * building.getHitPointsPercentage() / 100);
					std::set<Unit> workers = getWorkers();
					for (auto & worker : workers)
					{
						Unit repairedUnit = m_workerData.getWorkerRepairTarget(worker);
						if (repairedUnit.isValid() && repairedUnit.getID() == building.getID())
						{
							alreadyRepairing++;
							if (reparator == alreadyRepairing)//Already enough repairer, stop checking how many are repairing
							{
								break;
							}
						}
					}

					for (int i = 0; i < reparator - alreadyRepairing; i++)
					{
						Unit worker;
						if (onlyBaseWorker)
						{
							auto base = m_bot.Bases().getBaseContainingPosition(building.getPosition(), Players::Self);
							if (base != nullptr)
							{
								auto depot = base->getResourceDepot();
								if (depot.isValid())
								{
									for (auto & baseWorker : workers)///TODO order by closest to the target base location, allow gas workers
									{
										if (!isFree(baseWorker))
										{
											continue;
										}
										const auto workerDepot = m_workerData.getWorkerDepot(baseWorker);
										if (workerDepot.isValid() && workerDepot.getID() == depot.getID())
										{
											worker = baseWorker;
											break;
										}
									}
								}
							}
						}
						else
						{
							worker = getClosestMineralWorkerTo(building.getPosition());
						}

						if (worker.isValid())
							setRepairWorker(worker, building);
						else
							break;
					}
				}
		}
	}
}

Unit WorkerManager::getClosestMineralWorkerTo(const CCPosition & pos, CCUnitID workerToIgnore, float minHpPercentage, bool filterMoving) const
{
	auto workersToIgnore = std::vector<CCUnitID>();
	workersToIgnore.push_back(workerToIgnore);
	return getClosestMineralWorkerTo(pos, workersToIgnore, minHpPercentage, filterMoving);
}

Unit WorkerManager::getClosestMineralWorkerTo(const CCPosition & pos, const std::vector<CCUnitID> & workersToIgnore, float minHpPercentage, bool filterMoving) const
{
	Unit closestMineralWorker;
	auto closestDist = 0.f;

	const BaseLocation * base = nullptr;
	const auto & baseLocations = m_bot.Bases().getBaseLocations();
	for (const auto baseLocation : baseLocations)
	{
		if (Util::DistSq(pos, Util::GetPosition(baseLocation->getDepotTilePosition())) < 10 * 10)
		{
			base = baseLocation;
			break;
		}
	}

	// for each of our workers
	for (auto & worker : m_workerData.getWorkers())
	{
		if (!worker.isValid() || std::find(workersToIgnore.begin(), workersToIgnore.end(), worker.getID()) != workersToIgnore.end()) { continue; }
		const sc2::Unit* workerPtr = worker.getUnitPtr();
		if (workerPtr->health < minHpPercentage * workerPtr->health_max)
			continue;

		// if it is a mineral worker, Idle or None
		if (!isFree(worker))
			continue;
		if (isReturningCargo(worker))
			continue;
		if (filterMoving && worker.isMoving())
			continue;
		
		auto dist = Util::DistSq(worker.getPosition(), pos);
		if (base != nullptr && dist > 20 * 20)
		{
			dist = base->getGroundDistance(worker.getPosition());
			dist *= dist;
		}
		if (!closestMineralWorker.isValid() || dist < closestDist)
		{
			closestMineralWorker = worker;
			closestDist = dist;
		}
	}
	return closestMineralWorker;
}


Unit WorkerManager::getClosestMineralWorkerTo(const CCPosition & pos, float minHpPercentage, bool filterMoving) const
{
    return getClosestMineralWorkerTo(pos, CCUnitID{}, minHpPercentage, filterMoving);
}

Unit WorkerManager::getClosestGasWorkerTo(const CCPosition & pos, CCUnitID workerToIgnore, float minHpPercentage) const
{
	Unit closestMineralWorker;
	double closestDist = std::numeric_limits<double>::max();

	// for each of our workers
	for (auto & worker : m_workerData.getWorkers())
	{
		if (!worker.isValid() || worker.getID() == workerToIgnore) { continue; }
		const sc2::Unit* workerPtr = worker.getUnitPtr();
		if (workerPtr->health < minHpPercentage * workerPtr->health_max)
		{
			continue;
		}

		// if it is a mineral worker, Idle or None
		if (m_workerData.getWorkerJob(worker) == WorkerJobs::Gas)
		{
			if (!isReturningCargo(worker))
			{
				///TODO: Maybe it should by ground distance? Not sure how it will affect performance
				double dist = Util::DistSq(worker.getPosition(), pos);
				if (!closestMineralWorker.isValid() || dist < closestDist)
				{
					closestMineralWorker = worker;
					closestDist = dist;
				}
			}
		}
	}
	return closestMineralWorker;
}

Unit WorkerManager::getClosestGasWorkerTo(const CCPosition & pos, float minHpPercentage) const
{
	return getClosestGasWorkerTo(pos, CCUnitID{}, minHpPercentage);
}

Unit WorkerManager::getClosest(const Unit unit, const std::list<Unit> units) const
{
	Unit currentClosest;
	float minDist = -1;
	for (auto potentialClosest : units)
	{
		if (!potentialClosest.isValid())
		{
			continue;
		}
		const float dist = Util::DistSq(potentialClosest.getUnitPtr()->pos, unit.getPosition());
		if (minDist == -1 || dist < minDist) {
			currentClosest = potentialClosest;
			minDist = dist;
		}
	}
	return currentClosest;
}

/*std::list<Unit> WorkerManager::orderByDistance(const std::list<Unit> units, CCPosition pos, bool closestFirst)
{
	struct UnitDistance
	{
		Unit unit;
		float distance;

		UnitDistance(Unit unit, float distance) : unit(unit), distance(distance) {}

		bool operator < (const UnitDistance& str) const
		{
			return (distance < str.distance);
		}

		bool operator - (const UnitDistance& str) const
		{
			return (distance - str.distance);
		}

		bool operator == (const UnitDistance& str) const
		{
			return (distance == str.distance);
		}
	};

	std::list<UnitDistance> unorderedUnitsDistance;
	std::list<UnitDistance> orderedUnitsDistance;

	for (auto unit : units)
	{
		unorderedUnitsDistance.push_back(UnitDistance(unit, Util::Dist(unit.getUnitPtr()->pos, pos)));
	}
	
	//sort
	UnitDistance bestDistance = UnitDistance(Unit(), 1000000);
	while (!unorderedUnitsDistance.empty())
	{
		for (auto distance : unorderedUnitsDistance)
		{
			if (distance < bestDistance)
			{
				bestDistance = distance;
			}
		}
		if (bestDistance.distance == 1000000)
		{
			break;
		}
		orderedUnitsDistance.push_back(bestDistance);
		unorderedUnitsDistance.remove(bestDistance);
		bestDistance = UnitDistance(Unit(), 1000000);
	}

	std::list<Unit> orderedUnits;
	for (auto unitDistance : orderedUnitsDistance)
	{
		if (closestFirst)
		{
			orderedUnits.push_back(unitDistance.unit);
		}
		else
		{
			orderedUnits.push_front(unitDistance.unit);
		}
	}
	return orderedUnits;
}*/

// set a worker to mine minerals
void WorkerManager::setMineralWorker(const Unit & unit)
{
    // check if there is a mineral available to send the worker to
    auto depot = getClosestDepot(unit);

    // if there is a valid mineral
    if (depot.isValid() && depot.isCompleted())
    {
        // update m_workerData with the new job
        m_workerData.setWorkerJob(unit, WorkerJobs::Minerals, depot);
    }
}

Unit WorkerManager::getClosestDepot(Unit worker) const
{
    Unit closestDepot;
    double closestDistance = std::numeric_limits<double>::max();

    for (auto & unit : m_bot.UnitInfo().getUnits(Players::Self))
    {
        if (!unit.isValid()) { continue; }

        if (unit.getType().isResourceDepot())
        {
			if (!unit.isCompleted())
			{
				//Cannont allow unfinished bases, unless the base is morphing (orbital, lair and hive).
				switch ((sc2::UNIT_TYPEID)unit.getAPIUnitType())
				{
					case sc2::UNIT_TYPEID::TERRAN_COMMANDCENTER://Ignores flying depot
					case sc2::UNIT_TYPEID::PROTOSS_NEXUS:
					case sc2::UNIT_TYPEID::ZERG_HATCHERY:
						continue;
				}
			}
            const double distance = Util::DistSq(unit, worker);
            if (!closestDepot.isValid() || distance < closestDistance)
            {
                closestDepot = unit;
                closestDistance = distance;
            }
        }
    }

    return closestDepot;
}

// other managers that need workers call this when they're done with a unit
void WorkerManager::finishedWithWorker(const Unit & unit)
{
    if (m_workerData.getWorkerJob(unit) != WorkerJobs::Scout)
    {
        m_workerData.setWorkerJob(unit, WorkerJobs::Idle);
    }
}

Unit WorkerManager::getMineralWorker(Unit refinery) const
{
    return getClosestMineralWorkerTo(refinery.getPosition());
}

Unit WorkerManager::getGasWorker(Unit refinery, bool checkReturningCargo, bool checkInsideRefinery) const
{
	auto workers = m_workerData.getAssignedWorkersRefinery(refinery);
	if (checkReturningCargo || checkInsideRefinery)
	{
		for (auto & worker : workers)
		{
			if (m_bot.Commander().isInside(worker.getTag()))
			{
				continue;
			}

			if (checkInsideRefinery)
			{
				//Skip workers inside the geyser
				if (isInsideGeyser(worker))
				{
					continue;
				}
			}
			if (checkReturningCargo)
			{
				//Skip workers returning cargo
				if (isReturningCargo(worker))
				{
					continue;
				}
			}
			return worker;
		}
	}

	//If we found no suitable workers (so probably 1 returning cargo and 1 inside the geyser OR only 1 worker), then return the returning one if exist
	if (checkReturningCargo && checkInsideRefinery)
	{
		for (auto & worker : workers)
		{
			//Skip workers inside the geyser
			if (isInsideGeyser(worker))
			{
				continue;
			}

			//Return the worker returning cargo in priority over the worker inside the geyser
			return worker;
		}
	}
	return workers[0];
}

int WorkerManager::getGasWorkersTarget() const
{
	return gasWorkersTarget;
}

Unit WorkerManager::getDepotAtBasePosition(CCPosition basePosition) const
{
	for (auto & unit : m_bot.UnitInfo().getUnits(Players::Self))
	{
		// we only care about buildings on the ground
		if (!unit.getType().isBuilding() || unit.isFlying() || !unit.getType().isResourceDepot())
		{
			continue;
		}
		BaseLocation * baseLocation = m_bot.Bases().getBaseLocation(unit.getPosition());
		if (baseLocation && baseLocation->getPosition().x == basePosition.x && baseLocation->getPosition().y == basePosition.y)
		{
			return unit;
		}
	}
	return {};
}

int WorkerManager::getWorkerCountAtBasePosition(CCPosition basePosition) const
{
	return getWorkerData().getCountWorkerAtDepot(getDepotAtBasePosition(basePosition));
}

// gets a builder for BuildingManager to use
// if setJobAsBuilder is true (default), it will be flagged as a builder unit
// set 'setJobAsBuilder' to false if we just want to see which worker will build a building
Unit WorkerManager::getBuilder(Building & b, bool setJobAsBuilder, bool filterMoving) const
{
	bool isValid;
	std::vector<CCUnitID> invalidWorkers;
	Unit builderWorker;
	
	do
	{
		isValid = true;
		builderWorker = getClosestMineralWorkerTo(Util::GetPosition(b.finalPosition), invalidWorkers, 0, filterMoving);
		if (!builderWorker.isValid())//If no worker left to check
		{
			break;
		}

		//Check if worker is already building something else
		//TODO This shouldn't be needed (shouldn't happen), right?
		for (auto & building : m_bot.Buildings().getBuildings())
		{
			if (building.builderUnit.isValid() && building.builderUnit.getID() == builderWorker.getID())
			{
				invalidWorkers.push_back(builderWorker.getID());
				isValid = false;
				break;
			}
		}
	} while (!isValid);

    // if the worker exists (one may not have been found in rare cases)
    if (builderWorker.isValid() && setJobAsBuilder && m_workerData.getWorkerJob(builderWorker) != WorkerJobs::Build)
    {
		m_workerData.setWorkerJob(builderWorker, WorkerJobs::Build, b.builderUnit);
    }

    return builderWorker;
}

// sets a worker as a scout
void WorkerManager::setScoutWorker(Unit workerTag)
{
    m_workerData.setWorkerJob(workerTag, WorkerJobs::Scout);
}

void WorkerManager::setCombatWorker(Unit workerTag)
{
    m_workerData.setWorkerJob(workerTag, WorkerJobs::Combat);
}

void WorkerManager::drawResourceDebugInfo()
{
#ifdef PUBLIC_RELEASE
	return;
#endif
    if (!m_bot.Config().DrawResourceInfo)
    {
        return;
    }

    for (auto & worker : m_workerData.getWorkers())
    {
        if (!worker.isValid()) { continue; }
		if (worker.getAPIUnitType() == sc2::UNIT_TYPEID::TERRAN_MULE)
		{
			continue;
		}

        auto depot = m_workerData.getWorkerDepot(worker);
        if (depot.isValid())
        {
            m_bot.Map().drawLine(worker.getPosition(), depot.getPosition());
        }
    }

	for (auto & unit : m_bot.UnitInfo().getUnits(Players::Neutral))
	{
		if (unit.getType().isMineral())
		{
			switch ((sc2::UNIT_TYPEID)unit.getAPIUnitType())
			{
				case sc2::UNIT_TYPEID::NEUTRAL_RICHMINERALFIELD:
				case sc2::UNIT_TYPEID::NEUTRAL_RICHMINERALFIELD750:
				case sc2::UNIT_TYPEID::NEUTRAL_PURIFIERRICHMINERALFIELD:
				case sc2::UNIT_TYPEID::NEUTRAL_PURIFIERRICHMINERALFIELD750:
					m_bot.Map().drawText(unit.getPosition(), "Rich mineral");
					break;
				case sc2::UNIT_TYPEID::NEUTRAL_MINERALFIELD:
				case sc2::UNIT_TYPEID::NEUTRAL_MINERALFIELD750:
				case sc2::UNIT_TYPEID::NEUTRAL_PURIFIERMINERALFIELD:
				case sc2::UNIT_TYPEID::NEUTRAL_PURIFIERMINERALFIELD750:
				case sc2::UNIT_TYPEID::NEUTRAL_LABMINERALFIELD:
				case sc2::UNIT_TYPEID::NEUTRAL_LABMINERALFIELD750:
				case sc2::UNIT_TYPEID::NEUTRAL_BATTLESTATIONMINERALFIELD:
				case sc2::UNIT_TYPEID::NEUTRAL_BATTLESTATIONMINERALFIELD750:
					m_bot.Map().drawText(unit.getPosition(), "Mineral");
					break;
				default:
					m_bot.Map().drawText(unit.getPosition(), "Not mineral");
			}
		}
	}
}

void WorkerManager::drawWorkerInformation()
{
#ifdef PUBLIC_RELEASE
	return;
#endif
    if (!m_bot.Config().DrawWorkerInfo)
    {
        return;
    }

    for (auto & worker : m_workerData.getWorkers())
    {
		std::ostringstream oss;
		auto code = m_workerData.getJobCode(worker);
		oss << code;

		if (strcmp(code, "B") == 0)
		{
			std::string buildingType = "UNKNOWN";
			for (auto b : m_bot.Buildings().getBuildings())
			{
				if (b.builderUnit.getID() == worker.getID())
				{
					buildingType = b.type.getName();
					break;
				}
			}

			oss << " (" << buildingType << ")";
		}
		if (m_workerData.isProxyWorker(worker))
		{
			oss << " [Proxy]";
		}
		m_bot.Map().drawText(worker.getPosition(), oss.str());
    }
}
 
bool WorkerManager::isFree(Unit worker) const
{
	if (worker.getType().isMule())
		return false;
	int job = m_workerData.getWorkerJob(worker);
    return job == WorkerJobs::Minerals || job == WorkerJobs::Idle || job == WorkerJobs::None || (m_workerData.isProxyWorker(worker) && job != WorkerJobs::Build);
}

bool WorkerManager::isInsideGeyser(Unit worker) const
{
	int job = m_workerData.getWorkerJob(worker);
	if (job == WorkerJobs::Gas)
	{
		auto loop = m_bot.GetGameLoop();
		if (worker.getUnitPtr()->last_seen_game_loop != m_bot.GetGameLoop())
		{
			return true;
		}
		return false;
	}
	return false;
}

bool WorkerManager::isWorkerScout(Unit worker) const
{
    return (m_workerData.getWorkerJob(worker) == WorkerJobs::Scout);
}

bool WorkerManager::isBuilder(Unit worker) const
{
    return (m_workerData.getWorkerJob(worker) == WorkerJobs::Build);
}

bool WorkerManager::isReturningCargo(Unit worker) const
{
	return worker.isReturningCargo();
}

bool WorkerManager::canHandleMoreRefinery() const
{
	return gasWorkersTarget >= 3;
}

int WorkerManager::getNumMineralWorkers()
{
    return m_workerData.getWorkerJobCount(WorkerJobs::Minerals);
}

int WorkerManager::getNumGasWorkers()
{
    return m_workerData.getWorkerJobCount(WorkerJobs::Gas);

}

int WorkerManager::getNumWorkers()
{
    int count = 0;
    for (auto worker : m_workerData.getWorkers())
    {
        if (worker.isValid() && worker.isAlive())
        {
            ++count;
        }
    }
    return count;
}

std::set<Unit> WorkerManager::getWorkers() const
{
	return m_workerData.getWorkers();
}

WorkerData & WorkerManager::getWorkerData() const
{
	return m_workerData;
}

sc2::Tag WorkerManager::getMuleTargetTag(const Unit mule)
{
	auto id = mule.getID();
	if (muleHarvests.find(id) == muleHarvests.end())
	{
		muleHarvests[id] = std::pair<bool, std::pair<int, sc2::Tag>>();
	}
	return muleHarvests[id].second.second;
}

void WorkerManager::setMuleTargetTag(const Unit mule, const sc2::Tag mineral)
{
	auto id = mule.getID();
	if (muleHarvests.find(id) == muleHarvests.end())
	{
		muleHarvests[id] = std::pair<bool, std::pair<int, sc2::Tag>>();
	}
	muleHarvests[id].second.second = mineral;
}