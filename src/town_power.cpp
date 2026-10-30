/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file town_power.cpp Power plants, their coal stock and the electricity they deliver to linked towns.
 *
 * A power plant is an industry that produces no cargo and accepts coal. Delivered coal
 * stays in the accepted cargo slot of the plant as a stock. Every town links to its
 * nearest power plants; once a month the plants burn just enough coal to cover the
 * electricity demand of their linked towns, shared proportionally when a plant cannot
 * cover all of it.
 */

#include "stdafx.h"
#include "cargotype.h"
#include "industry.h"
#include "settings_type.h"
#include "town.h"
#include "town_growth.h"
#include "town_power.h"
#include "window_func.h"

#include "safeguards.h"

/**
 * Get the accepted cargo slot that holds the fuel of a power plant.
 * @param i The industry.
 * @return The slot of the coal, or \c nullptr when the industry is no power plant.
 */
static const Industry::AcceptedCargo *GetPowerPlantFuelSlot(const Industry *i)
{
	if (std::ranges::any_of(i->produced, [](const Industry::ProducedCargo &p) { return IsValidCargoType(p.cargo); })) return nullptr;

	for (const Industry::AcceptedCargo &a : i->accepted) {
		if (IsValidCargoType(a.cargo) && CargoSpec::Get(a.cargo)->label == CT_COAL) return &a;
	}
	return nullptr;
}

/**
 * Can power plants exist in this game, i.e. is there an available industry type that accepts coal and produces nothing?
 * Sub-tropical and toyland climates have none, so their towns never need electricity.
 * @return True if some available industry type is a power plant.
 */
bool IsElectricityAvailable()
{
	for (IndustryType type = 0; type < NUM_INDUSTRYTYPES; type++) {
		const IndustrySpec *indspec = GetIndustrySpec(type);
		if (!indspec->enabled) continue;
		if (std::ranges::any_of(indspec->produced_cargo, [](CargoType c) { return IsValidCargoType(c); })) continue;
		if (std::ranges::any_of(indspec->accepts_cargo, [](CargoType c) { return IsValidCargoType(c) && CargoSpec::Get(c)->label == CT_COAL; })) return true;
	}
	return false;
}

/**
 * Is the industry a power plant, i.e. does it produce no cargo and accept coal?
 * @param i The industry.
 * @return True if the industry is a power plant.
 */
bool IsPowerPlant(const Industry *i)
{
	return GetPowerPlantFuelSlot(i) != nullptr;
}

/**
 * Get the cargo a power plant burns.
 * @param i The industry.
 * @return The coal cargo type, or #INVALID_CARGO when the industry is no power plant.
 */
CargoType GetPowerPlantFuelCargo(const Industry *i)
{
	const Industry::AcceptedCargo *fuel = GetPowerPlantFuelSlot(i);
	return fuel == nullptr ? INVALID_CARGO : fuel->cargo;
}

/**
 * Get the amount of fuel a power plant has in stock.
 * @param i The industry.
 * @return The stock of coal, or 0 when the industry is no power plant.
 */
uint32_t GetPowerPlantFuelStock(const Industry *i)
{
	const Industry::AcceptedCargo *fuel = GetPowerPlantFuelSlot(i);
	return fuel == nullptr ? 0 : fuel->waiting;
}

/**
 * Get the number of power plants a town wants to be linked to.
 * Small towns use one plant; from half of #TownGrowthBalance::POWER_LINK_RESIDENTS residents on a town
 * uses two plants plus one per #TownGrowthBalance::POWER_LINK_RESIDENTS residents, up to #TownGrowthBalance::POWER_LINK_MAX.
 * @param t The town.
 * @return The wanted number of links.
 */
uint32_t GetTownWantedPowerLinks(const Town *t)
{
	uint32_t population = t->cache.population;
	if (population < TownGrowthBalance::POWER_LINK_RESIDENTS / 2) return 1;
	return std::min(2 + population / TownGrowthBalance::POWER_LINK_RESIDENTS, TownGrowthBalance::POWER_LINK_MAX);
}

/**
 * Get the electricity a town received from all its linked power plants last month.
 * @param t The town.
 * @return The received electricity.
 */
uint32_t GetTownPowerReceived(const Town *t)
{
	uint64_t received = 0;
	for (const TownPowerLink &link : t->growth.power_links) received += link.delivered;
	return ClampTo<uint32_t>(received);
}

/**
 * Link every town to its nearest power plants.
 * A town links to at most #GetTownWantedPowerLinks plants whose centre lies within
 * the power link radius setting plus the radius of the town, measured as Manhattan
 * distance from the town centre. Equally distant plants are ordered by their index.
 * Links that stay keep what their plant delivered last month.
 */
void RebuildTownPowerLinks()
{
	/** A power plant that a town may link to. */
	struct Candidate {
		uint distance; ///< Manhattan distance from the town centre.
		IndustryID plant; ///< The power plant.

		auto operator<=>(const Candidate &) const = default;
	};

	std::vector<const Industry *> plants;
	for (const Industry *i : Industry::Iterate()) {
		if (IsPowerPlant(i)) plants.push_back(i);
	}

	std::vector<Candidate> candidates;
	std::vector<TownPowerLink> links;
	for (Town *t : Town::Iterate()) {
		uint radius = _settings_game.economy.town_power_link_radius + IntSqrt(t->cache.squared_town_zone_radius[to_underlying(HouseZone::TownEdge)]);

		candidates.clear();
		for (const Industry *i : plants) {
			uint distance = DistanceManhattan(t->xy, i->location.GetCentreTile());
			if (distance <= radius) candidates.push_back({distance, i->index});
		}

		size_t wanted = std::min<size_t>(GetTownWantedPowerLinks(t), candidates.size());
		std::partial_sort(candidates.begin(), candidates.begin() + wanted, candidates.end());

		links.clear();
		for (size_t c = 0; c < wanted; c++) {
			IndustryID plant = candidates[c].plant;
			auto old = std::ranges::find(t->growth.power_links, plant, &TownPowerLink::plant);
			links.push_back({plant, old == std::end(t->growth.power_links) ? 0 : old->delivered});
		}
		t->growth.power_links = links;
	}
}

/**
 * Grant the electricity requested over a set of links.
 * A plant that can serve all requests made to it grants them in full. Otherwise it grants
 * every request proportionally to its capacity, rounded down, and then hands out what the
 * rounding left over one unit per link in link order, so its whole capacity is used.
 * @param link_plant Plant of every link, in allocation order.
 * @param requests Electricity requested over every link.
 * @param capacity Electricity every plant can grant.
 * @param[out] requested Total electricity requested from every plant.
 * @return Electricity granted over every link.
 */
static std::vector<uint32_t> GrantPowerRequests(std::span<const uint32_t> link_plant, std::span<const uint32_t> requests, std::span<const uint32_t> capacity, std::vector<uint64_t> &requested)
{
	requested.assign(capacity.size(), 0);
	for (size_t l = 0; l < link_plant.size(); l++) requested[link_plant[l]] += requests[l];

	std::vector<uint32_t> left(capacity.begin(), capacity.end());
	std::vector<uint32_t> grants(link_plant.size(), 0);
	for (size_t l = 0; l < link_plant.size(); l++) {
		uint32_t p = link_plant[l];
		if (requested[p] <= capacity[p]) {
			grants[l] = requests[l];
		} else {
			grants[l] = static_cast<uint32_t>(static_cast<uint64_t>(requests[l]) * capacity[p] / requested[p]);
		}
		left[p] -= grants[l];
	}

	/* Only oversubscribed plants can have a request that is not fully granted. */
	for (size_t l = 0; l < link_plant.size(); l++) {
		uint32_t p = link_plant[l];
		if (left[p] > 0 && grants[l] < requests[l]) {
			grants[l]++;
			left[p]--;
		}
	}

	return grants;
}

/**
 * Share the electricity of power plants among the towns linked to them.
 *
 * Pass 1: every town requests its demand split evenly over its links; the nearest links
 * take the remainder of the division. A plant whose requests exceed its supply grants
 * them proportionally, see #GrantPowerRequests.
 *
 * Pass 2: every town that is still short requests its deficit, capped at the spare supply
 * of its linked plants, from those plants proportionally to their spare supply; the
 * nearest plants with spare supply take the rounding remainder. The plants grant these
 * requests from their spare supply the same way as in pass 1.
 *
 * The result only depends on the input: integer math, plants and towns in list order.
 * @param plants The power plants. Their #PowerGridPlant::requested and #PowerGridPlant::granted are written.
 * @param towns The towns with their links to the plants. Their #PowerGridTown::delivered is written.
 */
void AllocatePowerGrid(std::span<PowerGridPlant> plants, std::span<PowerGridTown> towns)
{
	/* All links in allocation order: towns in list order, then their links nearest first. */
	std::vector<uint32_t> link_plant;
	for (const PowerGridTown &town : towns) {
		for (uint32_t p : town.plants) {
			assert(p < plants.size());
			link_plant.push_back(p);
		}
	}

	std::vector<uint32_t> supply;
	supply.reserve(plants.size());
	for (const PowerGridPlant &plant : plants) supply.push_back(plant.supply);

	/* Pass 1: spread the demand of every town evenly over its links. */
	std::vector<uint32_t> requests;
	requests.reserve(link_plant.size());
	for (const PowerGridTown &town : towns) {
		uint32_t count = static_cast<uint32_t>(town.plants.size());
		for (uint32_t j = 0; j < count; j++) {
			requests.push_back(town.demand / count + (j < town.demand % count ? 1 : 0));
		}
	}

	std::vector<uint64_t> requested;
	std::vector<uint32_t> first = GrantPowerRequests(link_plant, requests, supply, requested);

	std::vector<uint32_t> spare = supply;
	for (size_t l = 0; l < link_plant.size(); l++) spare[link_plant[l]] -= first[l];
	for (size_t p = 0; p < plants.size(); p++) plants[p].requested = requested[p];

	/* Pass 2: towns still short ask the plants with spare supply for their deficit. */
	size_t base = 0;
	for (const PowerGridTown &town : towns) {
		size_t count = town.plants.size();
		uint64_t received = 0;
		uint64_t available = 0;
		for (size_t j = 0; j < count; j++) {
			received += first[base + j];
			available += spare[town.plants[j]];
		}

		uint64_t wanted = std::min<uint64_t>(town.demand - received, available);
		uint64_t asked = 0;
		for (size_t j = 0; j < count; j++) {
			uint32_t share = wanted == 0 ? 0 : static_cast<uint32_t>(wanted * spare[town.plants[j]] / available);
			requests[base + j] = share;
			asked += share;
		}
		for (size_t j = 0; j < count && asked < wanted; j++) {
			if (spare[town.plants[j]] == 0) continue;
			requests[base + j]++;
			asked++;
		}

		base += count;
	}

	std::vector<uint32_t> second = GrantPowerRequests(link_plant, requests, spare, requested);

	for (PowerGridPlant &plant : plants) plant.granted = 0;
	base = 0;
	for (PowerGridTown &town : towns) {
		size_t count = town.plants.size();
		town.delivered.assign(count, 0);
		for (size_t j = 0; j < count; j++) {
			town.delivered[j] = first[base + j] + second[base + j];
			plants[town.plants[j]].granted += town.delivered[j];
		}
		base += count;
	}
}

/**
 * Monthly update of the power grid: relink the towns to their nearest power plants,
 * share the electricity of every plant among its linked towns and burn the coal for it.
 * Must run before the needs of the towns are updated.
 */
void UpdatePowerGrid()
{
	RebuildTownPowerLinks();

	std::vector<Industry *> plant_industries;
	std::vector<PowerGridPlant> plants;
	std::vector<uint32_t> plant_index(Industry::GetPoolSize(), UINT32_MAX);
	for (Industry *i : Industry::Iterate()) {
		i->power_output = 0;
		i->power_demand = 0;
		if (!IsPowerPlant(i)) continue;

		plant_index[i->index.base()] = static_cast<uint32_t>(plants.size());
		plant_industries.push_back(i);
		plants.emplace_back().supply = GetPowerPlantFuelStock(i) * TownGrowthBalance::POWER_PER_FUEL;
	}

	std::vector<Town *> town_list;
	std::vector<PowerGridTown> towns;
	for (Town *t : Town::Iterate()) {
		if (t->growth.power_links.empty()) continue;

		town_list.push_back(t);
		PowerGridTown &town = towns.emplace_back();
		town.demand = GetTownNeedDemand(t, TownNeed::Electricity);
		for (const TownPowerLink &link : t->growth.power_links) town.plants.push_back(plant_index[link.plant.base()]);
	}

	AllocatePowerGrid(plants, towns);

	for (size_t n = 0; n < town_list.size(); n++) {
		std::vector<TownPowerLink> &links = town_list[n]->growth.power_links;
		for (size_t j = 0; j < links.size(); j++) links[j].delivered = towns[n].delivered[j];
	}

	for (size_t p = 0; p < plants.size(); p++) {
		Industry *i = plant_industries[p];
		i->power_demand = ClampTo<uint32_t>(plants[p].requested);
		i->power_output = ClampTo<uint32_t>(plants[p].granted);

		/* Burn the fuel for the delivered electricity. */
		auto fuel = i->GetCargoAccepted(GetPowerPlantFuelCargo(i));
		fuel->waiting -= ClampTo<uint16_t>(CeilDiv(i->power_output, TownGrowthBalance::POWER_PER_FUEL));

		SetWindowDirty(WindowClass::IndustryView, i->index);
	}
}

/**
 * Remove all links of towns to a power plant that is being removed.
 * @param plant The removed industry.
 */
void OnPowerPlantRemoved(IndustryID plant)
{
	for (Town *t : Town::Iterate()) {
		std::erase_if(t->growth.power_links, [plant](const TownPowerLink &link) { return link.plant == plant; });
	}
}
