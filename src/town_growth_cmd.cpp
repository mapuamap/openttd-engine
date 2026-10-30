/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file town_growth_cmd.cpp Commands and the simulation log of the supply-driven town growth. */

#include "stdafx.h"
#include "command_func.h"
#include "debug.h"
#include "industry.h"
#include "town.h"
#include "town_cmd.h"
#include "town_growth.h"
#include "window_func.h"
#include "timer/timer.h"
#include "timer/timer_game_economy.h"

#include "safeguards.h"

/** Severity of the simulation log lines: they are written from debug level 1 of the "town" facility on ("-d town=1"). */
static constexpr Severity TOWN_SIMULATION_LOG_SEVERITY = Severity::Error;

/**
 * Force the level of a need of a town, or let it be computed again.
 * Used by the console and game scripts to test the growth model.
 * @param flags Type of operation.
 * @param town_id The town, or TownID::Invalid() for all towns.
 * @param need The need, or TownNeed::End for all needs.
 * @param level The level 0..#TOWN_NEED_LEVEL_MAX to force, or #TOWN_NEED_NOT_FORCED to compute it from the supply again.
 * @return Empty cost or an error.
 */
CommandCost CmdTownForceNeedLevel(DoCommandFlags flags, TownID town_id, TownNeed need, uint8_t level)
{
	if (town_id != TownID::Invalid() && !Town::IsValidID(town_id)) return CMD_ERROR;
	if (need > TownNeed::End) return CMD_ERROR;
	if (level > TOWN_NEED_LEVEL_MAX && level != TOWN_NEED_NOT_FORCED) return CMD_ERROR;

	if (flags.Test(DoCommandFlag::Execute)) {
		auto force = [need, level](Town *t) {
			for (TownNeed n : EnumRange(TownNeed::End)) {
				if (need == TownNeed::End || need == n) SetTownNeedForced(t, n, level);
			}
			InvalidateWindowData(WindowClass::TownView, t->index);
		};

		if (town_id == TownID::Invalid()) {
			for (Town *t : Town::Iterate()) force(t);
		} else {
			force(Town::Get(town_id));
		}
	}

	return CommandCost();
}

/**
 * Add fuel to the stock of a power plant, as if it was delivered.
 * Used by the console and game scripts to test the power grid.
 * @param flags Type of operation.
 * @param industry_id The power plant, or IndustryID::Invalid() for all power plants.
 * @param amount Fuel to add; the stock is capped at the maximum the industry can hold.
 * @return Empty cost or an error.
 */
CommandCost CmdPowerPlantAddFuel(DoCommandFlags flags, IndustryID industry_id, uint16_t amount)
{
	if (industry_id != IndustryID::Invalid()) {
		const Industry *i = Industry::GetIfValid(industry_id);
		if (i == nullptr || !IsPowerPlant(i)) return CMD_ERROR;
	}

	if (flags.Test(DoCommandFlag::Execute)) {
		auto add = [amount](Industry *i) {
			if (!IsPowerPlant(i)) return;
			auto it = i->GetCargoAccepted(GetPowerPlantFuelCargo(i));
			if (it == std::end(i->accepted)) return;
			it->waiting = static_cast<uint16_t>(std::min<uint32_t>(it->waiting + amount, UINT16_MAX));
			SetWindowDirty(WindowClass::IndustryView, i->index);
		};

		if (industry_id == IndustryID::Invalid()) {
			for (Industry *i : Industry::Iterate()) add(i);
		} else {
			add(Industry::Get(industry_id));
		}
	}

	return CommandCost();
}

/**
 * Write the simulation line of every town to the debug output (facility "town").
 * The lines are machine readable; the headless growth simulator of the workspace parses them.
 * @param force Write the lines even when the debug level of the facility is too low.
 */
void LogTownGrowthSimulation(bool force)
{
	if (!force && !IsVisibleSeverity(Facility::Town, TOWN_SIMULATION_LOG_SEVERITY)) return;

	for (const Town *t : Town::Iterate()) {
		const TownGrowthState &g = t->growth;
		std::string levels;
		std::string raw;
		std::string coverage;
		for (TownNeed n : EnumRange(TownNeed::End)) {
			const char *sep = (n == TownNeed::Begin) ? "" : ",";
			format_append(levels, "{}{}", sep, GetTownNeedEffectiveLevel(t, n));
			format_append(raw, "{}{}", sep, g.needs[n].level);
			format_append(coverage, "{}{}", sep, g.needs[n].coverage);
		}
		DebugPrint(Facility::Town, TOWN_SIMULATION_LOG_SEVERITY, fmt::format(
				"[townsim] year={} town={} pop={} houses={} mult={} levels={} raw={} cov={} growth={} budget={} status={}",
				TimerGameEconomy::year, t->index, t->cache.population, t->cache.num_houses, g.multiplier,
				levels, raw, coverage, g.yearly_growth, g.budget / 1000, to_underlying(g.status)));
	}
}

/** Write the simulation log at the start of every economy year when the "town" debug level asks for it. */
static const IntervalTimer<TimerGameEconomy> _economy_town_growth_log_yearly({TimerGameEconomy::Trigger::Year, TimerGameEconomy::Priority::None}, [](auto)
{
	LogTownGrowthSimulation(false);
});
