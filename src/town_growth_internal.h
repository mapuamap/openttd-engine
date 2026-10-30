/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file town_growth_internal.h Internals of the needs and growth budget module.
 *
 * The pure calculations of the growth model, kept free of game state so they
 * can be unit tested, and the hooks town_cmd.cpp uses to drive the module.
 * Everything here is integer math, so it is deterministic in multiplayer.
 */

#ifndef TOWN_GROWTH_INTERNAL_H
#define TOWN_GROWTH_INTERNAL_H

#include "town_growth_type.h"

struct Town;

/** Number of consecutive failed builder steps after which a town reports #TownGrowthStatus::NoSpace. */
static constexpr uint8_t TOWN_BUILDER_NO_SPACE_FAILURES = 4;
/** The builder of a town is tried at most once per this many ticks. */
static constexpr uint TOWN_BUILDER_STEP_TICKS = 8;
/** Highest coverage, in percent, that is stored for a need. */
static constexpr uint16_t TOWN_NEED_COVERAGE_MAX = 999;
/** Population below which a town grows as fast as a town of this size. */
static constexpr uint32_t TOWN_GROWTH_MIN_POPULATION = 200;

uint16_t CalcTownNeedCoverage(uint32_t supplied, uint32_t demand, uint32_t population);
uint16_t CalcTownNeedSmoothedCoverage(std::span<const uint16_t> history);
uint8_t CalcTownNeedLevelStep(uint8_t previous, uint8_t target);
void UpdateTownNeedCoverage(TownNeedState &state, uint16_t coverage);
uint8_t CalcTownNeedEffectiveLevel(uint8_t level, uint8_t forced, uint32_t population, uint32_t unlock_population);
uint32_t CalcTownGrowthSpeedFactor(uint8_t speed_setting, bool funded);
uint32_t CalcTownGrowthMultiplier(std::span<const uint8_t> effective_levels, bool funded, uint8_t speed_setting);
uint32_t CalcTownYearlyGrowth(uint32_t base, uint32_t multiplier, uint32_t population);
uint32_t CalcTownDailyBudget(uint32_t yearly_growth, uint day);
uint16_t CalcTownGrowthRateEstimate(uint32_t yearly_growth, uint32_t residents_per_house);

void UpdateTownGrowthState(Town *t);
uint16_t GetTownGrowthRateEstimate(const Town *t);

#endif /* TOWN_GROWTH_INTERNAL_H */
