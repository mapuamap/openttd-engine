/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file town_blocks.h Interface between the town code and the 3x3 block builder. */

#ifndef TOWN_BLOCKS_H
#define TOWN_BLOCKS_H

#include "house.h"
#include "map_func.h"
#include "tile_type.h"
#include "town_type.h"

struct Town;

/** Distance in tiles between two parallel road lines of the global town grid. */
static constexpr uint TOWN_BLOCK_PITCH = 4;

/**
 * Is a tile inside a block of the global town grid, i.e. not on one of its road lines?
 * @param tile The tile.
 * @return True if houses belong on this tile.
 */
inline bool IsTownBlockInteriorTile(TileIndex tile)
{
	return TileX(tile) % TOWN_BLOCK_PITCH != 0 && TileY(tile) % TOWN_BLOCK_PITCH != 0;
}

bool IsTownBlockHousePosition(TileIndex tile, BuildingFlags flags);

/* Block bookkeeping (town_blocks.cpp). */
void InitializeTownBlocks(Town *t);
void OnTownHouseTileBuilt(Town *t, TileIndex tile);
void OnTownHouseTileCleared(Town *t, TileIndex tile);
uint32_t GetTownBlockRadiusSquared(Town *t);

/* House placement used by the builder (town_cmd.cpp). */
HouseID TryBuildTownHouse(Town *t, TileIndex tile, TownExpandModes modes);
int32_t TryReplaceTownHouse(Town *t, TileIndex tile);
void LevelTownLand(TileIndex tile);

#endif /* TOWN_BLOCKS_H */
