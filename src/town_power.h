/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file town_power.h Allocation of the electricity of power plants to linked towns, independent of the game state. */

#ifndef TOWN_POWER_H
#define TOWN_POWER_H

/** A power plant taking part in the electricity allocation. */
struct PowerGridPlant {
	uint32_t supply = 0; ///< Electricity the plant can deliver this month.
	uint64_t requested = 0; ///< Output: electricity requested from the plant in the first pass.
	uint64_t granted = 0; ///< Output: electricity the plant delivers in total, never more than #supply.
};

/** A town taking part in the electricity allocation. */
struct PowerGridTown {
	uint32_t demand = 0; ///< Electricity the town needs this month.
	std::vector<uint32_t> plants{}; ///< Indices of the linked plants in the plant list, nearest first.
	std::vector<uint32_t> delivered{}; ///< Output: electricity received from every linked plant, parallel to #plants.
};

void AllocatePowerGrid(std::span<PowerGridPlant> plants, std::span<PowerGridTown> towns);

#endif /* TOWN_POWER_H */
