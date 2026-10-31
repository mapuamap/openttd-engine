/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file blueprint_cmd.cpp The command that builds one chunk of a rail blueprint.
 *
 * A chunk is a list of items, see #BlueprintItemKind. Every item is built with the regular command that
 * a player would use (rail, signals, depot, bridge, tunnel, station, waypoint), so every rule of those
 * commands applies. An item that cannot be built is counted and skipped; the rest is still built.
 */

#include "stdafx.h"
#include "blueprint_cmd.h"
#include "command_func.h"
#include "company_func.h"
#include "map_func.h"
#include "newgrf_station.h"
#include "rail_cmd.h"
#include "settings_type.h"
#include "signal_func.h"
#include "station_cmd.h"
#include "station_map.h"
#include "tunnelbridge_cmd.h"
#include "waypoint_cmd.h"

#include "table/strings.h"

#include "safeguards.h"

/** Reader of the items of a chunk; reading past the end sets #ok to false. */
struct BlueprintChunkReader {
	std::span<const uint8_t> in; ///< The chunk data.
	size_t pos = 0; ///< Read position.
	bool ok = true; ///< No read went past the end.

	bool AtEnd() const { return this->pos >= this->in.size(); }
	uint8_t U8()
	{
		if (this->pos >= this->in.size()) {
			this->ok = false;
			return 0;
		}
		return this->in[this->pos++];
	}
	uint16_t U16() { uint16_t lo = this->U8(); return lo | static_cast<uint16_t>(this->U8()) << 8; }
};

/**
 * Build one chunk of a rail blueprint.
 * @param flags Operation to perform.
 * @param origin Tile the item offsets are relative to.
 * @param chunk The items.
 * @return The cost (or the error when nothing was built), the money missing to build the next item,
 *         the number of items built, the number of items that failed, and the error of the last failed item.
 */
std::tuple<CommandCost, Money, uint32_t, uint32_t, StringID> CmdPasteBlueprint(DoCommandFlags flags, TileIndex origin, const BlueprintChunkData &chunk)
{
	if (origin >= Map::Size() || chunk.data.empty() || chunk.data.size() > BlueprintChunkData::MAX_SIZE) return {CMD_ERROR, 0, 0, 0, INVALID_STRING_ID};

	const bool exec = flags.Test(DoCommandFlag::Execute);
	const DoCommandFlags test_flags = DoCommandFlags{flags}.Reset(DoCommandFlag::Execute);
	Money money = GetAvailableMoneyForCommand();
	CommandCost total(ExpensesType::Construction);
	uint32_t built = 0;
	uint32_t failed = 0;
	StringID last_error = INVALID_STRING_ID;
	Money needed = 0;

	/* Station built for every station group of this chunk. */
	std::array<StationID, UINT8_MAX + 1> group_station;
	group_station.fill(StationID::Invalid());

	auto tile_at = [&](uint16_t dx, uint16_t dy) -> TileIndex {
		uint x = TileX(origin) + dx;
		uint y = TileY(origin) + dy;
		if (x >= Map::MaxX() || y >= Map::MaxY()) return INVALID_TILE;
		return TileXY(x, y);
	};

	/* Build one item: test, check the money, execute. Returns whether the item was built. */
	auto attempt = [&](auto &&build) -> bool {
		CommandCost ret = build(test_flags);
		if (ret.Failed()) {
			if (ret.GetErrorMessage() != STR_ERROR_ALREADY_BUILT) {
				failed++;
				last_error = ret.GetErrorMessage();
			}
			return false;
		}
		if (exec) {
			if (ret.GetCost() > 0 && money < ret.GetCost()) {
				needed = ret.GetCost();
				return false;
			}
			ret = build(flags);
			if (ret.Failed()) {
				failed++;
				last_error = ret.GetErrorMessage();
				return false;
			}
			money -= ret.GetCost();
		}
		total.AddCost(ret.GetCost());
		built++;
		return true;
	};
	auto item_off_map = [&]() {
		failed++;
		last_error = STR_ERROR_TOO_CLOSE_TO_EDGE_OF_MAP;
	};

	BlueprintChunkReader r{chunk.data};
	while (!r.AtEnd() && needed == 0) {
		uint8_t kind = r.U8();
		switch (static_cast<BlueprintItemKind>(kind)) {
			case BlueprintItemKind::Track: {
				uint16_t dx = r.U16(), dy = r.U16();
				RailType rt = static_cast<RailType>(r.U8());
				TrackBits bits{r.U8()};
				if (!r.ok || bits.None() || !TRACK_BIT_ALL.All(bits)) return {CMD_ERROR, 0, 0, 0, INVALID_STRING_ID};
				TileIndex tile = tile_at(dx, dy);
				if (tile == INVALID_TILE) {
					item_off_map();
					break;
				}
				for (Track track : EnumRange(Track::End)) {
					if (needed != 0) break;
					if (!bits.Test(track)) continue;
					attempt([&](DoCommandFlags f) { return Command<Commands::BuildRail>::Do(f, tile, rt, track, false); });
				}
				break;
			}

			case BlueprintItemKind::Signal: {
				uint16_t dx = r.U16(), dy = r.U16();
				uint8_t track_raw = r.U8(), type_raw = r.U8(), variant_raw = r.U8(), present = r.U8();
				if (!r.ok || track_raw >= to_underlying(Track::End) || type_raw >= to_underlying(SignalType::End) || variant_raw >= to_underlying(SignalVariant::End)) return {CMD_ERROR, 0, 0, 0, INVALID_STRING_ID};
				Track track = static_cast<Track>(track_raw);
				if (present == 0 || (present & ~SignalOnTrack(track)) != 0) return {CMD_ERROR, 0, 0, 0, INVALID_STRING_ID};
				TileIndex tile = tile_at(dx, dy);
				if (tile == INVALID_TILE) {
					item_off_map();
					break;
				}
				SignalType type = static_cast<SignalType>(type_raw);
				SignalVariant variant = static_cast<SignalVariant>(variant_raw);
				attempt([&](DoCommandFlags f) {
					return Command<Commands::BuildSignal>::Do(f, tile, track, type, variant, false, false, false, SignalType::Block, SignalType::PathOneWay, 0, present);
				});
				break;
			}

			case BlueprintItemKind::Depot: {
				uint16_t dx = r.U16(), dy = r.U16();
				RailType rt = static_cast<RailType>(r.U8());
				uint8_t dir_raw = r.U8();
				if (!r.ok || dir_raw >= to_underlying(DiagDirection::End)) return {CMD_ERROR, 0, 0, 0, INVALID_STRING_ID};
				TileIndex tile = tile_at(dx, dy);
				if (tile == INVALID_TILE) {
					item_off_map();
					break;
				}
				DiagDirection dir = static_cast<DiagDirection>(dir_raw);
				attempt([&](DoCommandFlags f) { return Command<Commands::BuildRailDepot>::Do(f, tile, rt, dir); });
				break;
			}

			case BlueprintItemKind::Bridge:
			case BlueprintItemKind::Tunnel: {
				bool bridge = kind == to_underlying(BlueprintItemKind::Bridge);
				uint16_t dx = r.U16(), dy = r.U16(), ex = r.U16(), ey = r.U16();
				RailType rt = static_cast<RailType>(r.U8());
				BridgeType bridge_type = bridge ? r.U8() : 0;
				if (!r.ok) return {CMD_ERROR, 0, 0, 0, INVALID_STRING_ID};
				TileIndex start = tile_at(dx, dy);
				TileIndex end = tile_at(ex, ey);
				if (start == INVALID_TILE || end == INVALID_TILE) {
					item_off_map();
					break;
				}
				if (bridge) {
					attempt([&](DoCommandFlags f) { return Command<Commands::BuildBridge>::Do(f, end, start, TransportType::Rail, bridge_type, rt, INVALID_ROADTYPE); });
				} else {
					/* The tunnel follows the slope of the tile it starts at; either end will do when the terrain matches. */
					TileIndex from = Command<Commands::BuildTunnel>::Do(test_flags, start, TransportType::Rail, rt, INVALID_ROADTYPE).Succeeded() ? start : end;
					attempt([&](DoCommandFlags f) { return Command<Commands::BuildTunnel>::Do(f, from, TransportType::Rail, rt, INVALID_ROADTYPE); });
				}
				break;
			}

			case BlueprintItemKind::Station:
			case BlueprintItemKind::Waypoint: {
				bool waypoint = kind == to_underlying(BlueprintItemKind::Waypoint);
				uint16_t dx = r.U16(), dy = r.U16();
				uint8_t w = r.U8(), h = r.U8(), axis_raw = r.U8();
				RailType rt = static_cast<RailType>(r.U8());
				StationClassID cls{r.U16()};
				uint16_t index = r.U16();
				uint8_t group = r.U8(), item_flags = r.U8();
				if (!r.ok || w == 0 || h == 0 || axis_raw >= to_underlying(Axis::End)) return {CMD_ERROR, 0, 0, 0, INVALID_STRING_ID};
				TileIndex tile = tile_at(dx, dy);
				if (tile == INVALID_TILE || tile_at(dx + w - 1, dy + h - 1) == INVALID_TILE) {
					item_off_map();
					break;
				}
				Axis axis = static_cast<Axis>(axis_raw);

				/* The first part of a station is a new station (or joins its neighbour when it continues a
				 * station of an earlier chunk); the other parts join it. */
				StationID join = group_station[group];
				bool adjacent = true;
				if (join == StationID::Invalid()) {
					join = NEW_STATION;
					if (HasBit(item_flags, to_underlying(BlueprintStationFlag::JoinNeighbour))) adjacent = false;
				} else if (!_settings_game.station.distant_join_stations) {
					join = NEW_STATION;
					adjacent = false;
				}

				bool ok;
				if (waypoint) {
					ok = attempt([&](DoCommandFlags f) { return Command<Commands::BuildRailWaypoint>::Do(f, tile, axis, w, h, cls, index, join, adjacent); });
				} else {
					uint8_t numtracks = axis == Axis::X ? h : w;
					uint8_t plat_len = axis == Axis::X ? w : h;
					ok = attempt([&](DoCommandFlags f) { return Command<Commands::BuildRailStation>::Do(f, tile, rt, axis, numtracks, plat_len, cls, index, join, adjacent); });
				}
				if (ok && exec && group_station[group] == StationID::Invalid() && IsTileType(tile, TileType::Station)) group_station[group] = GetStationIndex(tile);
				break;
			}

			default:
				return {CMD_ERROR, 0, 0, 0, INVALID_STRING_ID};
		}
	}

	if (needed != 0) return {total, needed, built, failed, last_error};
	if (exec && built == 0 && failed != 0) return {CommandCost(last_error), 0, built, failed, last_error};
	return {total, 0, built, failed, last_error};
}
