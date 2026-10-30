/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file town_power.cpp Tests for the allocation of electricity from power plants to towns. */

#include "../stdafx.h"

#include "../3rdparty/catch2/catch.hpp"

#include "../town_power.h"

#include "../safeguards.h"

/**
 * Make a list of power plants.
 * @param supplies Supply of every plant.
 * @return The plants.
 */
static std::vector<PowerGridPlant> MakePlants(std::initializer_list<uint32_t> supplies)
{
	std::vector<PowerGridPlant> plants;
	for (uint32_t supply : supplies) plants.emplace_back().supply = supply;
	return plants;
}

/**
 * Make a town for the allocation.
 * @param demand Electricity demand of the town.
 * @param links Indices of the linked plants, nearest first.
 * @return The town.
 */
static PowerGridTown MakeTown(uint32_t demand, std::initializer_list<uint32_t> links)
{
	PowerGridTown town;
	town.demand = demand;
	town.plants = links;
	return town;
}

/**
 * Check the invariants every allocation must satisfy.
 * @param plants The plants after the allocation.
 * @param towns The towns after the allocation.
 */
static void CheckInvariants(const std::vector<PowerGridPlant> &plants, const std::vector<PowerGridTown> &towns)
{
	std::vector<uint64_t> granted(plants.size(), 0);
	for (const PowerGridTown &town : towns) {
		REQUIRE(town.delivered.size() == town.plants.size());
		uint64_t received = 0;
		for (size_t j = 0; j < town.plants.size(); j++) {
			granted[town.plants[j]] += town.delivered[j];
			received += town.delivered[j];
		}
		CHECK(received <= town.demand);
	}
	for (size_t p = 0; p < plants.size(); p++) {
		CHECK(plants[p].granted == granted[p]);
		CHECK(plants[p].granted <= plants[p].supply);
	}
}

TEST_CASE("AllocatePowerGrid - one plant, two towns, enough supply")
{
	auto plants = MakePlants({100});
	std::vector<PowerGridTown> towns = {MakeTown(30, {0}), MakeTown(50, {0})};

	AllocatePowerGrid(plants, towns);

	CHECK(towns[0].delivered == std::vector<uint32_t>{30});
	CHECK(towns[1].delivered == std::vector<uint32_t>{50});
	CHECK(plants[0].requested == 80);
	CHECK(plants[0].granted == 80);
	CheckInvariants(plants, towns);
}

TEST_CASE("AllocatePowerGrid - oversubscribed plant shares proportionally")
{
	auto plants = MakePlants({60});
	std::vector<PowerGridTown> towns = {MakeTown(30, {0}), MakeTown(90, {0})};

	AllocatePowerGrid(plants, towns);

	CHECK(towns[0].delivered == std::vector<uint32_t>{15});
	CHECK(towns[1].delivered == std::vector<uint32_t>{45});
	CHECK(plants[0].requested == 120);
	CHECK(plants[0].granted == 60);
	CheckInvariants(plants, towns);
}

TEST_CASE("AllocatePowerGrid - oversubscribed plant hands out the rounding remainder")
{
	auto plants = MakePlants({10});
	std::vector<PowerGridTown> towns = {MakeTown(10, {0}), MakeTown(10, {0}), MakeTown(10, {0})};

	AllocatePowerGrid(plants, towns);

	CHECK(towns[0].delivered == std::vector<uint32_t>{4});
	CHECK(towns[1].delivered == std::vector<uint32_t>{3});
	CHECK(towns[2].delivered == std::vector<uint32_t>{3});
	CHECK(plants[0].granted == 10);
	CheckInvariants(plants, towns);
}

TEST_CASE("AllocatePowerGrid - demand is split evenly, nearest plants take the remainder")
{
	auto plants = MakePlants({100, 100, 100});
	std::vector<PowerGridTown> towns = {MakeTown(10, {2, 0, 1})};

	AllocatePowerGrid(plants, towns);

	CHECK(towns[0].delivered == std::vector<uint32_t>{4, 3, 3});
	CHECK(plants[2].requested == 4);
	CHECK(plants[0].requested == 3);
	CHECK(plants[1].requested == 3);
	CheckInvariants(plants, towns);
}

TEST_CASE("AllocatePowerGrid - second pass takes the deficit from plants with spare supply")
{
	SECTION("single town") {
		auto plants = MakePlants({20, 200});
		std::vector<PowerGridTown> towns = {MakeTown(100, {0, 1})};

		AllocatePowerGrid(plants, towns);

		CHECK(towns[0].delivered == std::vector<uint32_t>{20, 80});
		CHECK(plants[0].requested == 50);
		CHECK(plants[1].requested == 50);
		CHECK(plants[0].granted == 20);
		CHECK(plants[1].granted == 80);
		CheckInvariants(plants, towns);
	}

	SECTION("competing towns, deficit capped by the spare supply") {
		auto plants = MakePlants({10, 100});
		std::vector<PowerGridTown> towns = {MakeTown(60, {0, 1}), MakeTown(60, {1})};

		AllocatePowerGrid(plants, towns);

		CHECK(towns[0].delivered == std::vector<uint32_t>{10, 40});
		CHECK(towns[1].delivered == std::vector<uint32_t>{60});
		CHECK(plants[0].requested == 30);
		CHECK(plants[1].requested == 90);
		CHECK(plants[0].granted == 10);
		CHECK(plants[1].granted == 100);
		CheckInvariants(plants, towns);
	}

	SECTION("deficit split proportionally to the spare supply") {
		/* Requests of pass 1 are 34/33/33: plant 2 grants 10 of 34, plants 0 and 1 grant 33 each. */
		auto plants = MakePlants({40, 70, 10});
		std::vector<PowerGridTown> towns = {MakeTown(100, {2, 0, 1})};

		AllocatePowerGrid(plants, towns);

		/* Spare supply is 7 and 37: the deficit of 24 is split 24*7/44 = 3 and 24*37/44 = 20,
		 * the remainder of 1 goes to the nearest plant with spare supply. */
		CHECK(towns[0].delivered == std::vector<uint32_t>{10, 37, 53});
		CHECK(plants[0].granted == 37);
		CHECK(plants[1].granted == 53);
		CHECK(plants[2].granted == 10);
		CheckInvariants(plants, towns);
	}

	SECTION("plant oversubscribed in the second pass") {
		/* Pass 1: the shared plant 1 grants 20 to each town and has 10 spare; both towns ask it for 10 more. */
		auto plants = MakePlants({0, 50, 0});
		std::vector<PowerGridTown> towns = {MakeTown(40, {0, 1}), MakeTown(40, {2, 1})};

		AllocatePowerGrid(plants, towns);

		CHECK(towns[0].delivered == std::vector<uint32_t>{0, 25});
		CHECK(towns[1].delivered == std::vector<uint32_t>{0, 25});
		CHECK(plants[1].requested == 40);
		CHECK(plants[1].granted == 50);
		CheckInvariants(plants, towns);
	}
}

TEST_CASE("AllocatePowerGrid - zero supply")
{
	auto plants = MakePlants({0, 0});
	std::vector<PowerGridTown> towns = {MakeTown(50, {0, 1}), MakeTown(7, {1})};

	AllocatePowerGrid(plants, towns);

	CHECK(towns[0].delivered == std::vector<uint32_t>{0, 0});
	CHECK(towns[1].delivered == std::vector<uint32_t>{0});
	CHECK(plants[0].requested == 25);
	CHECK(plants[1].requested == 32);
	CHECK(plants[0].granted == 0);
	CHECK(plants[1].granted == 0);
	CheckInvariants(plants, towns);
}

TEST_CASE("AllocatePowerGrid - towns without demand or links")
{
	auto plants = MakePlants({100});
	std::vector<PowerGridTown> towns = {MakeTown(0, {0}), MakeTown(40, {}), MakeTown(20, {0})};

	AllocatePowerGrid(plants, towns);

	CHECK(towns[0].delivered == std::vector<uint32_t>{0});
	CHECK(towns[1].delivered.empty());
	CHECK(towns[2].delivered == std::vector<uint32_t>{20});
	CHECK(plants[0].requested == 20);
	CHECK(plants[0].granted == 20);
	CheckInvariants(plants, towns);
}

TEST_CASE("AllocatePowerGrid - large values do not overflow")
{
	auto plants = MakePlants({UINT32_MAX});
	std::vector<PowerGridTown> towns = {MakeTown(UINT32_MAX, {0}), MakeTown(UINT32_MAX, {0}), MakeTown(UINT32_MAX, {0})};

	AllocatePowerGrid(plants, towns);

	CHECK(plants[0].requested == 3ULL * UINT32_MAX);
	CHECK(plants[0].granted == UINT32_MAX);
	CheckInvariants(plants, towns);
}

TEST_CASE("AllocatePowerGrid - deterministic")
{
	/* A fixed pseudo random network; a linear congruential generator keeps it identical on every platform. */
	uint32_t seed = 12345;
	auto next = [&seed](uint32_t range) {
		seed = seed * 1103515245 + 12345;
		return (seed >> 16) % range;
	};

	std::vector<PowerGridPlant> plants;
	for (int p = 0; p < 20; p++) plants.emplace_back().supply = next(2000);

	std::vector<PowerGridTown> towns;
	for (int t = 0; t < 100; t++) {
		PowerGridTown &town = towns.emplace_back();
		town.demand = next(500);
		uint32_t links = 1 + next(4);
		for (uint32_t l = 0; l < links; l++) town.plants.push_back(next(20));
	}

	std::vector<PowerGridPlant> plants2 = plants;
	std::vector<PowerGridTown> towns2 = towns;

	AllocatePowerGrid(plants, towns);
	AllocatePowerGrid(plants2, towns2);

	for (size_t p = 0; p < plants.size(); p++) {
		CHECK(plants[p].requested == plants2[p].requested);
		CHECK(plants[p].granted == plants2[p].granted);
	}
	for (size_t t = 0; t < towns.size(); t++) {
		CHECK(towns[t].delivered == towns2[t].delivered);
	}
	CheckInvariants(plants, towns);

	/* Running it again on its own output must give the same result: outputs are overwritten, not accumulated. */
	AllocatePowerGrid(plants2, towns2);
	for (size_t p = 0; p < plants.size(); p++) CHECK(plants[p].granted == plants2[p].granted);
	for (size_t t = 0; t < towns.size(); t++) CHECK(towns[t].delivered == towns2[t].delivered);
}
