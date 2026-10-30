/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file town_growth_type.h Types of the supply-driven town growth model. */

#ifndef TOWN_GROWTH_TYPE_H
#define TOWN_GROWTH_TYPE_H

#include "core/enum_type.hpp"
#include "industry_type.h"

/** Supplies a town needs to grow. */
enum class TownNeed : uint8_t {
	Begin = 0, ///< Used for iteration.
	Passengers = Begin, ///< Share of the generated passengers that is transported.
	Mail, ///< Share of the generated mail that is transported.
	Goods, ///< Goods delivered to the town.
	Electricity, ///< Electricity received from the linked power plants.
	End, ///< End marker.
};

static constexpr uint8_t TOWN_NEED_LEVEL_MAX = 5; ///< Highest need level.
static constexpr uint8_t TOWN_NEED_LEVEL_NEUTRAL = 3; ///< Need level that neither speeds up nor slows down growth.
static constexpr uint8_t TOWN_NEED_NOT_FORCED = 0xFF; ///< Value of #TownNeedState::forced when the level is computed, not forced.

/** Why a town is or is not growing. */
enum class TownGrowthStatus : uint8_t {
	Growing = 0, ///< The growth budget accumulates and is spent by the builder.
	Blocked = 1, ///< An unlocked need is at level 0, see #TownGrowthState::blocking_need.
	NoSpace = 2, ///< The builder found no place to grow.
	Disabled = 3, ///< Town growth is switched off in the settings.
	Custom = 4, ///< Growth is controlled by a game script.
};

/** Supply state of one need of a town. */
struct TownNeedState {
	uint32_t supplied = 0; ///< Amount supplied last month (cargo units, or electricity units).
	uint32_t demand = 0; ///< Amount needed last month for full coverage (100%).
	std::array<uint16_t, 3> coverage_history{}; ///< Coverage in percent of the last three months, newest first.
	uint16_t coverage = 0; ///< Smoothed coverage in percent; can exceed 100.
	uint8_t level = TOWN_NEED_LEVEL_NEUTRAL; ///< Current level 0..5 from the coverage, before the unlock and grace rules.
	uint8_t forced = TOWN_NEED_NOT_FORCED; ///< Debug override of the level (console, scripts), or #TOWN_NEED_NOT_FORCED.
};

/** Electricity delivered to a town by one linked power plant. */
struct TownPowerLink {
	IndustryID plant = IndustryID::Invalid(); ///< The linked power plant.
	uint32_t delivered = 0; ///< Electricity received from this plant last month.
};

struct TownBlockCache;

/** Growth state of a town. */
struct TownGrowthState {
	EnumIndexArray<TownNeedState, TownNeed, TownNeed::End> needs{}; ///< State of every need.
	int64_t budget = 0; ///< Growth budget in milli-residents; while it is positive the town may build.
	uint32_t multiplier = 1000; ///< Combined growth multiplier of all needs, per mille.
	uint32_t yearly_growth = 0; ///< Expected growth in residents per year at the current multiplier.
	TownGrowthStatus status = TownGrowthStatus::Growing; ///< Why the town is or is not growing.
	TownNeed blocking_need = TownNeed::End; ///< The need that blocks growth while #status is #TownGrowthStatus::Blocked.
	std::vector<TownPowerLink> power_links{}; ///< Linked power plants, nearest first.
	uint16_t retry_ticks = 0; ///< Ticks to wait before the builder is tried again after a failure.
	uint8_t builder_failures = 0; ///< Consecutive failed builder steps; from a few on the town has no space to grow.
	std::shared_ptr<TownBlockCache> block_cache{}; ///< NOSAVE: blocks of the town grid used by the builder, rebuilt from the map after loading.
};

#endif /* TOWN_GROWTH_TYPE_H */
