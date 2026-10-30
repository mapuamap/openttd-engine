/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file town_growth.h Supply-driven town growth.
 *
 * A town grows from a growth budget measured in residents. Every year it earns
 * 2 * a * M * sqrt(P) residents, where P is the population, a is the base growth
 * constant (setting economy.town_growth_base, in thousandths) and M is the
 * product of the multipliers of the four needs (passengers, mail, goods,
 * electricity). Each need has a level 0..5 derived from its coverage of a
 * monthly norm; level 0 of an unlocked need blocks growth. The builder spends
 * the budget on houses laid out in 3x3 blocks surrounded by roads.
 *
 * Module ownership:
 *  - town_growth.cpp: needs, levels, multiplier and the budget (monthly + per tick).
 *  - town_power.cpp: power plants, their coal stock, town links and electricity allocation.
 *  - town_blocks.cpp: the 3x3 block builder that turns budget into roads and houses.
 */

#ifndef TOWN_GROWTH_H
#define TOWN_GROWTH_H

#include "town_growth_type.h"
#include "town_type.h"
#include "industry_type.h"
#include "cargo_type.h"
#include "strings_type.h"

struct Town;
struct Industry;

/** Balance constants of the growth model that are not exposed as settings. */
namespace TownGrowthBalance {
	/** Below this population a missing supply (level 0) never blocks growth. */
	static constexpr uint32_t GRACE_POPULATION = 1000;
	/** Population from which a need is unlocked; a locked need counts as at least neutral. */
	static constexpr std::array<uint32_t, to_underlying(TownNeed::End)> UNLOCK_POPULATION = {0, 1000, 2500, 5000};
	/** Growth multiplier per need level 0..5, per mille. */
	static constexpr std::array<uint16_t, TOWN_NEED_LEVEL_MAX + 1> LEVEL_MULTIPLIER = {0, 250, 700, 1000, 1300, 1600};
	/** Minimal coverage in percent for need levels 1..5 (index 0 is level 1). */
	static constexpr std::array<uint16_t, TOWN_NEED_LEVEL_MAX> LEVEL_COVERAGE = {5, 25, 50, 75, 100};
	/** The budget is capped at this many years of growth. */
	static constexpr uint32_t BUDGET_CAP_YEARS = 2;
	/** A house is replaced by a denser one only when it is at least this many years old. */
	static constexpr uint8_t DENSIFY_MIN_AGE = 10;
	/** Ticks to wait before trying the builder again after it failed. */
	static constexpr uint16_t BUILDER_RETRY_TICKS = 74 * 10;
	/** Number of power plants a town links to: one plus one per this many residents (from 50000 on). */
	static constexpr uint32_t POWER_LINK_RESIDENTS = 100000;
	/** Maximum number of power plants linked to one town. */
	static constexpr uint32_t POWER_LINK_MAX = 8;
	/** Electricity units produced from one unit of fuel (coal). */
	static constexpr uint32_t POWER_PER_FUEL = 1;
}

/* --- Needs and growth budget (town_growth.cpp) --- */

void UpdateTownNeeds(Town *t);
void TownGrowthTick(Town *t);
uint32_t GetTownNeedDemand(const Town *t, TownNeed need);
bool IsTownNeedUnlocked(const Town *t, TownNeed need);
uint32_t GetTownNeedUnlockPopulation(TownNeed need);
uint8_t GetTownNeedEffectiveLevel(const Town *t, TownNeed need);
uint8_t GetTownNeedLevelForCoverage(uint32_t coverage);
uint16_t GetTownNeedLevelMultiplier(uint8_t level);
uint32_t GetTownGrowthMultiplier(const Town *t);
uint32_t GetTownYearlyGrowth(const Town *t, uint32_t multiplier);
void SetTownNeedForced(Town *t, TownNeed need, uint8_t level);
StringID GetTownNeedName(TownNeed need);
uint32_t ScaleHousePopulation(uint32_t house_population);

/* --- Power grid (town_power.cpp) --- */

bool IsElectricityAvailable();
bool IsPowerPlant(const Industry *i);
CargoType GetPowerPlantFuelCargo(const Industry *i);
uint32_t GetPowerPlantFuelStock(const Industry *i);
uint32_t GetTownWantedPowerLinks(const Town *t);
uint32_t GetTownPowerReceived(const Town *t);
void UpdatePowerGrid();
void RebuildTownPowerLinks();
void OnPowerPlantRemoved(IndustryID plant);

/* --- Block builder (town_blocks.cpp) --- */

/** Outcome of one step of the town builder. */
struct TownBuildResult {
	bool success = false; ///< Did the builder build anything (road or house)?
	int32_t residents = 0; ///< Population the step will add once its houses are completed (density applied).
};

TownBuildResult TownBuilderStep(Town *t, TownExpandModes modes);

/* --- Commands and the simulation log (town_growth_cmd.cpp) --- */

void LogTownGrowthSimulation(bool force);

#endif /* TOWN_GROWTH_H */
