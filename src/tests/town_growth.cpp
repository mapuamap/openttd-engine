/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file town_growth.cpp Test the calculations of the supply-driven town growth model. */

#include "../stdafx.h"

#include "../3rdparty/catch2/catch.hpp"

#include "../town.h"
#include "../town_growth.h"
#include "../town_growth_internal.h"
#include "../timer/timer_game_calendar.h"

#include "../safeguards.h"

/** Default base growth constant (setting economy.town_growth_base). */
static constexpr uint32_t BASE = 1031;
/** Multiplier with all four needs at level 5. */
static constexpr uint32_t ALL_FIVE = 6553;
/** Multiplier with all four needs at level 3. */
static constexpr uint32_t ALL_THREE = 1000;
/** Value of the town growth speed setting for normal speed. */
static constexpr uint8_t SPEED_NORMAL = 2;

/**
 * Let a town grow for a number of years with fixed needs.
 * @param population Initial population.
 * @param multiplier Growth multiplier, per mille.
 * @param years Number of years.
 * @return Population after the years.
 */
static uint32_t GrowForYears(uint32_t population, uint32_t multiplier, int years)
{
	/* Growth is earned daily from the yearly growth at the current population. */
	uint64_t milli = 0;
	for (int day = 0; day < years * CalendarTime::DAYS_IN_YEAR; day++) {
		milli +=CalcTownDailyBudget(CalcTownYearlyGrowth(BASE, multiplier, population), day);
		population += static_cast<uint32_t>(milli / 1000);
		milli %= 1000;
	}
	return population;
}

TEST_CASE("TownNeedLevelForCoverage")
{
	CHECK(GetTownNeedLevelForCoverage(0) == 0);
	CHECK(GetTownNeedLevelForCoverage(4) == 0);
	CHECK(GetTownNeedLevelForCoverage(5) == 1);
	CHECK(GetTownNeedLevelForCoverage(24) == 1);
	CHECK(GetTownNeedLevelForCoverage(25) == 2);
	CHECK(GetTownNeedLevelForCoverage(49) == 2);
	CHECK(GetTownNeedLevelForCoverage(50) == 3);
	CHECK(GetTownNeedLevelForCoverage(74) == 3);
	CHECK(GetTownNeedLevelForCoverage(75) == 4);
	CHECK(GetTownNeedLevelForCoverage(99) == 4);
	CHECK(GetTownNeedLevelForCoverage(100) == 5);
	CHECK(GetTownNeedLevelForCoverage(TOWN_NEED_COVERAGE_MAX) == 5);
}

TEST_CASE("TownNeedCoverage")
{
	CHECK(CalcTownNeedCoverage(70, 70, 1000) == 100);
	CHECK(CalcTownNeedCoverage(35, 70, 1000) == 50);
	CHECK(CalcTownNeedCoverage(0, 70, 1000) == 0);
	CHECK(CalcTownNeedCoverage(UINT32_MAX, 1, 1000) == TOWN_NEED_COVERAGE_MAX);

	/* Nothing demanded: an empty town and any supply count as fully covered, otherwise neutral. */
	CHECK(CalcTownNeedCoverage(0, 0, 0) == 100);
	CHECK(CalcTownNeedCoverage(5, 0, 50) == 100);
	CHECK(GetTownNeedLevelForCoverage(CalcTownNeedCoverage(0, 0, 50)) == TOWN_NEED_LEVEL_NEUTRAL);
}

TEST_CASE("TownNeedSmoothedCoverage")
{
	const std::array<uint16_t, 3> history = {100, 50, 0};
	CHECK(CalcTownNeedSmoothedCoverage(history) == 50);
	/* Rounded to the nearest percent. */
	const std::array<uint16_t, 3> rounding = {100, 100, 99};
	CHECK(CalcTownNeedSmoothedCoverage(rounding) == 100);
}

TEST_CASE("TownNeedCoverageHistory")
{
	/* The first month of a new town fills the whole history. */
	TownNeedState state;
	UpdateTownNeedCoverage(state, 100);
	CHECK(state.coverage_history == std::array<uint16_t, 3>{100, 100, 100});
	CHECK(state.coverage == 100);
	CHECK(state.level == TOWN_NEED_LEVEL_NEUTRAL + 1);

	/* Later months are averaged with the two before, newest first. */
	UpdateTownNeedCoverage(state, 10);
	CHECK(state.coverage_history == std::array<uint16_t, 3>{10, 100, 100});
	CHECK(state.coverage == 70);
	CHECK(state.level == TOWN_NEED_LEVEL_NEUTRAL);

	/* Without supply the level drops one step per month down to 0, and stays there. */
	TownNeedState unserved;
	for (int month = 0; month < 3; month++) UpdateTownNeedCoverage(unserved, 0);
	CHECK(unserved.coverage == 0);
	CHECK(unserved.level == 0);
	UpdateTownNeedCoverage(unserved, 0);
	CHECK(unserved.level == 0);
	CHECK(unserved.coverage_history == std::array<uint16_t, 3>{0, 0, 0});

	/* Full supply from then on lifts it one step per month. */
	UpdateTownNeedCoverage(unserved, 300);
	CHECK(unserved.coverage == 100);
	CHECK(unserved.level == 1);
}

TEST_CASE("TownNeedLevelHysteresis")
{
	CHECK(CalcTownNeedLevelStep(3, 3) == 3);
	CHECK(CalcTownNeedLevelStep(3, 0) == 2);
	CHECK(CalcTownNeedLevelStep(3, 5) == 4);
	CHECK(CalcTownNeedLevelStep(0, 5) == 1);
	CHECK(CalcTownNeedLevelStep(5, 0) == 4);
	CHECK(CalcTownNeedLevelStep(200, 0) == TOWN_NEED_LEVEL_MAX - 1);

	/* An unserved town drops from neutral to blocking in three months. */
	uint8_t level = TOWN_NEED_LEVEL_NEUTRAL;
	for (int month = 0; month < 3; month++) level = CalcTownNeedLevelStep(level, GetTownNeedLevelForCoverage(0));
	CHECK(level == 0);
	/* And full service lifts it one step per month. */
	level = CalcTownNeedLevelStep(level, GetTownNeedLevelForCoverage(100));
	CHECK(level == 1);
}

TEST_CASE("TownNeedEffectiveLevel")
{
	using namespace TownGrowthBalance;

	/* Unlocked need above the grace population: the level counts as it is, 0 blocks. */
	CHECK(CalcTownNeedEffectiveLevel(0, TOWN_NEED_NOT_FORCED, 6000, UNLOCK_POPULATION[3]) == 0);
	CHECK(CalcTownNeedEffectiveLevel(2, TOWN_NEED_NOT_FORCED, 6000, UNLOCK_POPULATION[3]) == 2);
	CHECK(CalcTownNeedEffectiveLevel(5, TOWN_NEED_NOT_FORCED, 6000, UNLOCK_POPULATION[3]) == 5);

	/* Locked need: at least neutral, but it can give a bonus. */
	CHECK(CalcTownNeedEffectiveLevel(0, TOWN_NEED_NOT_FORCED, 3000, UNLOCK_POPULATION[3]) == 3);
	CHECK(CalcTownNeedEffectiveLevel(1, TOWN_NEED_NOT_FORCED, 3000, UNLOCK_POPULATION[3]) == 3);
	CHECK(CalcTownNeedEffectiveLevel(4, TOWN_NEED_NOT_FORCED, 3000, UNLOCK_POPULATION[3]) == 4);

	/* Grace: below 1000 residents a level 0 counts as neutral; other levels count as they are. */
	CHECK(CalcTownNeedEffectiveLevel(0, TOWN_NEED_NOT_FORCED, GRACE_POPULATION - 1, UNLOCK_POPULATION[0]) == 3);
	CHECK(CalcTownNeedEffectiveLevel(1, TOWN_NEED_NOT_FORCED, GRACE_POPULATION - 1, UNLOCK_POPULATION[0]) == 3);
	CHECK(CalcTownNeedEffectiveLevel(0, TOWN_NEED_NOT_FORCED, GRACE_POPULATION, UNLOCK_POPULATION[0]) == 0);

	/* A forced level replaces the computed one; the unlock and grace rules still apply. */
	CHECK(CalcTownNeedEffectiveLevel(0, 5, 6000, UNLOCK_POPULATION[3]) == 5);
	CHECK(CalcTownNeedEffectiveLevel(5, 0, 6000, UNLOCK_POPULATION[3]) == 0);
	CHECK(CalcTownNeedEffectiveLevel(5, 0, 3000, UNLOCK_POPULATION[3]) == 3);
}

TEST_CASE("TownGrowthMultiplier")
{
	const std::array<uint8_t, 4> all_five = {5, 5, 5, 5};
	const std::array<uint8_t, 4> all_three = {3, 3, 3, 3};
	const std::array<uint8_t, 4> mixed = {5, 4, 3, 2};
	const std::array<uint8_t, 4> blocked = {5, 5, 0, 5};

	CHECK(CalcTownGrowthMultiplier(all_five, false, SPEED_NORMAL) == ALL_FIVE);
	CHECK(CalcTownGrowthMultiplier(all_three, false, SPEED_NORMAL) == ALL_THREE);
	CHECK(CalcTownGrowthMultiplier(mixed, false, SPEED_NORMAL) == 1456); // 1.6 * 1.3 * 0.7
	CHECK(CalcTownGrowthMultiplier(blocked, false, SPEED_NORMAL) == 0);

	/* Funding: x1.5, but it never lifts a blocking need. */
	CHECK(CalcTownGrowthMultiplier(all_three, true, SPEED_NORMAL) == 1500);
	CHECK(CalcTownGrowthMultiplier(blocked, true, SPEED_NORMAL) == 0);

	/* Town growth speed setting: none, slow, normal, fast, very fast. */
	CHECK(CalcTownGrowthMultiplier(all_three, false, 0) == 0);
	CHECK(CalcTownGrowthMultiplier(all_three, false, 1) == 500);
	CHECK(CalcTownGrowthMultiplier(all_three, false, 3) == 2000);
	CHECK(CalcTownGrowthMultiplier(all_three, false, 4) == 4000);
	/* Funded towns grow at normal speed even with town growth switched off. */
	CHECK(CalcTownGrowthMultiplier(all_three, true, 0) == 1500);
}

TEST_CASE("TownYearlyGrowth")
{
	/* Calibration: all needs at 5 (M = 1.6^4) or all at 3 (M = 1). */
	CHECK(CalcTownYearlyGrowth(BASE, ALL_FIVE, 1000) == 427);
	CHECK(CalcTownYearlyGrowth(BASE, ALL_FIVE, 500000) == 9554);
	CHECK(CalcTownYearlyGrowth(BASE, ALL_THREE, 1000) == 65);
	CHECK(CalcTownYearlyGrowth(BASE, 0, 1000) == 0);

	/* Small towns grow like a town of 200. */
	CHECK(CalcTownYearlyGrowth(BASE, ALL_THREE, 0) == CalcTownYearlyGrowth(BASE, ALL_THREE, TOWN_GROWTH_MIN_POPULATION));
	CHECK(CalcTownYearlyGrowth(BASE, ALL_THREE, 50) == 29);

	/* No overflow at the extremes. */
	CHECK(CalcTownYearlyGrowth(20000, 40000, UINT32_MAX) > CalcTownYearlyGrowth(20000, 40000, 100000000));
}

TEST_CASE("TownDailyBudget")
{
	for (uint32_t yearly : {0U, 1U, 29U, 65U, 427U, 9554U, 1000000U}) {
		uint64_t total = 0;
		uint32_t lowest = UINT32_MAX;
		uint32_t highest = 0;
		for (uint day = 0; day < CalendarTime::DAYS_IN_YEAR; day++) {
			uint32_t budget = CalcTownDailyBudget(yearly, day);
			total += budget;
			lowest = std::min(lowest, budget);
			highest = std::max(highest, budget);
		}
		/* A year adds up to exactly the yearly growth, spread evenly over the days. */
		CHECK(total == static_cast<uint64_t>(yearly) * 1000);
		CHECK(highest - lowest <= 1);
	}
	/* The day wraps after a year. */
	CHECK(CalcTownDailyBudget(65, 3) == CalcTownDailyBudget(65, 3 + CalendarTime::DAYS_IN_YEAR));
}

TEST_CASE("TownGrowthCalibration")
{
	/* A fully supplied town of 1000 reaches about 500000 in 100 years. */
	uint32_t full = GrowForYears(1000, ALL_FIVE, 100);
	CHECK(full > 480000);
	CHECK(full < 520000);

	/* With all needs neutral it reaches about 18000, like vanilla. */
	uint32_t neutral = GrowForYears(1000, ALL_THREE, 100);
	CHECK(neutral > 17000);
	CHECK(neutral < 19000);
}

TEST_CASE("TownGrowthRateEstimate")
{
	CHECK(CalcTownGrowthRateEstimate(0, 90) == TOWN_GROWTH_RATE_NONE);
	/* 65 residents per year in houses of 90: a house every 505 days. */
	CHECK(CalcTownGrowthRateEstimate(65, 90) == 37397);
	/* 9554 residents per year in houses of 90: a house every 254 ticks. */
	CHECK(CalcTownGrowthRateEstimate(9554, 90) == 253);
	CHECK(CalcTownGrowthRateEstimate(UINT32_MAX, 1) == 0);
}
