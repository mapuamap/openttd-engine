/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file script_town.cpp Implementation of ScriptTown. */

#include "../../stdafx.h"
#include "script_town.hpp"
#include "script_map.hpp"
#include "script_error.hpp"
#include "../../town.h"
#include "../../townname_func.h"
#include "../../string_func.h"
#include "../../strings_func.h"
#include "../../station_base.h"
#include "../../landscape.h"
#include "../../town_cmd.h"
#include "../../town_growth.h"

#include "table/strings.h"

#include "../../safeguards.h"

/* static */ SQInteger ScriptTown::GetTownCount()
{
	return ::Town::GetNumItems();
}

/* static */ bool ScriptTown::IsValidTown(TownID town_id)
{
	return ::Town::IsValidID(town_id);
}

/* static */ std::optional<std::string> ScriptTown::GetName(TownID town_id)
{
	if (!IsValidTown(town_id)) return std::nullopt;

	return ::StrMakeValid(::GetString(STR_TOWN_NAME, town_id), {});
}

/* static */ bool ScriptTown::SetName(TownID town_id, Text *name)
{
	ScriptObjectRef counter(name);

	EnforceDeityMode(false);
	EnforcePrecondition(false, IsValidTown(town_id));
	std::string text;
	if (name != nullptr) {
		text = name->GetDecodedText();
		EnforcePreconditionCustomError(false, ::Utf8StringLength(text) < MAX_LENGTH_TOWN_NAME_CHARS, ScriptError::ERR_PRECONDITION_STRING_TOO_LONG);
	}

	return ScriptObject::Command<Commands::RenameTown>::Do(town_id, text);
}

/* static */ bool ScriptTown::SetText(TownID town_id, Text *text)
{
	ScriptObjectRef counter(text);

	EnforceDeityMode(false);
	EnforcePrecondition(false, IsValidTown(town_id));

	return ScriptObject::Command<Commands::TownSetText>::Do(town_id, text != nullptr ? text->GetEncodedText() : EncodedString{});
}

/* static */ SQInteger ScriptTown::GetPopulation(TownID town_id)
{
	if (!IsValidTown(town_id)) return -1;
	const Town *t = ::Town::Get(town_id);
	return t->cache.population;
}

/* static */ SQInteger ScriptTown::GetHouseCount(TownID town_id)
{
	if (!IsValidTown(town_id)) return -1;
	const Town *t = ::Town::Get(town_id);
	return t->cache.num_houses;
}

/* static */ TileIndex ScriptTown::GetLocation(TownID town_id)
{
	if (!IsValidTown(town_id)) return INVALID_TILE;
	const Town *t = ::Town::Get(town_id);
	return t->xy;
}

/* static */ SQInteger ScriptTown::GetLastMonthProduction(TownID town_id, CargoType cargo_type)
{
	if (!IsValidTown(town_id)) return -1;
	if (!ScriptCargo::IsValidCargo(cargo_type)) return -1;

	const Town *t = ::Town::Get(town_id);

	auto it = t->GetCargoSupplied(cargo_type);
	if (it == std::end(t->supplied)) return 0;

	return it->history[LAST_MONTH].production;
}

/* static */ SQInteger ScriptTown::GetLastMonthSupplied(TownID town_id, CargoType cargo_type)
{
	if (!IsValidTown(town_id)) return -1;
	if (!ScriptCargo::IsValidCargo(cargo_type)) return -1;

	const Town *t = ::Town::Get(town_id);

	auto it = t->GetCargoSupplied(cargo_type);
	if (it == std::end(t->supplied)) return 0;

	return it->history[LAST_MONTH].transported;
}

/* static */ SQInteger ScriptTown::GetLastMonthTransportedPercentage(TownID town_id, CargoType cargo_type)
{
	if (!IsValidTown(town_id)) return -1;
	if (!ScriptCargo::IsValidCargo(cargo_type)) return -1;

	const Town *t = ::Town::Get(town_id);
	return ::ToPercent8(t->GetPercentTransported(cargo_type));
}

/* static */ SQInteger ScriptTown::GetLastMonthReceived(TownID town_id, ScriptCargo::TownEffect towneffect_id)
{
	if (!IsValidTown(town_id)) return -1;
	if (!ScriptCargo::IsValidTownEffect(towneffect_id)) return -1;

	const Town *t = ::Town::Get(town_id);

	return t->received[static_cast<TownAcceptanceEffect>(towneffect_id)].old_act;
}

/* static */ bool ScriptTown::SetCargoGoal(TownID town_id, ScriptCargo::TownEffect towneffect_id, SQInteger goal)
{
	EnforceDeityMode(false);
	EnforcePrecondition(false, IsValidTown(town_id));
	EnforcePrecondition(false, ScriptCargo::IsValidTownEffect(towneffect_id));

	goal = Clamp<SQInteger>(goal, 0, UINT32_MAX);

	return ScriptObject::Command<Commands::TownCargoGoal>::Do(town_id, (::TownAcceptanceEffect)towneffect_id, goal);
}

/* static */ SQInteger ScriptTown::GetCargoGoal(TownID town_id, ScriptCargo::TownEffect towneffect_id)
{
	if (!IsValidTown(town_id)) return -1;
	if (!ScriptCargo::IsValidTownEffect(towneffect_id)) return -1;

	const Town *t = ::Town::Get(town_id);

	switch (t->goal[static_cast<TownAcceptanceEffect>(towneffect_id)]) {
		case TOWN_GROWTH_WINTER:
			if (TileHeight(t->xy) >= GetSnowLine() && t->cache.population > 90) return 1;
			return 0;

		case TOWN_GROWTH_DESERT:
			if (GetTropicZone(t->xy) == TropicZone::Desert && t->cache.population > 60) return 1;
			return 0;

		default: return t->goal[static_cast<TownAcceptanceEffect>(towneffect_id)];
	}
}

/* static */ bool ScriptTown::SetGrowthRate(TownID town_id, SQInteger days_between_town_growth)
{
	EnforceDeityMode(false);
	EnforcePrecondition(false, IsValidTown(town_id));
	uint16_t growth_rate;
	switch (days_between_town_growth) {
		case TOWN_GROWTH_NORMAL:
			growth_rate = 0;
			break;

		case TOWN_GROWTH_NONE:
			growth_rate = TOWN_GROWTH_RATE_NONE;
			break;

		default:
			EnforcePrecondition(false, (days_between_town_growth * ::Ticks::DAY_TICKS / ::Ticks::TOWN_GROWTH_TICKS) <= MAX_TOWN_GROWTH_TICKS);
			/* Don't use growth_rate 0 as it means GROWTH_NORMAL */
			growth_rate = std::max<SQInteger>(days_between_town_growth * ::Ticks::DAY_TICKS, 2u) - 1;
			break;
	}

	return ScriptObject::Command<Commands::TownGrowthRate>::Do(town_id, growth_rate);
}

/* static */ SQInteger ScriptTown::GetGrowthRate(TownID town_id)
{
	if (!IsValidTown(town_id)) return -1;

	const Town *t = ::Town::Get(town_id);

	if (t->growth_rate == TOWN_GROWTH_RATE_NONE) return TOWN_GROWTH_NONE;

	return RoundDivSU(t->growth_rate + 1, ::Ticks::DAY_TICKS);
}

/* static */ SQInteger ScriptTown::GetDistanceManhattanToTile(TownID town_id, TileIndex tile)
{
	return ScriptMap::DistanceManhattan(tile, GetLocation(town_id));
}

/* static */ SQInteger ScriptTown::GetDistanceSquareToTile(TownID town_id, TileIndex tile)
{
	return ScriptMap::DistanceSquare(tile, GetLocation(town_id));
}

/* static */ bool ScriptTown::IsWithinTownInfluence(TownID town_id, TileIndex tile)
{
	if (!IsValidTown(town_id)) return false;

	const Town *t = ::Town::Get(town_id);
	return ((uint32_t)GetDistanceSquareToTile(town_id, tile) <= t->cache.squared_town_zone_radius[to_underlying(HouseZone::TownEdge)]);
}

/* static */ bool ScriptTown::HasStatue(TownID town_id)
{
	EnforceCompanyModeValid(false);
	if (!IsValidTown(town_id)) return false;

	return ::Town::Get(town_id)->statues.Test(ScriptObject::GetCompany());
}

/* static */ bool ScriptTown::IsCity(TownID town_id)
{
	if (!IsValidTown(town_id)) return false;

	return ::Town::Get(town_id)->larger_town;
}

/* static */ SQInteger ScriptTown::GetRoadReworkDuration(TownID town_id)
{
	if (!IsValidTown(town_id)) return -1;

	return ::Town::Get(town_id)->road_build_months;
}

/* static */ SQInteger ScriptTown::GetFundBuildingsDuration(TownID town_id)
{
	if (!IsValidTown(town_id)) return -1;

	return ::Town::Get(town_id)->fund_buildings_months;
}

/* static */ ScriptCompany::CompanyID ScriptTown::GetExclusiveRightsCompany(TownID town_id)
{
	EnforceCompanyModeValid(ScriptCompany::COMPANY_INVALID);
	if (!IsValidTown(town_id)) return ScriptCompany::COMPANY_INVALID;

	return ScriptCompany::ToScriptCompanyID(::Town::Get(town_id)->exclusivity);
}

/* static */ SQInteger ScriptTown::GetExclusiveRightsDuration(TownID town_id)
{
	if (!IsValidTown(town_id)) return -1;

	return ::Town::Get(town_id)->exclusive_counter;
}

/* static */ bool ScriptTown::IsActionAvailable(TownID town_id, TownAction town_action)
{
	EnforceCompanyModeValid(false);
	if (!IsValidTown(town_id)) return false;

	return ::GetMaskOfTownActions(ScriptObject::GetCompany(), ::Town::Get(town_id)).Test(::TownAction(town_action));
}

/* static */ bool ScriptTown::PerformTownAction(TownID town_id, TownAction town_action)
{
	EnforceCompanyModeValid(false);
	EnforcePrecondition(false, IsValidTown(town_id));
	EnforcePrecondition(false, IsActionAvailable(town_id, town_action));

	return ScriptObject::Command<Commands::TownAction>::Do(town_id, ::TownAction(town_action));
}

/* static */ bool ScriptTown::ExpandTown(TownID town_id, SQInteger houses)
{
	EnforceDeityMode(false);
	EnforcePrecondition(false, IsValidTown(town_id));
	EnforcePrecondition(false, houses > 0);

	houses = std::min<SQInteger>(houses, UINT32_MAX);

	TownExpandModes modes{TownExpandMode::Buildings};

	if (_settings_game.economy.allow_town_roads) modes.Set(TownExpandMode::Roads);

	return ScriptObject::Command<Commands::ExpandTown>::Do(town_id, houses, modes);
}

/* static */ bool ScriptTown::FoundTown(TileIndex tile, TownSize size, bool city, RoadLayout layout, Text *name)
{
	ScriptObjectRef counter(name);

	EnforceDeityOrCompanyModeValid(false);
	EnforcePrecondition(false, ScriptCompanyMode::IsDeity() || _settings_game.economy.found_town != TownFounding::Forbidden);
	EnforcePrecondition(false, ::IsValidTile(tile));
	EnforcePrecondition(false, size == TOWN_SIZE_SMALL || size == TOWN_SIZE_MEDIUM || size == TOWN_SIZE_LARGE)
	EnforcePrecondition(false, ScriptCompanyMode::IsDeity() || size != TOWN_SIZE_LARGE);
	if (ScriptCompanyMode::IsDeity() || _settings_game.economy.found_town == TownFounding::CustomLayout) {
		EnforcePrecondition(false, layout >= ROAD_LAYOUT_ORIGINAL && layout <= ROAD_LAYOUT_RANDOM);
	} else {
		/* The layout parameter is ignored for AIs when custom layouts is disabled. */
		layout = (RoadLayout) (uint8_t)_settings_game.economy.town_layout;
	}

	std::string text;
	if (name != nullptr) {
		text = name->GetDecodedText();
		EnforcePreconditionCustomError(false, ::Utf8StringLength(text) < MAX_LENGTH_TOWN_NAME_CHARS, ScriptError::ERR_PRECONDITION_STRING_TOO_LONG);
	}
	uint32_t townnameparts;
	if (!GenerateTownName(ScriptObject::GetRandomizer(), &townnameparts)) {
		ScriptObject::SetLastError(ScriptError::ERR_NAME_IS_NOT_UNIQUE);
		return false;
	}

	return ScriptObject::Command<Commands::FoundTown>::Do(tile, (::TownSize)size, city, (::TownLayout)layout, false, townnameparts, text);
}

/* static */ ScriptTown::TownRating ScriptTown::GetRating(TownID town_id, ScriptCompany::CompanyID company_id)
{
	if (!IsValidTown(town_id)) return TOWN_RATING_INVALID;
	ScriptCompany::CompanyID company = ScriptCompany::ResolveCompanyID(company_id);
	if (company == ScriptCompany::COMPANY_INVALID) return TOWN_RATING_INVALID;

	::CompanyID c = ScriptCompany::FromScriptCompanyID(company);
	const Town *t = ::Town::Get(town_id);
	if (!t->have_ratings.Test(c)) {
		return TOWN_RATING_NONE;
	} else if (t->ratings[c] <= RATING_APPALLING) {
		return TOWN_RATING_APPALLING;
	} else if (t->ratings[c] <= RATING_VERYPOOR) {
		return TOWN_RATING_VERY_POOR;
	} else if (t->ratings[c] <= RATING_POOR) {
		return TOWN_RATING_POOR;
	} else if (t->ratings[c] <= RATING_MEDIOCRE) {
		return TOWN_RATING_MEDIOCRE;
	} else if (t->ratings[c] <= RATING_GOOD) {
		return TOWN_RATING_GOOD;
	} else if (t->ratings[c] <= RATING_VERYGOOD) {
		return TOWN_RATING_VERY_GOOD;
	} else if (t->ratings[c] <= RATING_EXCELLENT) {
		return TOWN_RATING_EXCELLENT;
	} else {
		return TOWN_RATING_OUTSTANDING;
	}
}

/* static */ SQInteger ScriptTown::GetDetailedRating(TownID town_id, ScriptCompany::CompanyID company_id)
{
	if (!IsValidTown(town_id)) return TOWN_RATING_INVALID;
	ScriptCompany::CompanyID company = ScriptCompany::ResolveCompanyID(company_id);
	if (company == ScriptCompany::COMPANY_INVALID) return TOWN_RATING_INVALID;

	const Town *t = ::Town::Get(town_id);
	return t->ratings[ScriptCompany::FromScriptCompanyID(company)];
}

/* static */ bool ScriptTown::ChangeRating(TownID town_id, ScriptCompany::CompanyID company, SQInteger delta)
{
	EnforceDeityMode(false);
	EnforcePrecondition(false, IsValidTown(town_id));
	company = ScriptCompany::ResolveCompanyID(company);
	EnforcePrecondition(false, company != ScriptCompany::COMPANY_INVALID);

	::CompanyID c = ScriptCompany::FromScriptCompanyID(company);
	const Town *t = ::Town::Get(town_id);
	int16_t new_rating = Clamp(t->ratings[c] + delta, RATING_MINIMUM, RATING_MAXIMUM);
	if (new_rating == t->ratings[c]) return false;

	return ScriptObject::Command<Commands::TownRating>::Do(town_id, c, new_rating);
}

/* static */ SQInteger ScriptTown::GetAllowedNoise(TownID town_id)
{
	if (!IsValidTown(town_id)) return -1;

	const Town *t = ::Town::Get(town_id);
	if (_settings_game.economy.station_noise_level) {
		return t->MaxTownNoise() - t->noise_reached;
	}

	int num = 0;
	for (const Station *st : Station::Iterate()) {
		if (st->town == t && st->facilities.Test(StationFacility::Airport) && st->airport.type != AT_OILRIG) num++;
	}
	return std::max(0, 2 - num);
}

/* static */ ScriptTown::RoadLayout ScriptTown::GetRoadLayout(TownID town_id)
{
	if (!IsValidTown(town_id)) return ROAD_LAYOUT_INVALID;

	return (ScriptTown::RoadLayout)((TownLayout)::Town::Get(town_id)->layout);
}

/* static */ bool ScriptTown::IsValidNeed(TownNeed need)
{
	return need >= NEED_PASSENGERS && need <= NEED_ELECTRICITY;
}

/* static */ SQInteger ScriptTown::GetNeedLevel(TownID town_id, TownNeed need)
{
	if (!IsValidTown(town_id) || !IsValidNeed(need)) return -1;

	return ::GetTownNeedEffectiveLevel(::Town::Get(town_id), static_cast<::TownNeed>(need));
}

/* static */ SQInteger ScriptTown::GetNeedRawLevel(TownID town_id, TownNeed need)
{
	if (!IsValidTown(town_id) || !IsValidNeed(need)) return -1;

	return ::Town::Get(town_id)->growth.needs[static_cast<::TownNeed>(need)].level;
}

/* static */ SQInteger ScriptTown::GetNeedLevelOverride(TownID town_id, TownNeed need)
{
	if (!IsValidTown(town_id) || !IsValidNeed(need)) return -1;

	uint8_t forced = ::Town::Get(town_id)->growth.needs[static_cast<::TownNeed>(need)].forced;
	return forced == TOWN_NEED_NOT_FORCED ? -1 : forced;
}

/* static */ SQInteger ScriptTown::GetNeedCoverage(TownID town_id, TownNeed need)
{
	if (!IsValidTown(town_id) || !IsValidNeed(need)) return -1;

	return ::Town::Get(town_id)->growth.needs[static_cast<::TownNeed>(need)].coverage;
}

/* static */ SQInteger ScriptTown::GetNeedSupplied(TownID town_id, TownNeed need)
{
	if (!IsValidTown(town_id) || !IsValidNeed(need)) return -1;

	return ::Town::Get(town_id)->growth.needs[static_cast<::TownNeed>(need)].supplied;
}

/* static */ SQInteger ScriptTown::GetNeedDemand(TownID town_id, TownNeed need)
{
	if (!IsValidTown(town_id) || !IsValidNeed(need)) return -1;

	return ::Town::Get(town_id)->growth.needs[static_cast<::TownNeed>(need)].demand;
}

/* static */ bool ScriptTown::IsNeedUnlocked(TownID town_id, TownNeed need)
{
	if (!IsValidTown(town_id) || !IsValidNeed(need)) return false;

	return ::IsTownNeedUnlocked(::Town::Get(town_id), static_cast<::TownNeed>(need));
}

/* static */ SQInteger ScriptTown::GetGrowthMultiplier(TownID town_id)
{
	if (!IsValidTown(town_id)) return -1;

	return ::Town::Get(town_id)->growth.multiplier;
}

/* static */ SQInteger ScriptTown::GetYearlyGrowth(TownID town_id)
{
	if (!IsValidTown(town_id)) return -1;

	return ::Town::Get(town_id)->growth.yearly_growth;
}

/* static */ SQInteger ScriptTown::GetGrowthBudget(TownID town_id)
{
	if (!IsValidTown(town_id)) return 0;

	/* The budget is kept in milli-residents. */
	return ::Town::Get(town_id)->growth.budget / 1000;
}

/* static */ ScriptTown::TownGrowthStatus ScriptTown::GetGrowthStatus(TownID town_id)
{
	if (!IsValidTown(town_id)) return GROWTH_STATUS_INVALID;

	return static_cast<TownGrowthStatus>(::Town::Get(town_id)->growth.status);
}

/* static */ ScriptTown::TownNeed ScriptTown::GetGrowthBlockingNeed(TownID town_id)
{
	if (!IsValidTown(town_id)) return NEED_INVALID;

	const ::TownGrowthState &growth = ::Town::Get(town_id)->growth;
	if (growth.status != ::TownGrowthStatus::Blocked || growth.blocking_need >= ::TownNeed::End) return NEED_INVALID;
	return static_cast<TownNeed>(growth.blocking_need);
}

/* static */ SQInteger ScriptTown::GetPowerPlantCount(TownID town_id)
{
	if (!IsValidTown(town_id)) return -1;

	return ::Town::Get(town_id)->growth.power_links.size();
}

/* static */ IndustryID ScriptTown::GetPowerPlant(TownID town_id, SQInteger index)
{
	if (!IsValidTown(town_id)) return IndustryID::Invalid();

	const auto &links = ::Town::Get(town_id)->growth.power_links;
	if (index < 0 || static_cast<size_t>(index) >= links.size()) return IndustryID::Invalid();
	return links[index].plant;
}

/* static */ bool ScriptTown::SetNeedLevelOverride(TownID town_id, TownNeed need, SQInteger level)
{
	EnforceDeityMode(false);
	EnforcePrecondition(false, IsValidTown(town_id));
	EnforcePrecondition(false, IsValidNeed(need));
	EnforcePrecondition(false, level >= -1 && level <= TOWN_NEED_LEVEL_MAX);

	uint8_t forced = level < 0 ? TOWN_NEED_NOT_FORCED : static_cast<uint8_t>(level);
	return ScriptObject::Command<Commands::TownForceNeedLevel>::Do(town_id, static_cast<::TownNeed>(need), forced);
}
