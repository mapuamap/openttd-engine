/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file town_blocks.cpp The 3x3 block town builder of the supply-driven growth model.
 *
 * All towns share one global grid: road lines run along the tiles with X % 4 == 0 and
 * along the tiles with Y % 4 == 0. The 3x3 tiles between the lines form a block, which
 * is addressed by (X / 4, Y / 4). A town grows by
 *  1. filling free plots of its opened blocks with houses, nearest blocks first;
 *  2. replacing old houses by denser ones when they are below the zone of their position;
 *  3. opening a new block: the cheapest block next to the town gets its missing perimeter roads and a first house.
 *
 * A block is opened by a town while it holds at least one house of that town. This makes
 * the state of the builder a pure function of the house tiles on the map: it is never saved,
 * every client rebuilds it after loading and then keeps it up to date from the house
 * construction and removal hooks, exactly like the server does.
 *
 * The cost of a block in the plan only depends on its distance to the town centre, the
 * terrain and a deterministic jitter, so the player can foresee where a town will grow.
 */

#include "stdafx.h"
#include "town.h"
#include "town_blocks.h"
#include "town_growth.h"
#include "bridge.h"
#include "bridge_map.h"
#include "command_func.h"
#include "company_func.h"
#include "core/backup_type.hpp"
#include "core/random_func.hpp"
#include "map_func.h"
#include "rail_map.h"
#include "road.h"
#include "road_cmd.h"
#include "road_func.h"
#include "road_map.h"
#include "settings_type.h"
#include "slope_func.h"
#include "station_map.h"
#include "tile_map.h"
#include "town_map.h"
#include "tunnelbridge_cmd.h"
#include "tunnelbridge_map.h"
#include "water_map.h"

#include "safeguards.h"

/** Key of a block of the global town grid: block X in the low 16 bits, block Y in the high 16 bits. */
using TownBlockKey = uint32_t;

/**
 * The blocks of one town as seen by the builder.
 * Everything in here is derived from the house tiles of the town that lie inside blocks.
 */
struct TownBlockCache {
	std::map<TownBlockKey, uint8_t> house_tiles{}; ///< Opened blocks with the number of house tiles of the town inside.
	std::set<std::pair<uint32_t, TownBlockKey>> order{}; ///< Opened blocks by plan cost, nearest first.
	std::map<TownBlockKey, uint8_t> frontier{}; ///< Blocks next to an opened block, with their number of opened neighbours.
	std::map<uint32_t, uint32_t> distances{}; ///< Number of opened blocks per squared distance (in tiles) of their centre to the town centre.
};

static constexpr uint BLOCK_PLOTS = 9; ///< Number of plots (tiles) inside a block.
static constexpr uint BLOCK_MAX_UNUSABLE_PLOTS = BLOCK_PLOTS / 2; ///< A block with more unusable plots is not opened.

/* Plan costs. One squared tile of distance to the town centre costs PLAN_DISTANCE_WEIGHT. */
static constexpr uint32_t PLAN_DISTANCE_WEIGHT = 4; ///< Cost per squared tile of distance between the block centre and the town centre.
static constexpr uint32_t PLAN_JITTER = 64; ///< Range of the deterministic jitter that makes the town outline less regular.
static constexpr uint32_t PENALTY_PLOT_SLOPED = 8; ///< Plot that needs a foundation or levelling.
static constexpr uint32_t PENALTY_PLOT_UNUSABLE = 96; ///< Plot that cannot hold a house: water, steep slope.
static constexpr uint32_t PENALTY_PLOT_FOREIGN = 160; ///< Plot taken by something else: rail, station, industry, object, house of another town.
static constexpr uint32_t PENALTY_ROAD_SLOPED = 4; ///< Road piece on a slope.
static constexpr uint32_t PENALTY_ROAD_CROSSING = 48; ///< Road piece that makes a level crossing.
static constexpr uint32_t PENALTY_ROAD_BRIDGE = 64; ///< Road piece that needs a bridge.
static constexpr uint32_t PENALTY_ROAD_BLOCKED = 48; ///< Road piece that cannot be built.

static constexpr uint TOWN_BRIDGE_BASE_LENGTH = 5; ///< Longest bridge of a small town.
static constexpr uint TOWN_BRIDGE_MAX_LENGTH = 11; ///< Longest bridge of any town.

/**
 * Make the key of a block.
 * @param bx Block X (tile X / 4).
 * @param by Block Y (tile Y / 4).
 * @return The key.
 */
static inline TownBlockKey MakeBlockKey(uint bx, uint by)
{
	return by << 16 | bx;
}

/**
 * Get the block X of a block key.
 * @param key The block.
 * @return Tile X / 4 of the block.
 */
static inline uint GetBlockX(TownBlockKey key)
{
	return GB(key, 0, 16);
}

/**
 * Get the block Y of a block key.
 * @param key The block.
 * @return Tile Y / 4 of the block.
 */
static inline uint GetBlockY(TownBlockKey key)
{
	return GB(key, 16, 16);
}

/**
 * Get the block a tile lies in.
 * @param tile The tile, inside the block.
 * @return The key of the block.
 */
static inline TownBlockKey GetBlockKey(TileIndex tile)
{
	return MakeBlockKey(TileX(tile) / TOWN_BLOCK_PITCH, TileY(tile) / TOWN_BLOCK_PITCH);
}

/**
 * Get the northern corner of a block, a crossing of two road lines.
 * @param key The block.
 * @return The tile of the corner; the block interior starts one tile further south.
 */
static inline TileIndex GetBlockOrigin(TownBlockKey key)
{
	return TileXY(GetBlockX(key) * TOWN_BLOCK_PITCH, GetBlockY(key) * TOWN_BLOCK_PITCH);
}

/**
 * Can a block be used by towns? Its perimeter must not touch the edge of the map.
 * @param bx Block X.
 * @param by Block Y.
 * @return True if the block lies completely on the map.
 */
static bool IsValidBlock(uint bx, uint by)
{
	return bx >= 1 && by >= 1 && (bx + 1) * TOWN_BLOCK_PITCH < Map::MaxX() && (by + 1) * TOWN_BLOCK_PITCH < Map::MaxY();
}

/**
 * Call a function for each valid block that shares a side with a block.
 * @param key The block.
 * @param func Function to call with the key of the neighbour.
 */
template <typename Func>
static void ForEachBlockNeighbour(TownBlockKey key, Func func)
{
	static const std::array<std::pair<int, int>, 4> offsets = {{{1, 0}, {-1, 0}, {0, 1}, {0, -1}}};
	for (const auto &[dx, dy] : offsets) {
		int bx = static_cast<int>(GetBlockX(key)) + dx;
		int by = static_cast<int>(GetBlockY(key)) + dy;
		if (bx < 0 || by < 0 || !IsValidBlock(bx, by)) continue;
		func(MakeBlockKey(bx, by));
	}
}

/**
 * Get the squared distance between the centre of a block and the centre of a town.
 * @param t The town.
 * @param key The block.
 * @return Squared distance in tiles.
 */
static uint32_t GetBlockDistanceSquared(const Town *t, TownBlockKey key)
{
	int dx = static_cast<int>(GetBlockX(key) * TOWN_BLOCK_PITCH + TOWN_BLOCK_PITCH / 2) - static_cast<int>(TileX(t->xy));
	int dy = static_cast<int>(GetBlockY(key) * TOWN_BLOCK_PITCH + TOWN_BLOCK_PITCH / 2) - static_cast<int>(TileY(t->xy));
	return dx * dx + dy * dy;
}

/**
 * Get the plan cost of a block without the terrain: its distance to the town centre plus a
 * deterministic jitter, so the town does not grow as a perfect diamond and every town has its own outline.
 * @param t The town.
 * @param key The block.
 * @return The cost.
 */
static uint32_t GetBlockPlanCost(const Town *t, TownBlockKey key)
{
	uint32_t hash = key * 0x9E3779B1U ^ t->index.base() * 0x85EBCA77U;
	hash ^= hash >> 15;
	hash *= 0x2C1B3C6DU;
	hash ^= hash >> 12;
	hash *= 0x297A2D39U;
	hash ^= hash >> 15;
	return GetBlockDistanceSquared(t, key) * PLAN_DISTANCE_WEIGHT + hash % PLAN_JITTER;
}

/**
 * Register a block that just got its first house of the town.
 * @param t The town.
 * @param cache The blocks of the town.
 * @param key The block.
 */
static void AddOpenedBlock(const Town *t, TownBlockCache &cache, TownBlockKey key)
{
	cache.order.emplace(GetBlockPlanCost(t, key), key);
	cache.distances[GetBlockDistanceSquared(t, key)]++;
	cache.frontier.erase(key);
	ForEachBlockNeighbour(key, [&cache](TownBlockKey neighbour) {
		if (!cache.house_tiles.contains(neighbour)) cache.frontier[neighbour]++;
	});
}

/**
 * Unregister a block that lost its last house of the town.
 * @param t The town.
 * @param cache The blocks of the town.
 * @param key The block.
 */
static void RemoveOpenedBlock(const Town *t, TownBlockCache &cache, TownBlockKey key)
{
	cache.order.erase({GetBlockPlanCost(t, key), key});

	auto distance = cache.distances.find(GetBlockDistanceSquared(t, key));
	if (distance != cache.distances.end() && --distance->second == 0) cache.distances.erase(distance);

	uint8_t opened_neighbours = 0;
	ForEachBlockNeighbour(key, [&cache, &opened_neighbours](TownBlockKey neighbour) {
		if (cache.house_tiles.contains(neighbour)) {
			opened_neighbours++;
			return;
		}
		auto it = cache.frontier.find(neighbour);
		if (it != cache.frontier.end() && --it->second == 0) cache.frontier.erase(it);
	});
	if (opened_neighbours != 0) cache.frontier[key] = opened_neighbours;
}

/**
 * Build the block caches of all towns that do not have one yet, e.g. after loading a game.
 * One pass over the map serves all towns.
 */
static void BuildMissingBlockCaches()
{
	std::vector<TownBlockCache *> building;
	for (Town *t : Town::Iterate()) {
		if (t->growth.block_cache != nullptr) continue;
		t->growth.block_cache = std::make_shared<TownBlockCache>();
		if (building.size() <= t->index.base()) building.resize(t->index.base() + 1, nullptr);
		building[t->index.base()] = t->growth.block_cache.get();
	}
	if (building.empty()) return;

	for (const auto tile : Map::Iterate()) {
		if (!IsTileType(tile, TileType::House) || !IsTownBlockInteriorTile(tile)) continue;
		TownID town = GetTownIndex(tile);
		if (town.base() >= building.size() || building[town.base()] == nullptr) continue;
		building[town.base()]->house_tiles[GetBlockKey(tile)]++;
	}

	for (const Town *t : Town::Iterate()) {
		if (t->index.base() >= building.size() || building[t->index.base()] == nullptr) continue;
		TownBlockCache &cache = *building[t->index.base()];
		for (const auto &[key, count] : cache.house_tiles) AddOpenedBlock(t, cache, key);
	}
}

/**
 * Get the block cache of a town, building it from the map if needed.
 * @param t The town.
 * @return The cache; it stays valid even if the town drops it meanwhile.
 */
static std::shared_ptr<TownBlockCache> GetBlockCache(Town *t)
{
	if (t->growth.block_cache == nullptr) BuildMissingBlockCaches();
	return t->growth.block_cache;
}

/**
 * Give a new town an empty block cache.
 * @param t The town that is being founded.
 */
void InitializeTownBlocks(Town *t)
{
	t->growth.block_cache = std::make_shared<TownBlockCache>();
}

/**
 * Register a new house tile of a town.
 * @param t The town owning the house.
 * @param tile The tile of the house.
 */
void OnTownHouseTileBuilt(Town *t, TileIndex tile)
{
	TownBlockCache *cache = t->growth.block_cache.get();
	if (cache == nullptr || !IsTownBlockInteriorTile(tile)) return;

	TownBlockKey key = GetBlockKey(tile);
	if (++cache->house_tiles[key] == 1) AddOpenedBlock(t, *cache, key);
}

/**
 * Unregister a removed house tile of a town.
 * @param t The town that owned the house.
 * @param tile The tile of the house.
 */
void OnTownHouseTileCleared(Town *t, TileIndex tile)
{
	TownBlockCache *cache = t->growth.block_cache.get();
	if (cache == nullptr || !IsTownBlockInteriorTile(tile)) return;

	TownBlockKey key = GetBlockKey(tile);
	auto it = cache->house_tiles.find(key);
	if (it == cache->house_tiles.end()) {
		/* The cache does not match the map; build it again from the map when it is needed next. */
		t->growth.block_cache.reset();
		return;
	}
	if (--it->second != 0) return;

	cache->house_tiles.erase(it);
	RemoveOpenedBlock(t, *cache, key);
}

/**
 * Get the squared radius of a town: the distance between the town centre and the centre of its farthest opened block.
 * @param t The town.
 * @return Squared radius in tiles, 0 if the town has no blocks.
 */
uint32_t GetTownBlockRadiusSquared(Town *t)
{
	std::shared_ptr<TownBlockCache> cache = GetBlockCache(t);
	return cache->distances.empty() ? 0 : cache->distances.rbegin()->first;
}

/**
 * Check whether a house of the given size may start at a tile, following the block layout:
 * 2x2 houses fill a corner of the block, 2x1 and 1x2 houses lie along an edge of the block
 * and 1x1 houses may be built on any tile of the block, its centre included.
 * @param tile The northern tile of the house.
 * @param flags The building flags of the house, giving its size.
 * @return True if the house fits the block layout.
 */
bool IsTownBlockHousePosition(TileIndex tile, BuildingFlags flags)
{
	uint x = TileX(tile) % TOWN_BLOCK_PITCH;
	uint y = TileY(tile) % TOWN_BLOCK_PITCH;
	if (x == 0 || y == 0) return false;

	if (flags.Test(BuildingFlag::Size2x2)) return x <= 2 && y <= 2;
	if (flags.Test(BuildingFlag::Size2x1)) return x <= 2 && y != 2;
	if (flags.Test(BuildingFlag::Size1x2)) return x != 2 && y <= 2;
	return true;
}

/**
 * Quick check whether a tile could get a house: bare land or trees, not too steep, nothing above.
 * @param tile The tile.
 * @return True if the tile is a free plot.
 */
static bool IsFreeHousePlot(TileIndex tile)
{
	if (!IsTileType(tile, TileType::Clear) && !IsTileType(tile, TileType::Trees)) return false;
	if (IsSteepSlope(GetTileSlope(tile))) return false;
	return !IsBridgeAbove(tile);
}

/**
 * Try to build a house on one of the free plots of a block.
 * @param t The town.
 * @param key The block.
 * @param modes The parts of the town that may be grown.
 * @return The house that was built, or INVALID_HOUSE_ID.
 */
static HouseID BuildHouseInBlock(Town *t, TownBlockKey key, TownExpandModes modes)
{
	TileIndex origin = GetBlockOrigin(key);
	std::array<TileIndex, BLOCK_PLOTS> plots;
	uint count = 0;
	for (uint y = 1; y < TOWN_BLOCK_PITCH; y++) {
		for (uint x = 1; x < TOWN_BLOCK_PITCH; x++) {
			TileIndex tile = origin + TileDiffXY(x, y);
			if (IsFreeHousePlot(tile)) plots[count++] = tile;
		}
	}
	if (count == 0) return INVALID_HOUSE_ID;

	uint first = RandomRange(count);
	for (uint i = 0; i < count; i++) {
		TileIndex tile = plots[(first + i) % count];
		/* The plot might have been covered by a multi-tile house or levelled away meanwhile. */
		if (!IsFreeHousePlot(tile)) continue;
		if (!IsTileFlat(tile) && (!_settings_game.construction.build_on_slopes || Chance16(1, 6))) LevelTownLand(tile);

		HouseID house = TryBuildTownHouse(t, tile, modes);
		if (house != INVALID_HOUSE_ID) return house;
	}
	return INVALID_HOUSE_ID;
}

/**
 * Builder step 1: build a house on a free plot of an opened block, nearest blocks first.
 * @param t The town.
 * @param cache The blocks of the town.
 * @param modes The parts of the town that may be grown.
 * @param[out] result What the step built.
 * @return True if a house was built.
 */
static bool FillOpenedBlocks(Town *t, TownBlockCache &cache, TownExpandModes modes, TownBuildResult &result)
{
	for (const auto &[cost, key] : cache.order) {
		auto houses = cache.house_tiles.find(key);
		if (houses != cache.house_tiles.end() && houses->second >= BLOCK_PLOTS) continue;

		HouseID house = BuildHouseInBlock(t, key, modes);
		if (house == INVALID_HOUSE_ID) continue;

		result.success = true;
		result.residents = ScaleHousePopulation(HouseSpec::Get(house)->population);
		return true;
	}
	return false;
}

/**
 * Builder step 2: replace an old house that is not dense enough for its place by a denser one, nearest blocks first.
 * @param t The town.
 * @param cache The blocks of the town.
 * @param[out] result What the step built.
 * @return True if a house was replaced.
 */
static bool DensifyOpenedBlocks(Town *t, TownBlockCache &cache, TownBuildResult &result)
{
	for (const auto &[cost, key] : cache.order) {
		TileIndex origin = GetBlockOrigin(key);
		for (uint y = 1; y < TOWN_BLOCK_PITCH; y++) {
			for (uint x = 1; x < TOWN_BLOCK_PITCH; x++) {
				TileIndex tile = origin + TileDiffXY(x, y);
				if (!IsTileType(tile, TileType::House) || GetTownIndex(tile) != t->index) continue;

				int32_t residents = TryReplaceTownHouse(t, tile);
				if (residents < 0) continue;

				/* The iteration must stop here: replacing the house may have changed the opened blocks. */
				result.success = true;
				result.residents = residents;
				return true;
			}
		}
	}
	return false;
}

/**
 * Does a tile carry a road along a road line?
 * @param tile The tile.
 * @param axis The direction of the road line.
 * @return True if there is a road, road stop, road bridge or road tunnel along the axis.
 */
static bool HasRoadAlong(TileIndex tile, Axis axis)
{
	RoadBits axis_bits = AxisToRoadBits(axis);
	if (GetAnyRoadBits(tile, RoadTramType::Road, true).All(axis_bits)) return true;

	/* A road bridge over the line. */
	if (IsBridgeAbove(tile) && GetBridgeAxis(tile) == axis) {
		return GetTunnelBridgeTransportType(GetNorthernBridgeEnd(tile)) == TransportType::Road;
	}
	return false;
}

/** What a tile of a road line needs before the road along the line is complete. */
enum class RoadLineTile : uint8_t {
	Done, ///< The road is there.
	Buildable, ///< A road piece can probably be built.
	Sloped, ///< A road piece can probably be built, on a slope.
	Crossing, ///< A level crossing can probably be built.
	Bridge, ///< Water or rail; a bridge has to span it.
	Blocked, ///< Nothing can be built without demolishing something.
};

/**
 * Classify a tile of a road line.
 * @param tile The tile.
 * @param axis The direction of the road line.
 * @return What the tile needs.
 */
static RoadLineTile ClassifyRoadLineTile(TileIndex tile, Axis axis)
{
	if (HasRoadAlong(tile, axis)) return RoadLineTile::Done;
	if (IsBridgeAbove(tile)) return RoadLineTile::Blocked;

	switch (GetTileType(tile)) {
		case TileType::Clear:
		case TileType::Trees: {
			Slope slope = GetTileSlope(tile);
			if (IsSteepSlope(slope)) return RoadLineTile::Blocked;
			return slope == SLOPE_FLAT ? RoadLineTile::Buildable : RoadLineTile::Sloped;
		}

		case TileType::Road:
			/* A road across the line can get the missing pieces; depots and crossings cannot. */
			return IsNormalRoadTile(tile) ? RoadLineTile::Buildable : RoadLineTile::Blocked;

		case TileType::Water:
			return RoadLineTile::Bridge;

		case TileType::Railway:
			if (!IsPlainRailTile(tile)) return RoadLineTile::Blocked;
			return _settings_game.economy.allow_town_level_crossings ? RoadLineTile::Crossing : RoadLineTile::Bridge;

		default:
			return RoadLineTile::Blocked;
	}
}

/**
 * Is a tile a crossing of two road lines of the grid?
 * @param tile The tile.
 * @return True for block corners.
 */
static inline bool IsRoadLineCrossing(TileIndex tile)
{
	return TileX(tile) % TOWN_BLOCK_PITCH == 0 && TileY(tile) % TOWN_BLOCK_PITCH == 0;
}

/**
 * Get the longest bridge a town may build.
 * @param t The town.
 * @return Maximum bridge length in tiles, heads included.
 */
static uint GetTownMaxBridgeLength(const Town *t)
{
	return std::min<uint>(TOWN_BRIDGE_BASE_LENGTH + t->cache.population / ScaleHousePopulation(1000), TOWN_BRIDGE_MAX_LENGTH);
}

/** A bridge the town plans to build on a road line. */
struct TownBridgePlan {
	TileIndex start; ///< First head.
	TileIndex end; ///< Last head.
};

/**
 * Plan the bridge over a stretch of water or rail on a road line.
 * @param t The town.
 * @param tile A tile of the stretch.
 * @param dir Direction of the road line, from the start of the side to its end.
 * @return The bridge heads, or std::nullopt if the town cannot bridge the stretch.
 */
static std::optional<TownBridgePlan> PlanTownBridge(const Town *t, TileIndex tile, DiagDirection dir)
{
	Axis axis = DiagDirToAxis(dir);
	TileIndexDiff delta = TileOffsByDiagDir(dir);
	uint max_length = GetTownMaxBridgeLength(t);

	/* Walk back and forth to the land at both ends of the stretch. */
	TileIndex start = tile;
	uint length = 1;
	do {
		start -= delta;
		if (!IsValidTile(start) || ++length > max_length) return std::nullopt;
	} while (ClassifyRoadLineTile(start, axis) == RoadLineTile::Bridge);

	TileIndex end = tile;
	do {
		end += delta;
		if (!IsValidTile(end) || ++length > max_length) return std::nullopt;
	} while (ClassifyRoadLineTile(end, axis) == RoadLineTile::Bridge);

	/* The heads need free land, and must not take the place of a crossing of road lines. */
	for (TileIndex head : {start, end}) {
		RoadLineTile kind = ClassifyRoadLineTile(head, axis);
		if (kind != RoadLineTile::Buildable && kind != RoadLineTile::Sloped) return std::nullopt;
		if (IsTileType(head, TileType::Road) || IsRoadLineCrossing(head)) return std::nullopt;
	}
	return TownBridgePlan{start, end};
}

/**
 * Build a planned bridge.
 * @param plan The bridge heads.
 * @param test Only test whether the bridge can be built.
 * @return True if the bridge was (or can be) built.
 */
static bool BuildTownBridge(const TownBridgePlan &plan, bool test)
{
	RoadType rt = GetTownRoadType();
	DoCommandFlags flags = CommandFlagsToDCFlags(GetCommandFlags<Commands::BuildBridge>());
	uint first_type = RandomRange(MAX_BRIDGES);
	for (uint i = 0; i < MAX_BRIDGES; i++) {
		BridgeType type = static_cast<BridgeType>((first_type + i) % MAX_BRIDGES);
		if (Command<Commands::BuildBridge>::Do(flags, plan.end, plan.start, TransportType::Road, type, INVALID_RAILTYPE, rt).Failed()) continue;
		if (test) return true;
		return Command<Commands::BuildBridge>::Do(DoCommandFlags{flags}.Set(DoCommandFlag::Execute), plan.end, plan.start, TransportType::Road, type, INVALID_RAILTYPE, rt).Succeeded();
	}
	return false;
}

/**
 * Build the missing road of one side of a block: five tiles from one corner to the other.
 * Either the whole side can be completed, or nothing is built, so the town leaves no dead ends.
 * @param t The town.
 * @param start The first corner of the side.
 * @param dir The direction from the first corner to the second one.
 * @return True if anything was built.
 */
static bool BuildBlockSide(Town *t, TileIndex start, DiagDirection dir)
{
	static constexpr uint SIDE_TILES = TOWN_BLOCK_PITCH + 1;
	Axis axis = DiagDirToAxis(dir);
	TileIndexDiff delta = TileOffsByDiagDir(dir);
	RoadType rt = GetTownRoadType();

	/* What to build on each tile of the side, if anything. */
	std::array<RoadBits, SIDE_TILES> pieces{};
	std::vector<TownBridgePlan> bridges;
	bool needed = false;

	for (uint i = 0; i < SIDE_TILES; i++) {
		TileIndex tile = start + delta * static_cast<int>(i);
		switch (ClassifyRoadLineTile(tile, axis)) {
			case RoadLineTile::Done:
				break;

			case RoadLineTile::Buildable:
			case RoadLineTile::Sloped:
			case RoadLineTile::Crossing: {
				RoadBits want{};
				if (i > 0) want.Set(DiagDirToRoadBits(ReverseDiagDir(dir)));
				if (i + 1 < SIDE_TILES) want.Set(DiagDirToRoadBits(dir));
				pieces[i] = want.Reset(GetAnyRoadBits(tile, RoadTramType::Road, true));
				needed |= pieces[i].Any();
				break;
			}

			case RoadLineTile::Bridge: {
				std::optional<TownBridgePlan> plan = PlanTownBridge(t, tile, dir);
				if (!plan.has_value()) return false;
				bridges.push_back(*plan);
				needed = true;
				/* Skip the stretch; the tiles up to the far head belong to the bridge. */
				while (i + 1 < SIDE_TILES && tile != plan->end) {
					tile += delta;
					i++;
					pieces[i] = {};
				}
				/* A bridge head inside the side needs no road piece. */
				if (tile == plan->end) pieces[i] = {};
				break;
			}

			case RoadLineTile::Blocked:
				return false;
		}
	}
	if (!needed) return false;

	/* The near head of a bridge may lie on a tile before it that was planned as a road piece. */
	for (const TownBridgePlan &bridge : bridges) {
		for (uint i = 0; i < SIDE_TILES; i++) {
			if (start + delta * static_cast<int>(i) == bridge.start) pieces[i] = {};
		}
	}

	/* Test everything before building anything. */
	for (uint i = 0; i < SIDE_TILES; i++) {
		if (pieces[i].None()) continue;
		TileIndex tile = start + delta * static_cast<int>(i);
		if (Command<Commands::BuildRoad>::Do({DoCommandFlag::Auto, DoCommandFlag::NoWater}, tile, pieces[i], rt, {}, t->index).Failed()) return false;
	}
	for (const TownBridgePlan &bridge : bridges) {
		if (!BuildTownBridge(bridge, true)) return false;
	}

	bool built = false;
	for (uint i = 0; i < SIDE_TILES; i++) {
		if (pieces[i].None()) continue;
		TileIndex tile = start + delta * static_cast<int>(i);
		if (!IsTileType(tile, TileType::Road) && (!_settings_game.construction.build_on_slopes || Chance16(1, 6))) LevelTownLand(tile);
		built |= Command<Commands::BuildRoad>::Do({DoCommandFlag::Execute, DoCommandFlag::Auto, DoCommandFlag::NoWater}, tile, pieces[i], rt, {}, t->index).Succeeded();
	}
	for (const TownBridgePlan &bridge : bridges) {
		built |= BuildTownBridge(bridge, false);
	}
	return built;
}

/** Sides of a block: corner offset from the block origin and the direction along the side. */
static const std::array<std::tuple<uint, uint, DiagDirection>, 4> _block_sides = {{
	{0, 0, DiagDirection::SW},
	{0, TOWN_BLOCK_PITCH, DiagDirection::SW},
	{0, 0, DiagDirection::SE},
	{TOWN_BLOCK_PITCH, 0, DiagDirection::SE},
}};

/**
 * Build the missing perimeter roads of a block.
 * @param t The town.
 * @param key The block.
 * @return True if anything was built.
 */
static bool BuildBlockRoads(Town *t, TownBlockKey key)
{
	TileIndex origin = GetBlockOrigin(key);
	bool built = false;
	for (const auto &[x, y, dir] : _block_sides) {
		built |= BuildBlockSide(t, origin + TileDiffXY(x, y), dir);
	}
	return built;
}

/**
 * Is a block connected to the road network: does at least one of its sides have a complete road?
 * @param key The block.
 * @return True if at least one side has road on its three middle tiles.
 */
static bool IsBlockConnected(TownBlockKey key)
{
	TileIndex origin = GetBlockOrigin(key);
	for (const auto &[x, y, dir] : _block_sides) {
		TileIndex tile = origin + TileDiffXY(x, y);
		TileIndexDiff delta = TileOffsByDiagDir(dir);
		Axis axis = DiagDirToAxis(dir);
		bool complete = true;
		for (uint i = 1; i < TOWN_BLOCK_PITCH && complete; i++) {
			complete = HasRoadAlong(tile + delta * static_cast<int>(i), axis);
		}
		if (complete) return true;
	}
	return false;
}

/**
 * Evaluate a block that the town might open: its plan cost including the terrain.
 * @param t The town.
 * @param key The block.
 * @return The cost, or std::nullopt if the block cannot be used.
 */
static std::optional<uint32_t> EvaluateBlock(const Town *t, TownBlockKey key)
{
	if (!IsValidBlock(GetBlockX(key), GetBlockY(key))) return std::nullopt;

	TileIndex origin = GetBlockOrigin(key);
	uint32_t cost = GetBlockPlanCost(t, key);

	uint unusable = 0;
	for (uint y = 1; y < TOWN_BLOCK_PITCH; y++) {
		for (uint x = 1; x < TOWN_BLOCK_PITCH; x++) {
			TileIndex tile = origin + TileDiffXY(x, y);
			if (IsFreeHousePlot(tile)) {
				if (!IsTileFlat(tile)) cost += PENALTY_PLOT_SLOPED;
				continue;
			}
			unusable++;
			switch (GetTileType(tile)) {
				case TileType::Clear:
				case TileType::Trees:
				case TileType::Water:
					cost += PENALTY_PLOT_UNUSABLE;
					break;

				case TileType::House:
					/* Houses of this town are not in a block that is still to be opened. */
					cost += GetTownIndex(tile) == t->index ? PENALTY_PLOT_UNUSABLE : PENALTY_PLOT_FOREIGN;
					break;

				default:
					cost += PENALTY_PLOT_FOREIGN;
					break;
			}
		}
	}
	if (unusable > BLOCK_MAX_UNUSABLE_PLOTS) return std::nullopt;

	for (const auto &[x, y, dir] : _block_sides) {
		TileIndex tile = origin + TileDiffXY(x, y);
		TileIndexDiff delta = TileOffsByDiagDir(dir);
		Axis axis = DiagDirToAxis(dir);
		for (uint i = 0; i <= TOWN_BLOCK_PITCH; i++) {
			switch (ClassifyRoadLineTile(tile + delta * static_cast<int>(i), axis)) {
				case RoadLineTile::Done: break;
				case RoadLineTile::Buildable: break;
				case RoadLineTile::Sloped: cost += PENALTY_ROAD_SLOPED; break;
				case RoadLineTile::Crossing: cost += PENALTY_ROAD_CROSSING; break;
				case RoadLineTile::Bridge: cost += PENALTY_ROAD_BRIDGE; break;
				case RoadLineTile::Blocked: cost += PENALTY_ROAD_BLOCKED; break;
			}
		}
	}
	return cost;
}

/**
 * Builder step 3: open the cheapest block next to the town, by building its missing perimeter roads and a first house.
 * @param t The town.
 * @param cache The blocks of the town.
 * @param modes The parts of the town that may be grown.
 * @param[out] result What the step built.
 * @return True if anything was built.
 */
static bool OpenNewBlock(Town *t, const TownBlockCache &cache, TownExpandModes modes, TownBuildResult &result)
{
	std::vector<std::pair<uint32_t, TownBlockKey>> candidates;
	auto consider = [t, &candidates](TownBlockKey key) {
		std::optional<uint32_t> cost = EvaluateBlock(t, key);
		if (cost.has_value()) candidates.emplace_back(*cost, key);
	};

	if (cache.frontier.empty()) {
		/* A town without blocks starts with a block that touches its centre. */
		uint x = TileX(t->xy);
		uint y = TileY(t->xy);
		for (uint by = (y - 1) / TOWN_BLOCK_PITCH; by <= y / TOWN_BLOCK_PITCH; by++) {
			for (uint bx = (x - 1) / TOWN_BLOCK_PITCH; bx <= x / TOWN_BLOCK_PITCH; bx++) {
				if (!cache.house_tiles.contains(MakeBlockKey(bx, by))) consider(MakeBlockKey(bx, by));
			}
		}
	} else {
		for (const auto &[key, neighbours] : cache.frontier) consider(key);
	}
	std::sort(candidates.begin(), candidates.end());

	for (const auto &[cost, key] : candidates) {
		bool built = modes.Test(TownExpandMode::Roads) && BuildBlockRoads(t, key);

		if (modes.Test(TownExpandMode::Buildings) && IsBlockConnected(key)) {
			HouseID house = BuildHouseInBlock(t, key, modes);
			if (house != INVALID_HOUSE_ID) {
				result.success = true;
				result.residents = ScaleHousePopulation(HouseSpec::Get(house)->population);
				return true;
			}
		}

		if (built) {
			result.success = true;
			return true;
		}
	}
	return false;
}

/**
 * Do one step of the town builder: fill a free plot, else densify, else open a new block.
 * @param t The town to grow.
 * @param modes The parts of the town that may be grown.
 * @return What the step built.
 */
TownBuildResult TownBuilderStep(Town *t, TownExpandModes modes)
{
	AutoRestoreBackup cur_company(_current_company, OWNER_TOWN);

	/* Keep the cache alive for the whole step, even if a hook drops it from the town. */
	std::shared_ptr<TownBlockCache> cache = GetBlockCache(t);
	TownBuildResult result;

	if (modes.Test(TownExpandMode::Buildings)) {
		if (FillOpenedBlocks(t, *cache, modes, result)) return result;
		if (DensifyOpenedBlocks(t, *cache, result)) return result;
	}
	OpenNewBlock(t, *cache, modes, result);
	return result;
}
