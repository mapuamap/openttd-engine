/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file town_growth.cpp Needs of towns and the growth budget of the supply-driven growth model.
 *
 * Once a month every need of a town gets the coverage of its norm for the month
 * that just ended. The coverage of the last three months is averaged and mapped
 * to a level 0..5, which moves at most one step per month. The effective levels
 * of the four needs give the growth multiplier, and with the population the
 * yearly growth in residents.
 *
 * Every day the town earns its share of the yearly growth as budget (in
 * milli-residents). While the budget is positive the town asks the builder for
 * a step at most every #TOWN_BUILDER_STEP_TICKS ticks; the residents of the
 * houses a step builds are taken from the budget.
 */

#include "stdafx.h"
#include "town.h"
#include "town_growth.h"
#include "town_growth_internal.h"
#include "cargotype.h"
#include "landscape.h"
#include "settings_type.h"
#include "tile_map.h"
#include "window_func.h"
#include "timer/timer_game_calendar.h"
#include "timer/timer_game_tick.h"

#include "table/strings.h"

#include "safeguards.h"

/**
 * Scale the population of a house by the town density setting.
 * @param house_population Population of the house as defined by its spec.
 * @return Residents living in the house.
 */
uint32_t ScaleHousePopulation(uint32_t house_population)
{
	return house_population * std::max<uint8_t>(_settings_game.economy.town_density, 1);
}

/**
 * Get the name of a need.
 * @param need The need.
 * @return Its name.
 */
StringID GetTownNeedName(TownNeed need)
{
	switch (need) {
		case TownNeed::Passengers: return STR_TOWN_NEED_PASSENGERS;
		case TownNeed::Mail: return STR_TOWN_NEED_MAIL;
		case TownNeed::Goods: return STR_TOWN_NEED_GOODS;
		case TownNeed::Electricity: return STR_TOWN_NEED_ELECTRICITY;
		case TownNeed::Valuables: return STR_TOWN_NEED_VALUABLES;
		default: NOT_REACHED();
	}
}

/* --- Pure calculations --- */

/**
 * Compute the rounded integer square root of a 64 bit number.
 * @param num Radicand.
 * @return Rounded integer square root.
 */
static uint64_t IntSqrt64(uint64_t num)
{
	uint64_t res = 0;
	uint64_t bit = 1ULL << 62; // Second to top bit number.

	/* 'bit' starts at the highest power of four <= the argument. */
	while (bit > num) bit >>= 2;

	while (bit != 0) {
		if (num >= res + bit) {
			num -= res + bit;
			res = (res >> 1) + bit;
		} else {
			res >>= 1;
		}
		bit >>= 2;
	}

	/* Arithmetic rounding to nearest integer. */
	if (num > res) res++;

	return res;
}

/**
 * Compute the coverage of a need in one month.
 * When nothing is demanded, a town without population counts as fully covered,
 * any supply counts as fully covered, and otherwise the month counts as neutral
 * (the lowest coverage of #TOWN_NEED_LEVEL_NEUTRAL), so it neither helps nor hurts.
 * @param supplied Amount supplied in the month.
 * @param demand Amount needed in the month for full coverage.
 * @param population Population of the town.
 * @return Coverage in percent, at most #TOWN_NEED_COVERAGE_MAX.
 */
uint16_t CalcTownNeedCoverage(uint32_t supplied, uint32_t demand, uint32_t population)
{
	if (demand == 0) {
		if (population == 0 || supplied > 0) return 100;
		return TownGrowthBalance::LEVEL_COVERAGE[TOWN_NEED_LEVEL_NEUTRAL - 1];
	}
	return static_cast<uint16_t>(std::min<uint64_t>(static_cast<uint64_t>(supplied) * 100 / demand, TOWN_NEED_COVERAGE_MAX));
}

/**
 * Average the coverage of the last months.
 * @param history Coverage of the last months.
 * @return Rounded average coverage in percent.
 */
uint16_t CalcTownNeedSmoothedCoverage(std::span<const uint16_t> history)
{
	if (history.empty()) return 0;
	uint32_t sum = std::accumulate(history.begin(), history.end(), 0U);
	return static_cast<uint16_t>((sum + history.size() / 2) / history.size());
}

/**
 * Get the level a need aims for at a given coverage.
 * @param coverage Coverage in percent.
 * @return Level 0..#TOWN_NEED_LEVEL_MAX.
 */
uint8_t GetTownNeedLevelForCoverage(uint32_t coverage)
{
	uint8_t level = 0;
	for (uint16_t threshold : TownGrowthBalance::LEVEL_COVERAGE) {
		if (coverage < threshold) break;
		level++;
	}
	return level;
}

/**
 * Move a need level one step towards its target (hysteresis: at most one step per month).
 * @param previous Level of the previous month.
 * @param target Level the coverage calls for.
 * @return The new level.
 */
uint8_t CalcTownNeedLevelStep(uint8_t previous, uint8_t target)
{
	previous = std::min(previous, TOWN_NEED_LEVEL_MAX);
	target = std::min(target, TOWN_NEED_LEVEL_MAX);
	if (target > previous) return previous + 1;
	if (target < previous) return previous - 1;
	return previous;
}

/**
 * Add the coverage of the month that just ended to a need, and update its smoothed coverage and level.
 * A need that was never evaluated (the state of a new town, or of a savegame from before the needs:
 * no coverage at all at the neutral level) gets its whole history filled with this month, so the
 * average does not start with months without data.
 * @param state The need.
 * @param coverage Coverage of the month, in percent.
 */
void UpdateTownNeedCoverage(TownNeedState &state, uint16_t coverage)
{
	auto &history = state.coverage_history;
	bool fresh = state.level == TOWN_NEED_LEVEL_NEUTRAL && state.coverage == 0 && std::ranges::all_of(history, [](uint16_t c) { return c == 0; });

	if (fresh) {
		std::ranges::fill(history, coverage);
	} else {
		std::copy_backward(history.begin(), history.end() - 1, history.end());
		history[0] = coverage;
	}

	state.coverage = CalcTownNeedSmoothedCoverage(history);
	state.level = CalcTownNeedLevelStep(state.level, GetTownNeedLevelForCoverage(state.coverage));
}

/**
 * Get the level of a need that counts for growth.
 * A forced level replaces the computed one. A need that is not unlocked yet counts as at least
 * #TOWN_NEED_LEVEL_NEUTRAL, and below TownGrowthBalance::GRACE_POPULATION a level 0 counts as
 * #TOWN_NEED_LEVEL_NEUTRAL, so a small town is never blocked.
 * @param level Level computed from the coverage.
 * @param forced Forced level, or #TOWN_NEED_NOT_FORCED.
 * @param population Population of the town.
 * @param unlock_population Population from which the need is unlocked.
 * @return The effective level 0..#TOWN_NEED_LEVEL_MAX.
 */
uint8_t CalcTownNeedEffectiveLevel(uint8_t level, uint8_t forced, uint32_t population, uint32_t unlock_population)
{
	uint8_t effective = std::min(forced != TOWN_NEED_NOT_FORCED ? forced : level, TOWN_NEED_LEVEL_MAX);
	if (population < unlock_population) effective = std::max(effective, TOWN_NEED_LEVEL_NEUTRAL);
	if (population < TownGrowthBalance::GRACE_POPULATION) effective = std::max(effective, TOWN_NEED_LEVEL_NEUTRAL);
	return effective;
}

/**
 * Get the factor of the town growth speed setting.
 * Funded towns grow even when town growth is switched off, as fast as with the normal speed.
 * @param speed_setting Value of the town growth speed setting (0 = none, 1 = slow, 2 = normal, 3 = fast, 4 = very fast).
 * @param funded Whether new buildings are funded in the town.
 * @return Factor per mille; 0 when the town does not grow.
 */
uint32_t CalcTownGrowthSpeedFactor(uint8_t speed_setting, bool funded)
{
	static constexpr std::array<uint32_t, 5> SPEED_FACTOR = {0, 500, 1000, 2000, 4000};
	if (speed_setting == 0 && funded) return SPEED_FACTOR[2];
	return SPEED_FACTOR[std::min<size_t>(speed_setting, SPEED_FACTOR.size() - 1)];
}

/**
 * Compute the growth multiplier of a town.
 * @param effective_levels Effective level of every need, in the order of #TownNeed.
 * @param funded Whether new buildings are funded in the town (x1.5; it never lifts a level 0).
 * @param speed_setting Value of the town growth speed setting.
 * @return Multiplier per mille.
 */
uint32_t CalcTownGrowthMultiplier(std::span<const uint8_t> effective_levels, bool funded, uint8_t speed_setting)
{
	uint64_t multiplier = 1000;
	for (size_t i = 0; i < effective_levels.size() && i < to_underlying(TownNeed::End); i++) {
		multiplier = multiplier * GetTownNeedMultiplier(static_cast<TownNeed>(i), effective_levels[i]) / 1000;
	}
	if (funded) multiplier = multiplier * 3 / 2;
	multiplier = multiplier * CalcTownGrowthSpeedFactor(speed_setting, funded) / 1000;
	return ClampTo<uint32_t>(multiplier);
}

/**
 * Compute the yearly growth of a town: 2 * base * multiplier * sqrt(population).
 * The square root is taken with three decimals; towns smaller than #TOWN_GROWTH_MIN_POPULATION grow like a town of that size.
 * @param base Base growth constant, per mille.
 * @param multiplier Growth multiplier, per mille.
 * @param population Population of the town.
 * @return Growth in residents per year.
 */
uint32_t CalcTownYearlyGrowth(uint32_t base, uint32_t multiplier, uint32_t population)
{
	/* Square root of the population, in thousandths. */
	uint64_t root = IntSqrt64(static_cast<uint64_t>(std::max(population, TOWN_GROWTH_MIN_POPULATION)) * 1000000);
	return ClampTo<uint32_t>(2 * static_cast<uint64_t>(base) * multiplier * root / 1000000000);
}

/**
 * Get the budget a town earns on one day of the year.
 * The yearly growth is spread over the days so that the days of a year add up to exactly the yearly growth.
 * @param yearly_growth Growth in residents per year.
 * @param day Day of the year (wraps after a year).
 * @return Budget in milli-residents.
 */
uint32_t CalcTownDailyBudget(uint32_t yearly_growth, uint day)
{
	uint64_t total = static_cast<uint64_t>(yearly_growth) * 1000;
	day %= CalendarTime::DAYS_IN_YEAR;
	return static_cast<uint32_t>(total * (day + 1) / CalendarTime::DAYS_IN_YEAR - total * day / CalendarTime::DAYS_IN_YEAR);
}

/**
 * Estimate the ticks between two houses of a town, in the format of Town::growth_rate.
 * @param yearly_growth Growth in residents per year.
 * @param residents_per_house Typical residents of a new house.
 * @return Ticks between two houses minus one, or #TOWN_GROWTH_RATE_NONE when the town does not grow.
 */
uint16_t CalcTownGrowthRateEstimate(uint32_t yearly_growth, uint32_t residents_per_house)
{
	if (yearly_growth == 0) return TOWN_GROWTH_RATE_NONE;
	uint64_t ticks = static_cast<uint64_t>(CalendarTime::DAYS_IN_YEAR) * Ticks::DAY_TICKS * residents_per_house / yearly_growth;
	return static_cast<uint16_t>(std::clamp<uint64_t>(ticks, 1, TOWN_GROWTH_RATE_NONE - 1) - 1);
}

/* --- Needs of a town --- */

/**
 * Get the passengers or mail a town generated and the part of it that was transported last month.
 * @param t The town.
 * @param effect Town production effect of the cargo.
 * @param[out] production Amount generated.
 * @param[out] transported Amount transported.
 */
static void GetTownProductionLastMonth(const Town *t, TownProductionEffect effect, uint64_t &production, uint64_t &transported)
{
	production = 0;
	transported = 0;
	for (const CargoSpec *cs : CargoSpec::town_production_cargoes[effect]) {
		auto it = t->GetCargoSupplied(cs->Index());
		if (it == std::end(t->supplied)) continue;
		production += it->history[LAST_MONTH].production;
		transported += it->history[LAST_MONTH].transported;
	}
}

/**
 * Get the supply and the demand of a need for the last month.
 * @param t The town.
 * @param need The need.
 * @param[out] supplied Amount supplied last month.
 * @param[out] demand Amount needed for full coverage.
 */
static void GetTownNeedSupplyAndDemand(const Town *t, TownNeed need, uint32_t &supplied, uint32_t &demand)
{
	const EconomySettings &settings = _settings_game.economy;
	uint32_t population = t->cache.population;

	switch (need) {
		case TownNeed::Passengers:
		case TownNeed::Mail: {
			uint64_t production;
			uint64_t transported;
			GetTownProductionLastMonth(t, need == TownNeed::Passengers ? TownProductionEffect::Passengers : TownProductionEffect::Mail, production, transported);
			supplied = ClampTo<uint32_t>(transported);
			demand = ClampTo<uint32_t>(production * settings.town_need_transport_pct / 100);
			break;
		}

		case TownNeed::Goods:
			supplied = t->received[TownAcceptanceEffect::Goods].old_act;
			demand = population / std::max<uint16_t>(settings.town_need_goods_residents, 1);
			break;

		case TownNeed::Electricity:
			supplied = GetTownPowerReceived(t);
			demand = population / std::max<uint16_t>(settings.town_need_power_residents, 1);
			break;

		case TownNeed::Valuables:
			supplied = t->growth.valuables_received;
			demand = GetTownNeedDemand(t, need);
			break;

		default: NOT_REACHED();
	}
}

/**
 * Get the amount of a need a town has to be supplied with per month for full coverage.
 * Only the current population, the settings and the cargo history of last month are used,
 * so this can be called at any time (the power grid calls it before the monthly needs update).
 * @param t The town.
 * @param need The need.
 * @return Demand in cargo units (passengers, mail, goods) or electricity units per month.
 */
uint32_t GetTownNeedDemand(const Town *t, TownNeed need)
{
	switch (need) {
		case TownNeed::Passengers:
		case TownNeed::Mail: {
			uint64_t production;
			uint64_t transported;
			GetTownProductionLastMonth(t, need == TownNeed::Passengers ? TownProductionEffect::Passengers : TownProductionEffect::Mail, production, transported);
			return ClampTo<uint32_t>(production * _settings_game.economy.town_need_transport_pct / 100);
		}

		case TownNeed::Goods:
			return t->cache.population / std::max<uint16_t>(_settings_game.economy.town_need_goods_residents, 1);

		case TownNeed::Electricity:
			return t->cache.population / std::max<uint16_t>(_settings_game.economy.town_need_power_residents, 1);

		case TownNeed::Valuables:
			/* Never zero: without a demand the coverage of a bonus would count as neutral (level 3) for free. */
			return std::max<uint32_t>(t->cache.population / TownGrowthBalance::VALUABLES_RESIDENTS, 1);

		default: NOT_REACHED();
	}
}

/**
 * Update the supply, coverage and level of every need of a town.
 * Called monthly, after the cargo history was rotated and the received cargo was moved to last month.
 * @param t The town.
 */
void UpdateTownNeeds(Town *t)
{
	for (TownNeed need : EnumRange(TownNeed::End)) {
		TownNeedState &state = t->growth.needs[need];
		GetTownNeedSupplyAndDemand(t, need, state.supplied, state.demand);
		UpdateTownNeedCoverage(state, CalcTownNeedCoverage(state.supplied, state.demand, t->cache.population));
	}
	t->growth.valuables_received = 0;
}

/**
 * Get the population from which a need counts for the town.
 * @param need The need.
 * @return Population that unlocks the need.
 */
uint32_t GetTownNeedUnlockPopulation(TownNeed need)
{
	/* Without power plants in this climate electricity never becomes a need. */
	if (need == TownNeed::Electricity && !IsElectricityAvailable()) return UINT32_MAX;
	/* Without valuables in this climate there is no valuables bonus. */
	if (need == TownNeed::Valuables) {
		bool available = false;
		for (const CargoSpec *cs : CargoSpec::Iterate()) available |= IsValuablesCargo(cs->Index());
		if (!available) return UINT32_MAX;
	}
	return TownGrowthBalance::UNLOCK_POPULATION[to_underlying(need)];
}

/**
 * Check whether a need already counts for a town.
 * @param t The town.
 * @param need The need.
 * @return True iff the town is large enough for the need.
 */
bool IsTownNeedUnlocked(const Town *t, TownNeed need)
{
	return t->cache.population >= GetTownNeedUnlockPopulation(need);
}

/**
 * Get the level of a need that counts for the growth of a town.
 * @param t The town.
 * @param need The need.
 * @return Effective level 0..#TOWN_NEED_LEVEL_MAX; 0 blocks growth.
 * @see CalcTownNeedEffectiveLevel
 */
uint8_t GetTownNeedEffectiveLevel(const Town *t, TownNeed need)
{
	const TownNeedState &state = t->growth.needs[need];
	/* A bonus only helps: neither the unlock nor the small town rule lifts it. */
	if (IsTownNeedBonus(need)) return std::min(state.forced != TOWN_NEED_NOT_FORCED ? state.forced : state.level, TOWN_NEED_LEVEL_MAX);
	return CalcTownNeedEffectiveLevel(state.level, state.forced, t->cache.population, GetTownNeedUnlockPopulation(need));
}

/**
 * Get the growth multiplier of a need level.
 * @param level The level.
 * @return Multiplier per mille.
 */
uint16_t GetTownNeedLevelMultiplier(uint8_t level)
{
	return TownGrowthBalance::LEVEL_MULTIPLIER[std::min(level, TOWN_NEED_LEVEL_MAX)];
}

/**
 * Check whether a need is a bonus: it can only speed growth up, its level 0 is neutral and it never blocks growth.
 * @param need The need.
 * @return True iff the need is a bonus.
 */
bool IsTownNeedBonus(TownNeed need)
{
	return need == TownNeed::Valuables;
}

/**
 * Get the growth multiplier of a level of a need.
 * @param need The need.
 * @param level The level.
 * @return Multiplier per mille.
 */
uint16_t GetTownNeedMultiplier(TownNeed need, uint8_t level)
{
	if (IsTownNeedBonus(need)) return TownGrowthBalance::BONUS_LEVEL_MULTIPLIER[std::min(level, TOWN_NEED_LEVEL_MAX)];
	return GetTownNeedLevelMultiplier(level);
}

/**
 * Check whether a cargo is valuables for the growth bonus (armoured cargo: valuables, gold, diamonds).
 * @param cargo The cargo.
 * @return True iff deliveries of the cargo count for the valuables bonus.
 */
bool IsValuablesCargo(CargoType cargo)
{
	return IsValidCargoType(cargo) && CargoSpec::Get(cargo)->classes.Test(CargoClass::Armoured);
}

/**
 * Check whether every house of a town accepts a cargo, because the growth model counts its deliveries:
 * goods and valuables.
 * @param cargo The cargo.
 * @return True iff every house accepts the cargo.
 */
bool IsTownGrowthCargo(CargoType cargo)
{
	if (!IsValidCargoType(cargo)) return false;
	return CargoSpec::Get(cargo)->town_acceptance_effect == TownAcceptanceEffect::Goods || IsValuablesCargo(cargo);
}

/**
 * Get the current growth multiplier of a town: the product of the multipliers of the effective
 * need levels, x1.5 while new buildings are funded, times the town growth speed setting.
 * @param t The town.
 * @return Multiplier per mille; 0 when the town cannot grow.
 */
uint32_t GetTownGrowthMultiplier(const Town *t)
{
	std::array<uint8_t, to_underlying(TownNeed::End)> levels{};
	for (TownNeed need : EnumRange(TownNeed::End)) {
		levels[to_underlying(need)] = GetTownNeedEffectiveLevel(t, need);
	}
	return CalcTownGrowthMultiplier(levels, t->fund_buildings_months != 0, _settings_game.economy.town_growth_rate);
}

/**
 * Get the yearly growth of a town at a given multiplier and its current population.
 * @param t The town.
 * @param multiplier Growth multiplier, per mille.
 * @return Growth in residents per year.
 */
uint32_t GetTownYearlyGrowth(const Town *t, uint32_t multiplier)
{
	return CalcTownYearlyGrowth(_settings_game.economy.town_growth_base, multiplier, t->cache.population);
}

/**
 * Check whether a town receives the cargo a game script or the climate requires for growth.
 * @param t The town.
 * @return True iff every cargo goal of the town is met.
 */
static bool AreTownCargoGoalsMet(const Town *t)
{
	for (TownAcceptanceEffect i : EnumRange(TownAcceptanceEffect::End)) {
		switch (t->goal[i]) {
			case TOWN_GROWTH_WINTER:
				if (TileHeight(t->xy) >= GetSnowLine() && t->received[i].old_act == 0 && t->cache.population > ScaleHousePopulation(90)) return false;
				break;

			case TOWN_GROWTH_DESERT:
				if (GetTropicZone(t->xy) == TropicZone::Desert && t->received[i].old_act == 0 && t->cache.population > ScaleHousePopulation(60)) return false;
				break;

			default:
				if (t->goal[i] > t->received[i].old_act) return false;
				break;
		}
	}
	return true;
}

/**
 * Update the multiplier, the yearly growth and the growth status of a town, and its TownFlag::IsGrowing.
 * @param t The town.
 */
void UpdateTownGrowthState(Town *t)
{
	TownGrowthState &growth = t->growth;
	bool funded = t->fund_buildings_months != 0;

	growth.multiplier = GetTownGrowthMultiplier(t);
	growth.yearly_growth = GetTownYearlyGrowth(t, growth.multiplier);
	growth.blocking_need = TownNeed::End;

	if (_settings_game.economy.town_growth_rate == 0 && !funded) {
		growth.status = TownGrowthStatus::Disabled;
	} else if (t->flags.Test(TownFlag::CustomGrowth)) {
		growth.status = TownGrowthStatus::Custom;
	} else {
		growth.status = TownGrowthStatus::Growing;
		for (TownNeed need : EnumRange(TownNeed::End)) {
			if (!IsTownNeedBonus(need) && GetTownNeedEffectiveLevel(t, need) == 0) {
				growth.status = TownGrowthStatus::Blocked;
				growth.blocking_need = need;
				break;
			}
		}
		if (growth.status == TownGrowthStatus::Growing && !AreTownCargoGoalsMet(t)) growth.status = TownGrowthStatus::Blocked;
		if (growth.status == TownGrowthStatus::Growing && growth.builder_failures >= TOWN_BUILDER_NO_SPACE_FAILURES) growth.status = TownGrowthStatus::NoSpace;
	}

	bool growing;
	if (growth.status == TownGrowthStatus::Custom) {
		/* Growth controlled by a game script works as before: the cargo goals must be met unless funded. */
		growing = t->growth_rate != TOWN_GROWTH_RATE_NONE && (funded || AreTownCargoGoalsMet(t));
	} else {
		growing = growth.status == TownGrowthStatus::Growing;
	}
	t->flags.Set(TownFlag::IsGrowing, growing);
}

/**
 * Estimate the ticks between two houses of a town for Town::growth_rate, which older readers
 * (town window, NewGRF variables, scripts) still use.
 * @param t The town, without custom growth.
 * @return Estimated ticks between two houses minus one, or #TOWN_GROWTH_RATE_NONE when the town is not growing.
 */
uint16_t GetTownGrowthRateEstimate(const Town *t)
{
	if (t->growth.status != TownGrowthStatus::Growing) return TOWN_GROWTH_RATE_NONE;
	uint32_t residents_per_house = (t->cache.num_houses != 0 && t->cache.population != 0) ? t->cache.population / t->cache.num_houses : ScaleHousePopulation(30);
	return CalcTownGrowthRateEstimate(t->growth.yearly_growth, std::max<uint32_t>(residents_per_house, 1));
}

/**
 * Force the level of a need of a town (debugging, scripts), or let it be computed again.
 * The growth state of the town is updated at once.
 * @param t The town.
 * @param need The need.
 * @param level Level 0..#TOWN_NEED_LEVEL_MAX, or #TOWN_NEED_NOT_FORCED to compute the level from the coverage.
 * @note This changes the game state; call it from a command so every client does the same.
 */
void SetTownNeedForced(Town *t, TownNeed need, uint8_t level)
{
	assert(level <= TOWN_NEED_LEVEL_MAX || level == TOWN_NEED_NOT_FORCED);
	t->growth.needs[need].forced = level;

	UpdateTownGrowthState(t);
	if (!t->flags.Test(TownFlag::CustomGrowth)) t->growth_rate = GetTownGrowthRateEstimate(t);
	SetWindowDirty(WindowClass::TownView, t->index);
}

/* --- Growth budget --- */

/**
 * Set the growth status of a town after a builder step changed whether it has space to grow.
 * @param t The town.
 * @param status The new status (TownGrowthStatus::Growing or TownGrowthStatus::NoSpace).
 */
static void SetTownBuilderStatus(Town *t, TownGrowthStatus status)
{
	t->growth.status = status;
	t->flags.Set(TownFlag::IsGrowing, status == TownGrowthStatus::Growing);
	t->growth_rate = GetTownGrowthRateEstimate(t);
	SetWindowDirty(WindowClass::TownView, t->index);
}

/**
 * Grow a town whose growth rate is controlled by a game script: a builder step every Town::growth_rate ticks.
 * @param t The town.
 * @param modes The parts of the town that may be grown.
 */
static void TownCustomGrowthTick(Town *t, TownExpandModes modes)
{
	if (!t->flags.Test(TownFlag::IsGrowing)) return;

	int counter = static_cast<int>(t->grow_counter) - 1;
	if (counter < 0) {
		if (TownBuilderStep(t, modes).success) {
			counter = t->growth_rate;
		} else {
			/* If growth failed wait a bit before retrying */
			counter = std::min<uint16_t>(t->growth_rate, Ticks::TOWN_GROWTH_TICKS - 1);
		}
	}
	t->grow_counter = counter;
}

/**
 * Per tick part of town growth: earn the daily budget and spend it on builder steps.
 * Towns are staggered by their index, so they do not all do their work in the same tick.
 * @param t The town.
 */
void TownGrowthTick(Town *t)
{
	TownExpandModes modes{TownExpandMode::Buildings};
	if (_settings_game.economy.allow_town_roads) modes.Set(TownExpandMode::Roads);

	if (t->flags.Test(TownFlag::CustomGrowth)) {
		TownCustomGrowthTick(t, modes);
		return;
	}

	TownGrowthState &growth = t->growth;
	if (growth.retry_ticks != 0) growth.retry_ticks--;
	if (growth.status != TownGrowthStatus::Growing && growth.status != TownGrowthStatus::NoSpace) return;

	uint64_t tick = TimerGameTick::counter + t->index.base();

	if (tick % Ticks::DAY_TICKS == 0) {
		int64_t cap = static_cast<int64_t>(TownGrowthBalance::BUDGET_CAP_YEARS) * growth.yearly_growth * 1000;
		if (growth.budget < cap) {
			uint day = static_cast<uint>(tick / Ticks::DAY_TICKS % CalendarTime::DAYS_IN_YEAR);
			growth.budget = std::min<int64_t>(growth.budget + CalcTownDailyBudget(growth.yearly_growth, day), cap);
		}
	}

	if (tick % TOWN_BUILDER_STEP_TICKS != 0 || growth.budget <= 0 || growth.retry_ticks != 0) return;

	TownBuildResult result = TownBuilderStep(t, modes);
	if (result.success) {
		growth.budget -= static_cast<int64_t>(std::max<int32_t>(result.residents, 0)) * 1000;
		growth.builder_failures = 0;
		if (growth.status == TownGrowthStatus::NoSpace) SetTownBuilderStatus(t, TownGrowthStatus::Growing);
	} else {
		growth.retry_ticks = TownGrowthBalance::BUILDER_RETRY_TICKS;
		if (growth.builder_failures < UINT8_MAX) growth.builder_failures++;
		if (growth.status == TownGrowthStatus::Growing && growth.builder_failures >= TOWN_BUILDER_NO_SPACE_FAILURES) SetTownBuilderStatus(t, TownGrowthStatus::NoSpace);
	}
}
