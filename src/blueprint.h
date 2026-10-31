/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file blueprint.h Rail blueprints: copy an area of rail infrastructure, store it, and paste it elsewhere, rotated or mirrored.
 *
 * A blueprint holds the rail infrastructure of a rectangular area in coordinates relative to its north
 * corner: plain track (also the rail of level crossings), signals, train depots, rail bridges and tunnels
 * whose both ends are inside the area, and rail station and waypoint tiles. Terrain, roads and other
 * structures are not part of a blueprint.
 *
 * Blueprints live on the client, as files in the "blueprint" directory of the personal directory. Pasting
 * resolves rail types and station specs in the current game and turns the blueprint into a list of
 * chunks; each chunk is one #Commands::PasteBlueprint command, small enough for a network packet.
 * A paste job sends the chunks one after the other (a few in flight at a time) and adds up the results.
 */

#ifndef BLUEPRINT_H
#define BLUEPRINT_H

#include "core/label_type.hpp"
#include "bridge_type.h"
#include "company_type.h"
#include "direction_type.h"
#include "rail_type.h"
#include "signal_type.h"
#include "tile_type.h"
#include "track_type.h"
#include "economy_type.h"
#include "newgrf_type.h"
#include "strings_type.h"

/** Rotation and mirroring of a blueprint. The mirror (along the x axis) is applied first, then the rotation. */
struct BlueprintTransform {
	uint8_t rotation = 0; ///< Number of quarter turns clockwise (as seen on the screen), 0..3.
	bool mirror = false; ///< Mirror the blueprint: the x coordinate is flipped (north-east <-> south-west).

	bool operator==(const BlueprintTransform &other) const = default;
};

/** Plain track on one tile. */
struct BlueprintTrack {
	uint16_t x = 0; ///< X offset from the north corner of the blueprint.
	uint16_t y = 0; ///< Y offset from the north corner of the blueprint.
	uint8_t railtype = 0; ///< Index into Blueprint::railtypes.
	TrackBits tracks{}; ///< The track pieces.

	bool operator==(const BlueprintTrack &other) const = default;
};

/** Signals on one track of a tile. */
struct BlueprintSignal {
	uint16_t x = 0; ///< X offset from the north corner of the blueprint.
	uint16_t y = 0; ///< Y offset from the north corner of the blueprint.
	Track track = Track::Invalid; ///< The track with the signals.
	SignalType type = SignalType::Block; ///< Type of the signals.
	SignalVariant variant = SignalVariant::Electric; ///< Variant of the signals.
	uint8_t trackdirs = 0; ///< Bit 0: signal for trains along TrackToTrackdir(track); bit 1: for trains in the reverse direction.

	bool operator==(const BlueprintSignal &other) const = default;
};

/** A train depot. */
struct BlueprintDepot {
	uint16_t x = 0; ///< X offset from the north corner of the blueprint.
	uint16_t y = 0; ///< Y offset from the north corner of the blueprint.
	uint8_t railtype = 0; ///< Index into Blueprint::railtypes.
	DiagDirection dir = DiagDirection::NE; ///< Direction of the exit.

	bool operator==(const BlueprintDepot &other) const = default;
};

/** A rail bridge or tunnel; both ends are inside the blueprint. */
struct BlueprintTunnelBridge {
	uint16_t x = 0; ///< X offset of the first end.
	uint16_t y = 0; ///< Y offset of the first end.
	uint16_t end_x = 0; ///< X offset of the other end.
	uint16_t end_y = 0; ///< Y offset of the other end.
	uint8_t railtype = 0; ///< Index into Blueprint::railtypes.
	bool bridge = false; ///< True for a bridge, false for a tunnel.
	BridgeType bridge_type = 0; ///< Type of the bridge.

	bool operator==(const BlueprintTunnelBridge &other) const = default;
};

/** One tile of a rail station or waypoint. */
struct BlueprintStationTile {
	uint16_t x = 0; ///< X offset from the north corner of the blueprint.
	uint16_t y = 0; ///< Y offset from the north corner of the blueprint.
	uint16_t group = 0; ///< Tiles with the same group belong to the same station.
	uint8_t railtype = 0; ///< Index into Blueprint::railtypes.
	Axis axis = Axis::X; ///< Direction of the track.
	bool waypoint = false; ///< True for a waypoint, false for a station.
	uint16_t spec = UINT16_MAX; ///< Index into Blueprint::specs, or UINT16_MAX for the default station or waypoint.

	bool operator==(const BlueprintStationTile &other) const = default;
};

/** A NewGRF station or waypoint spec, identified independently of the game. */
struct BlueprintStationSpec {
	GrfID grfid{}; ///< GRF ID of the NewGRF that defines the spec.
	uint16_t local_id = 0; ///< ID of the spec within the NewGRF.

	bool operator==(const BlueprintStationSpec &other) const = default;
};

/** The rail infrastructure of an area. */
struct Blueprint {
	static constexpr uint MAX_SIZE = 4096; ///< Maximum width and height of a blueprint.

	std::string name{}; ///< Name of the blueprint (also its file name).
	uint16_t width = 0; ///< Width (x extent) in tiles.
	uint16_t height = 0; ///< Height (y extent) in tiles.
	std::vector<RailTypeLabel> railtypes{}; ///< Labels of the rail types used.
	std::vector<BlueprintStationSpec> specs{}; ///< NewGRF station specs used.
	std::vector<BlueprintTrack> tracks{}; ///< Plain track.
	std::vector<BlueprintSignal> signals{}; ///< Signals.
	std::vector<BlueprintDepot> depots{}; ///< Train depots.
	std::vector<BlueprintTunnelBridge> tunnelbridges{}; ///< Bridges and tunnels.
	std::vector<BlueprintStationTile> stations{}; ///< Station and waypoint tiles.

	size_t CountItems() const;
	bool IsEmpty() const { return this->CountItems() == 0; }
	Blueprint Transformed(BlueprintTransform transform) const;
	void Normalise();
	std::string Compare(const Blueprint &other) const;
};

/** What #CaptureBlueprint left out. */
struct BlueprintCaptureStats {
	uint partial_tunnelbridges = 0; ///< Bridges and tunnels with only one end in the area.
};

/* Transformations of map directions and positions. */
DiagDirection TransformDiagDir(DiagDirection dir, BlueprintTransform transform);
Axis TransformAxis(Axis axis, BlueprintTransform transform);
Track TransformTrack(Track track, BlueprintTransform transform);
TrackBits TransformTrackBits(TrackBits bits, BlueprintTransform transform);
Trackdir TransformTrackdir(Trackdir trackdir, BlueprintTransform transform);
std::pair<uint, uint> TransformPosition(uint x, uint y, uint width, uint height, BlueprintTransform transform);
std::pair<uint, uint> TransformSize(uint width, uint height, BlueprintTransform transform);

Blueprint CaptureBlueprint(TileIndex corner1, TileIndex corner2, std::string_view name, BlueprintCaptureStats *stats = nullptr);

/* Files. */
std::string GetBlueprintDirectory();
std::string GetBlueprintFileName(std::string_view name);
bool SaveBlueprint(const Blueprint &bp);
std::optional<Blueprint> LoadBlueprint(std::string_view name);
bool DeleteBlueprint(std::string_view name);
bool RenameBlueprint(std::string_view name, std::string_view new_name);
std::vector<std::string> ListBlueprints();
std::vector<uint8_t> SerialiseBlueprint(const Blueprint &bp);
std::optional<Blueprint> DeserialiseBlueprint(std::span<const uint8_t> data);

/** Payload of one #Commands::PasteBlueprint command: items relative to the origin tile of the command. */
struct BlueprintChunkData {
	static constexpr size_t MAX_SIZE = 1200; ///< Maximum number of bytes, so the command fits in a network packet.
	std::vector<uint8_t> data{}; ///< Encoded items, see blueprint_cmd.cpp.
};

/** Item kinds in a #BlueprintChunkData. */
enum class BlueprintItemKind : uint8_t {
	Track = 0, ///< x, y (u16), railtype, trackbits (u8).
	Signal = 1, ///< x, y (u16), track, signal type, variant, present signal bits of the tile (u8).
	Depot = 2, ///< x, y (u16), railtype, direction (u8).
	Bridge = 3, ///< x, y, end x, end y (u16), railtype, bridge type (u8).
	Tunnel = 4, ///< x, y, end x, end y (u16), railtype (u8).
	Station = 5, ///< x, y (u16), width, height, axis, railtype (u8), class, spec index (u16), group, flags (u8).
	Waypoint = 6, ///< Same as Station.
	End, ///< End marker.
};

/** Flags of a station item. */
enum class BlueprintStationFlag : uint8_t {
	JoinNeighbour = 0, ///< The group was started in an earlier chunk: join the adjacent station of the company.
};

/** Options of a paste. */
struct BlueprintPasteOptions {
	BlueprintTransform transform{}; ///< Rotation and mirror.
	RailType railtype = INVALID_RAILTYPE; ///< Build everything with this rail type, or INVALID_RAILTYPE to keep the rail types of the blueprint.
};

/** Result of turning a blueprint into commands. */
struct BlueprintPastePlan {
	TileIndex origin = INVALID_TILE; ///< North corner of the pasted (transformed) blueprint.
	std::vector<BlueprintChunkData> chunks{}; ///< Commands to send, in order.
	size_t items = 0; ///< Number of items (track pieces count as one per tile).
	std::string error{}; ///< Why nothing can be pasted, when chunks is empty.
};

BlueprintPastePlan PlanBlueprintPaste(const Blueprint &bp, TileIndex origin, const BlueprintPasteOptions &options);

/** State of the paste job. */
struct BlueprintPasteStatus {
	bool active = false; ///< A paste is running.
	std::string name{}; ///< Name of the blueprint being pasted.
	size_t chunks_total = 0; ///< Number of commands of the paste.
	size_t chunks_sent = 0; ///< Commands sent.
	size_t chunks_done = 0; ///< Commands whose result came back.
	uint32_t built = 0; ///< Items built.
	uint32_t failed = 0; ///< Items that could not be built.
	Money cost = 0; ///< Money spent.
	bool out_of_money = false; ///< The paste stopped because the company ran out of money.
	bool aborted = false; ///< The paste was aborted.
	StringID last_error = INVALID_STRING_ID; ///< Last error of an item, if any.
};

bool StartBlueprintPaste(std::string_view name, BlueprintPastePlan &&plan, CompanyID company);
void AbortBlueprintPaste();
const BlueprintPasteStatus &GetBlueprintPasteStatus();
void RunBlueprintPasteJob();

#endif /* BLUEPRINT_H */
