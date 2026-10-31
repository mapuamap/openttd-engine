/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file blueprint.cpp Rail blueprints: capture, transformation, files and the paste job. */

#include "stdafx.h"
#include <deque>
#include <filesystem>
#include <map>
#include <set>
#include "blueprint.h"
#include "blueprint_cmd.h"
#include "bridge_map.h"
#include "command_func.h"
#include "company_base.h"
#include "company_func.h"
#include "console_func.h"
#include "core/backup_type.hpp"
#include "fileio_func.h"
#include "newgrf_station.h"
#include "rail.h"
#include "rail_map.h"
#include "road_map.h"
#include "signal_func.h"
#include "station_map.h"
#include "string_func.h"
#include "strings_func.h"
#include "tunnelbridge_map.h"
#include "window_func.h"
#include "network/network.h"
#include "timer/timer.h"
#include "timer/timer_game_realtime.h"

#include "table/strings.h"

#include "safeguards.h"

/* --- Transformations --- */

/**
 * Transform a diagonal direction: mirror (north-east <-> south-west), then rotate clockwise.
 * @param dir The direction.
 * @param transform The transformation.
 * @return The transformed direction.
 */
DiagDirection TransformDiagDir(DiagDirection dir, BlueprintTransform transform)
{
	uint d = to_underlying(dir);
	if (transform.mirror && (dir == DiagDirection::NE || dir == DiagDirection::SW)) d ^= 2;
	return static_cast<DiagDirection>((d + transform.rotation) % 4);
}

/**
 * Transform an axis.
 * @param axis The axis.
 * @param transform The transformation.
 * @return The transformed axis.
 */
Axis TransformAxis(Axis axis, BlueprintTransform transform)
{
	return (transform.rotation % 2 == 0) ? axis : OtherAxis(axis);
}

/** The two tile edges every track connects. */
static constexpr std::array<std::pair<DiagDirection, DiagDirection>, to_underlying(Track::End)> _track_edges = {{
	{DiagDirection::NE, DiagDirection::SW}, // X
	{DiagDirection::SE, DiagDirection::NW}, // Y
	{DiagDirection::NW, DiagDirection::NE}, // Upper (north corner)
	{DiagDirection::SE, DiagDirection::SW}, // Lower (south corner)
	{DiagDirection::SW, DiagDirection::NW}, // Left (west corner)
	{DiagDirection::NE, DiagDirection::SE}, // Right (east corner)
}};

/**
 * Transform a track.
 * @param track The track.
 * @param transform The transformation.
 * @return The transformed track.
 */
Track TransformTrack(Track track, BlueprintTransform transform)
{
	auto [a, b] = _track_edges[to_underlying(track)];
	a = TransformDiagDir(a, transform);
	b = TransformDiagDir(b, transform);
	for (Track t : EnumRange(Track::End)) {
		auto [c, d] = _track_edges[to_underlying(t)];
		if ((c == a && d == b) || (c == b && d == a)) return t;
	}
	NOT_REACHED();
}

/**
 * Transform track bits.
 * @param bits The track bits.
 * @param transform The transformation.
 * @return The transformed track bits.
 */
TrackBits TransformTrackBits(TrackBits bits, BlueprintTransform transform)
{
	TrackBits result{};
	for (Track t : EnumRange(Track::End)) {
		if (bits.Test(t)) result.Set(TransformTrack(t, transform));
	}
	return result;
}

/**
 * Transform a track direction.
 * @param trackdir The track direction.
 * @param transform The transformation.
 * @return The transformed track direction.
 */
Trackdir TransformTrackdir(Trackdir trackdir, BlueprintTransform transform)
{
	Track track = TransformTrack(TrackdirToTrack(trackdir), transform);
	return TrackExitdirToTrackdir(track, TransformDiagDir(TrackdirToExitdir(trackdir), transform));
}

/**
 * Get the size of a blueprint after a transformation.
 * @param width Width before the transformation.
 * @param height Height before the transformation.
 * @param transform The transformation.
 * @return Width and height after the transformation.
 */
std::pair<uint, uint> TransformSize(uint width, uint height, BlueprintTransform transform)
{
	if (transform.rotation % 2 == 0) return {width, height};
	return {height, width};
}

/**
 * Transform a position inside a blueprint.
 * A quarter turn clockwise maps the offset (x, y) to (y, width - 1 - x).
 * @param x X offset.
 * @param y Y offset.
 * @param width Width of the blueprint before the transformation.
 * @param height Height of the blueprint before the transformation.
 * @param transform The transformation.
 * @return The transformed offsets.
 */
std::pair<uint, uint> TransformPosition(uint x, uint y, uint width, uint height, BlueprintTransform transform)
{
	if (transform.mirror) x = width - 1 - x;
	for (uint i = 0; i < transform.rotation % 4; i++) {
		uint nx = y;
		uint ny = width - 1 - x;
		x = nx;
		y = ny;
		std::swap(width, height);
	}
	return {x, y};
}

/* --- The blueprint --- */

/**
 * Count the items of a blueprint.
 * @return Number of tiles with track, signalled tracks, depots, bridges and tunnels, and station tiles.
 */
size_t Blueprint::CountItems() const
{
	return this->tracks.size() + this->signals.size() + this->depots.size() + this->tunnelbridges.size() + this->stations.size();
}

/**
 * Get a copy of the blueprint, rotated and mirrored.
 * @param transform The transformation.
 * @return The transformed blueprint, normalised.
 */
Blueprint Blueprint::Transformed(BlueprintTransform transform) const
{
	Blueprint out = *this;
	transform.rotation %= 4;
	std::tie(out.width, out.height) = TransformSize(this->width, this->height, transform);

	auto pos = [&](uint16_t &x, uint16_t &y) {
		auto [nx, ny] = TransformPosition(x, y, this->width, this->height, transform);
		x = static_cast<uint16_t>(nx);
		y = static_cast<uint16_t>(ny);
	};

	for (BlueprintTrack &t : out.tracks) {
		pos(t.x, t.y);
		t.tracks = TransformTrackBits(t.tracks, transform);
	}
	for (BlueprintSignal &s : out.signals) {
		pos(s.x, s.y);
		Trackdir along = TrackToTrackdir(s.track);
		Track track = TransformTrack(s.track, transform);
		Trackdir new_along = TrackToTrackdir(track);
		uint8_t dirs = 0;
		if (HasBit(s.trackdirs, 0)) dirs |= (TransformTrackdir(along, transform) == new_along) ? 1 : 2;
		if (HasBit(s.trackdirs, 1)) dirs |= (TransformTrackdir(ReverseTrackdir(along), transform) == new_along) ? 1 : 2;
		s.track = track;
		s.trackdirs = dirs;
	}
	for (BlueprintDepot &d : out.depots) {
		pos(d.x, d.y);
		d.dir = TransformDiagDir(d.dir, transform);
	}
	for (BlueprintTunnelBridge &tb : out.tunnelbridges) {
		pos(tb.x, tb.y);
		pos(tb.end_x, tb.end_y);
	}
	for (BlueprintStationTile &st : out.stations) {
		pos(st.x, st.y);
		st.axis = TransformAxis(st.axis, transform);
	}

	out.Normalise();
	return out;
}

/** Key to sort items by position (rows from north to south). */
template <typename T>
static auto PositionKey(const T &item) { return std::make_pair(item.y, item.x); }

/**
 * Bring the blueprint in a canonical form, so equal infrastructure gives equal blueprints:
 * rail types and specs sorted, items sorted by position, bridges and tunnels start at their northern end,
 * and station groups numbered in the order of their first tile.
 */
void Blueprint::Normalise()
{
	/* Rail types: sort the labels and remap. */
	std::vector<RailTypeLabel> labels = this->railtypes;
	std::ranges::sort(labels);
	labels.erase(std::unique(labels.begin(), labels.end()), labels.end());
	auto rt_map = [&](uint8_t index) -> uint8_t {
		if (index >= this->railtypes.size()) return index;
		return static_cast<uint8_t>(std::ranges::find(labels, this->railtypes[index]) - labels.begin());
	};
	for (auto &t : this->tracks) t.railtype = rt_map(t.railtype);
	for (auto &d : this->depots) d.railtype = rt_map(d.railtype);
	for (auto &tb : this->tunnelbridges) tb.railtype = rt_map(tb.railtype);
	for (auto &st : this->stations) st.railtype = rt_map(st.railtype);
	this->railtypes = std::move(labels);

	/* Station specs. */
	std::vector<BlueprintStationSpec> specs = this->specs;
	auto spec_less = [](const BlueprintStationSpec &a, const BlueprintStationSpec &b) { return std::make_pair(a.grfid, a.local_id) < std::make_pair(b.grfid, b.local_id); };
	std::ranges::sort(specs, spec_less);
	specs.erase(std::unique(specs.begin(), specs.end()), specs.end());
	for (auto &st : this->stations) {
		if (st.spec >= this->specs.size()) continue;
		st.spec = static_cast<uint16_t>(std::ranges::find(specs, this->specs[st.spec]) - specs.begin());
	}
	this->specs = std::move(specs);

	for (auto &tb : this->tunnelbridges) {
		if (std::make_pair(tb.end_y, tb.end_x) < std::make_pair(tb.y, tb.x)) {
			std::swap(tb.x, tb.end_x);
			std::swap(tb.y, tb.end_y);
		}
	}

	std::ranges::sort(this->tracks, {}, [](const auto &i) { return PositionKey(i); });
	std::ranges::sort(this->signals, {}, [](const auto &i) { return std::make_tuple(i.y, i.x, i.track); });
	std::ranges::sort(this->depots, {}, [](const auto &i) { return PositionKey(i); });
	std::ranges::sort(this->tunnelbridges, {}, [](const auto &i) { return PositionKey(i); });
	std::ranges::sort(this->stations, {}, [](const auto &i) { return PositionKey(i); });

	/* Number station groups by their first tile. */
	std::map<uint16_t, uint16_t> groups;
	for (auto &st : this->stations) {
		auto [it, inserted] = groups.try_emplace(st.group, static_cast<uint16_t>(groups.size()));
		st.group = it->second;
	}
}

/**
 * Compare two blueprints after normalising both.
 * @param other The other blueprint.
 * @return Empty when the infrastructure is equal, otherwise a description of the first differences.
 */
std::string Blueprint::Compare(const Blueprint &other) const
{
	Blueprint a = *this;
	Blueprint b = other;
	a.Normalise();
	b.Normalise();

	std::string diff;
	int lines = 0;
	auto note = [&](std::string_view text) {
		if (lines++ < 20) format_append(diff, "{}\n", text);
	};

	if (a.width != b.width || a.height != b.height) note(fmt::format("size {}x{} != {}x{}", a.width, a.height, b.width, b.height));
	if (a.railtypes != b.railtypes) note("rail types differ");
	if (a.specs != b.specs) note("station specs differ");

	auto compare = [&](const auto &va, const auto &vb, std::string_view what, auto describe) {
		if (va.size() != vb.size()) note(fmt::format("{}: {} != {} items", what, va.size(), vb.size()));
		size_t n = std::min(va.size(), vb.size());
		for (size_t i = 0; i < n; i++) {
			if (!(va[i] == vb[i])) note(fmt::format("{} #{}: {} != {}", what, i, describe(va[i]), describe(vb[i])));
		}
	};
	compare(a.tracks, b.tracks, "track", [](const BlueprintTrack &t) { return fmt::format("({},{}) rt{} bits{:02x}", t.x, t.y, t.railtype, t.tracks.base()); });
	compare(a.signals, b.signals, "signal", [](const BlueprintSignal &s) { return fmt::format("({},{}) track{} type{} var{} dirs{}", s.x, s.y, to_underlying(s.track), to_underlying(s.type), to_underlying(s.variant), s.trackdirs); });
	compare(a.depots, b.depots, "depot", [](const BlueprintDepot &d) { return fmt::format("({},{}) rt{} dir{}", d.x, d.y, d.railtype, to_underlying(d.dir)); });
	compare(a.tunnelbridges, b.tunnelbridges, "tunnel/bridge", [](const BlueprintTunnelBridge &tb) { return fmt::format("({},{})-({},{}) rt{} {} type{}", tb.x, tb.y, tb.end_x, tb.end_y, tb.railtype, tb.bridge ? "bridge" : "tunnel", tb.bridge_type); });
	compare(a.stations, b.stations, "station tile", [](const BlueprintStationTile &st) { return fmt::format("({},{}) group{} rt{} axis{} {} spec{}", st.x, st.y, st.group, st.railtype, to_underlying(st.axis), st.waypoint ? "waypoint" : "station", st.spec); });

	if (lines > 20) format_append(diff, "... {} more\n", lines - 20);
	return diff;
}

/* --- Capture --- */

/**
 * Copy the rail infrastructure of an area into a blueprint.
 * @param corner1 One corner of the area.
 * @param corner2 The opposite corner of the area.
 * @param name Name of the blueprint.
 * @param[out] stats What was left out, if not nullptr.
 * @return The blueprint, normalised.
 */
Blueprint CaptureBlueprint(TileIndex corner1, TileIndex corner2, std::string_view name, BlueprintCaptureStats *stats)
{
	uint x0 = std::min(TileX(corner1), TileX(corner2));
	uint x1 = std::max(TileX(corner1), TileX(corner2));
	uint y0 = std::min(TileY(corner1), TileY(corner2));
	uint y1 = std::max(TileY(corner1), TileY(corner2));
	x1 = std::min(x1, x0 + Blueprint::MAX_SIZE - 1);
	y1 = std::min(y1, y0 + Blueprint::MAX_SIZE - 1);

	Blueprint bp;
	bp.name = name;
	bp.width = static_cast<uint16_t>(x1 - x0 + 1);
	bp.height = static_cast<uint16_t>(y1 - y0 + 1);

	auto rt_index = [&](RailType rt) -> uint8_t {
		RailTypeLabel label = GetRailTypeInfo(rt)->label;
		auto it = std::ranges::find(bp.railtypes, label);
		if (it != bp.railtypes.end()) return static_cast<uint8_t>(it - bp.railtypes.begin());
		bp.railtypes.push_back(label);
		return static_cast<uint8_t>(bp.railtypes.size() - 1);
	};
	auto spec_index = [&](const StationSpec *spec) -> uint16_t {
		if (spec == nullptr) return UINT16_MAX;
		BlueprintStationSpec ref{spec->grf_prop.grfid, spec->grf_prop.local_id};
		auto it = std::ranges::find(bp.specs, ref);
		if (it != bp.specs.end()) return static_cast<uint16_t>(it - bp.specs.begin());
		bp.specs.push_back(ref);
		return static_cast<uint16_t>(bp.specs.size() - 1);
	};
	auto inside = [&](TileIndex t) {
		return TileX(t) >= x0 && TileX(t) <= x1 && TileY(t) >= y0 && TileY(t) <= y1;
	};

	std::map<StationID, uint16_t> groups;

	for (uint y = y0; y <= y1; y++) {
		for (uint x = x0; x <= x1; x++) {
			TileIndex t = TileXY(x, y);
			uint16_t rx = static_cast<uint16_t>(x - x0);
			uint16_t ry = static_cast<uint16_t>(y - y0);

			switch (GetTileType(t)) {
				case TileType::Railway:
					if (IsRailDepot(t)) {
						bp.depots.push_back({rx, ry, rt_index(GetRailType(t)), GetRailDepotDirection(t)});
						break;
					} else {
						TrackBits bits = GetTrackBits(t);
						if (bits.None()) break;
						bp.tracks.push_back({rx, ry, rt_index(GetRailType(t)), bits});
						if (!HasSignals(t)) break;
						for (Track track : EnumRange(Track::End)) {
							if (!bits.Test(track) || !HasSignalOnTrack(t, track)) continue;
							Trackdir along = TrackToTrackdir(track);
							uint present = GetPresentSignals(t);
							uint8_t dirs = ((present & SignalAlongTrackdir(along)) != 0 ? 1 : 0) | ((present & SignalAlongTrackdir(ReverseTrackdir(along))) != 0 ? 2 : 0);
							bp.signals.push_back({rx, ry, track, GetSignalType(t, track), GetSignalVariant(t, track), dirs});
						}
					}
					break;

				case TileType::Road:
					if (IsLevelCrossing(t)) bp.tracks.push_back({rx, ry, rt_index(GetRailType(t)), TrackBits{GetCrossingRailTrack(t)}});
					break;

				case TileType::TunnelBridge: {
					if (GetTunnelBridgeTransportType(t) != TransportType::Rail) break;
					TileIndex other = GetOtherTunnelBridgeEnd(t);
					if (!inside(other)) {
						if (stats != nullptr) stats->partial_tunnelbridges++;
						break;
					}
					if (other < t) break; // Captured from the other end.
					bool bridge = IsBridge(t);
					bp.tunnelbridges.push_back({rx, ry, static_cast<uint16_t>(TileX(other) - x0), static_cast<uint16_t>(TileY(other) - y0),
							rt_index(GetRailType(t)), bridge, bridge ? GetBridgeType(t) : 0});
					break;
				}

				case TileType::Station: {
					bool waypoint = IsRailWaypointTile(t);
					if (!waypoint && !IsRailStationTile(t)) break;
					auto [it, inserted] = groups.try_emplace(GetStationIndex(t), static_cast<uint16_t>(groups.size()));
					bp.stations.push_back({rx, ry, it->second, rt_index(GetRailType(t)), GetRailStationAxis(t), waypoint, spec_index(GetStationSpec(t))});
					break;
				}

				default:
					break;
			}
		}
	}

	bp.Normalise();
	return bp;
}

/* --- Files --- */

static constexpr std::array<uint8_t, 4> BLUEPRINT_MAGIC = {'O', 'T', 'B', 'P'}; ///< Start of every blueprint file.
static constexpr uint16_t BLUEPRINT_VERSION = 1; ///< Version of the file format.
static constexpr std::string_view BLUEPRINT_EXTENSION = ".otbp"; ///< Extension of blueprint files.

/** Little endian writer into a byte vector. */
struct BlueprintWriter {
	std::vector<uint8_t> &out; ///< Destination.

	void U8(uint8_t v) { this->out.push_back(v); }
	void U16(uint16_t v) { this->U8(GB(v, 0, 8)); this->U8(GB(v, 8, 8)); }
	void U32(uint32_t v) { this->U16(GB(v, 0, 16)); this->U16(GB(v, 16, 16)); }
	void Bytes(std::span<const uint8_t> v) { this->out.insert(this->out.end(), v.begin(), v.end()); }
	void Str(std::string_view s)
	{
		this->U16(static_cast<uint16_t>(std::min<size_t>(s.size(), UINT16_MAX)));
		this->out.insert(this->out.end(), s.begin(), s.begin() + std::min<size_t>(s.size(), UINT16_MAX));
	}
};

/** Little endian reader with bounds checking; reading past the end sets #ok to false. */
struct BlueprintReader {
	std::span<const uint8_t> in; ///< Source.
	size_t pos = 0; ///< Read position.
	bool ok = true; ///< No read went past the end.

	uint8_t U8()
	{
		if (this->pos >= this->in.size()) {
			this->ok = false;
			return 0;
		}
		return this->in[this->pos++];
	}
	uint16_t U16() { uint16_t lo = this->U8(); return lo | static_cast<uint16_t>(this->U8()) << 8; }
	uint32_t U32() { uint32_t lo = this->U16(); return lo | static_cast<uint32_t>(this->U16()) << 16; }
	template <typename T> T Label4()
	{
		T label{};
		for (auto &c : label) c = this->U8();
		return label;
	}
	std::string Str()
	{
		size_t len = this->U16();
		std::string s;
		for (size_t i = 0; i < len && this->ok; i++) s.push_back(static_cast<char>(this->U8()));
		return s;
	}
	/** Read a count and check that the rest of the data can hold that many items of the given size. */
	uint32_t Count(size_t item_size)
	{
		uint32_t n = this->U32();
		if (static_cast<uint64_t>(n) * item_size > this->in.size() - std::min(this->pos, this->in.size())) {
			this->ok = false;
			return 0;
		}
		return n;
	}
};

/**
 * Encode a blueprint in the file format.
 * @param bp The blueprint.
 * @return The file contents.
 */
std::vector<uint8_t> SerialiseBlueprint(const Blueprint &bp)
{
	std::vector<uint8_t> data;
	BlueprintWriter w{data};
	w.Bytes(BLUEPRINT_MAGIC);
	w.U16(BLUEPRINT_VERSION);
	w.Str(bp.name);
	w.U16(bp.width);
	w.U16(bp.height);

	w.U32(static_cast<uint32_t>(bp.railtypes.size()));
	for (const RailTypeLabel &label : bp.railtypes) w.Bytes(label);
	w.U32(static_cast<uint32_t>(bp.specs.size()));
	for (const BlueprintStationSpec &spec : bp.specs) {
		w.Bytes(spec.grfid);
		w.U16(spec.local_id);
	}

	w.U32(static_cast<uint32_t>(bp.tracks.size()));
	for (const BlueprintTrack &t : bp.tracks) {
		w.U16(t.x); w.U16(t.y); w.U8(t.railtype); w.U8(t.tracks.base());
	}
	w.U32(static_cast<uint32_t>(bp.signals.size()));
	for (const BlueprintSignal &s : bp.signals) {
		w.U16(s.x); w.U16(s.y); w.U8(to_underlying(s.track)); w.U8(to_underlying(s.type)); w.U8(to_underlying(s.variant)); w.U8(s.trackdirs);
	}
	w.U32(static_cast<uint32_t>(bp.depots.size()));
	for (const BlueprintDepot &d : bp.depots) {
		w.U16(d.x); w.U16(d.y); w.U8(d.railtype); w.U8(to_underlying(d.dir));
	}
	w.U32(static_cast<uint32_t>(bp.tunnelbridges.size()));
	for (const BlueprintTunnelBridge &tb : bp.tunnelbridges) {
		w.U16(tb.x); w.U16(tb.y); w.U16(tb.end_x); w.U16(tb.end_y); w.U8(tb.railtype); w.U8(tb.bridge ? 1 : 0); w.U16(static_cast<uint16_t>(tb.bridge_type));
	}
	w.U32(static_cast<uint32_t>(bp.stations.size()));
	for (const BlueprintStationTile &st : bp.stations) {
		w.U16(st.x); w.U16(st.y); w.U16(st.group); w.U8(st.railtype); w.U8(to_underlying(st.axis)); w.U8(st.waypoint ? 1 : 0); w.U16(st.spec);
	}
	return data;
}

/**
 * Decode a blueprint from the file format, checking every value.
 * @param data The file contents.
 * @return The blueprint, or std::nullopt when the data is not a valid blueprint.
 */
std::optional<Blueprint> DeserialiseBlueprint(std::span<const uint8_t> data)
{
	BlueprintReader r{data};
	if (r.Label4<std::array<uint8_t, 4>>() != BLUEPRINT_MAGIC) return std::nullopt;
	if (r.U16() != BLUEPRINT_VERSION) return std::nullopt;

	Blueprint bp;
	bp.name = r.Str();
	bp.width = r.U16();
	bp.height = r.U16();
	if (bp.width == 0 || bp.height == 0 || bp.width > Blueprint::MAX_SIZE || bp.height > Blueprint::MAX_SIZE) return std::nullopt;

	auto in_area = [&](uint16_t x, uint16_t y) { return x < bp.width && y < bp.height; };

	uint32_t n = r.Count(4);
	if (n > UINT8_MAX + 1) return std::nullopt;
	for (uint32_t i = 0; i < n; i++) bp.railtypes.push_back(r.Label4<RailTypeLabel>());
	auto rt_ok = [&](uint8_t rt) { return rt < bp.railtypes.size(); };

	n = r.Count(6);
	if (n >= UINT16_MAX) return std::nullopt;
	for (uint32_t i = 0; i < n; i++) {
		BlueprintStationSpec spec;
		spec.grfid = r.Label4<GrfID>();
		spec.local_id = r.U16();
		bp.specs.push_back(spec);
	}

	n = r.Count(6);
	for (uint32_t i = 0; i < n && r.ok; i++) {
		BlueprintTrack t;
		t.x = r.U16(); t.y = r.U16(); t.railtype = r.U8(); t.tracks = TrackBits{r.U8()};
		if (!in_area(t.x, t.y) || !rt_ok(t.railtype) || t.tracks.None() || !TRACK_BIT_ALL.All(t.tracks)) return std::nullopt;
		bp.tracks.push_back(t);
	}
	n = r.Count(8);
	for (uint32_t i = 0; i < n && r.ok; i++) {
		BlueprintSignal s;
		s.x = r.U16(); s.y = r.U16();
		uint8_t track = r.U8(), type = r.U8(), variant = r.U8();
		s.trackdirs = r.U8();
		if (!in_area(s.x, s.y) || track >= to_underlying(Track::End) || type >= to_underlying(SignalType::End) || variant >= to_underlying(SignalVariant::End)) return std::nullopt;
		if (s.trackdirs == 0 || s.trackdirs > 3) return std::nullopt;
		s.track = static_cast<Track>(track);
		s.type = static_cast<SignalType>(type);
		s.variant = static_cast<SignalVariant>(variant);
		bp.signals.push_back(s);
	}
	n = r.Count(6);
	for (uint32_t i = 0; i < n && r.ok; i++) {
		BlueprintDepot d;
		d.x = r.U16(); d.y = r.U16(); d.railtype = r.U8();
		uint8_t dir = r.U8();
		if (!in_area(d.x, d.y) || !rt_ok(d.railtype) || dir >= to_underlying(DiagDirection::End)) return std::nullopt;
		d.dir = static_cast<DiagDirection>(dir);
		bp.depots.push_back(d);
	}
	n = r.Count(12);
	for (uint32_t i = 0; i < n && r.ok; i++) {
		BlueprintTunnelBridge tb;
		tb.x = r.U16(); tb.y = r.U16(); tb.end_x = r.U16(); tb.end_y = r.U16(); tb.railtype = r.U8();
		uint8_t bridge = r.U8();
		tb.bridge = bridge != 0;
		tb.bridge_type = r.U16();
		if (!in_area(tb.x, tb.y) || !in_area(tb.end_x, tb.end_y) || !rt_ok(tb.railtype) || bridge > 1) return std::nullopt;
		if ((tb.x != tb.end_x) == (tb.y != tb.end_y)) return std::nullopt; // Must be a straight line along one axis.
		bp.tunnelbridges.push_back(tb);
	}
	n = r.Count(11);
	for (uint32_t i = 0; i < n && r.ok; i++) {
		BlueprintStationTile st;
		st.x = r.U16(); st.y = r.U16(); st.group = r.U16(); st.railtype = r.U8();
		uint8_t axis = r.U8(), waypoint = r.U8();
		st.spec = r.U16();
		if (!in_area(st.x, st.y) || !rt_ok(st.railtype) || axis >= to_underlying(Axis::End) || waypoint > 1) return std::nullopt;
		if (st.spec != UINT16_MAX && st.spec >= bp.specs.size()) return std::nullopt;
		st.axis = static_cast<Axis>(axis);
		st.waypoint = waypoint != 0;
		bp.stations.push_back(st);
	}

	if (!r.ok) return std::nullopt;
	bp.Normalise();
	return bp;
}

/**
 * Get the directory of the blueprint files, creating it when needed.
 * @return Path ending with a path separator.
 */
std::string GetBlueprintDirectory()
{
	std::string dir = FioFindDirectory(Subdirectory::Blueprint);
	FioCreateDirectory(dir);
	return dir;
}

/**
 * Get the file name of a blueprint: its name with characters that are not allowed in file names replaced.
 * @param name Name of the blueprint.
 * @return Full path of the file.
 */
std::string GetBlueprintFileName(std::string_view name)
{
	std::string file{name};
	for (char &c : file) {
		if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|' || static_cast<uint8_t>(c) < 0x20) c = '_';
	}
	if (file.empty() || file == "." || file == "..") file = "_";
	return fmt::format("{}{}{}", GetBlueprintDirectory(), file, BLUEPRINT_EXTENSION);
}

/**
 * Write a blueprint to its file.
 * @param bp The blueprint; its name gives the file name.
 * @return True on success.
 */
bool SaveBlueprint(const Blueprint &bp)
{
	std::vector<uint8_t> data = SerialiseBlueprint(bp);
	auto f = FileHandle::Open(GetBlueprintFileName(bp.name), "wb");
	if (!f.has_value()) return false;
	return fwrite(data.data(), 1, data.size(), *f) == data.size();
}

/**
 * Read a blueprint from its file.
 * @param name Name of the blueprint.
 * @return The blueprint, or std::nullopt when the file is missing or broken.
 */
std::optional<Blueprint> LoadBlueprint(std::string_view name)
{
	auto f = FileHandle::Open(GetBlueprintFileName(name), "rb");
	if (!f.has_value()) return std::nullopt;

	std::vector<uint8_t> data;
	std::array<uint8_t, 65536> buf;
	size_t n;
	while ((n = fread(buf.data(), 1, buf.size(), *f)) > 0) data.insert(data.end(), buf.begin(), buf.begin() + n);

	std::optional<Blueprint> bp = DeserialiseBlueprint(data);
	if (bp.has_value()) bp->name = name;
	return bp;
}

/**
 * Delete the file of a blueprint.
 * @param name Name of the blueprint.
 * @return True when the file was removed.
 */
bool DeleteBlueprint(std::string_view name)
{
	std::error_code error_code;
	return std::filesystem::remove(OTTD2FS(GetBlueprintFileName(name)), error_code);
}

/**
 * Rename a blueprint.
 * @param name Current name.
 * @param new_name New name; a blueprint with that name must not exist.
 * @return True on success.
 */
bool RenameBlueprint(std::string_view name, std::string_view new_name)
{
	if (name == new_name) return true;
	std::optional<Blueprint> bp = LoadBlueprint(name);
	if (!bp.has_value()) return false;
	std::error_code error_code;
	if (std::filesystem::exists(OTTD2FS(GetBlueprintFileName(new_name)), error_code)) return false;
	bp->name = new_name;
	if (!SaveBlueprint(*bp)) return false;
	DeleteBlueprint(name);
	return true;
}

/**
 * List the saved blueprints.
 * @return Names of the blueprints, sorted.
 */
std::vector<std::string> ListBlueprints()
{
	std::vector<std::string> names;
	std::error_code error_code;
	for (const auto &entry : std::filesystem::directory_iterator(OTTD2FS(GetBlueprintDirectory()), error_code)) {
		if (!entry.is_regular_file(error_code)) continue;
		std::string file = FS2OTTD(entry.path().filename().native());
		if (file.size() <= BLUEPRINT_EXTENSION.size() || !file.ends_with(BLUEPRINT_EXTENSION)) continue;
		names.push_back(file.substr(0, file.size() - BLUEPRINT_EXTENSION.size()));
	}
	std::ranges::sort(names, [](const std::string &a, const std::string &b) { return StrNaturalCompare(a, b) < 0; });
	return names;
}

/* --- Planning a paste --- */

/** Writer of the items of a paste into chunks that each fit in one command. */
class BlueprintChunkWriter {
	std::vector<BlueprintChunkData> &chunks; ///< Destination.
	std::vector<uint8_t> item{}; ///< Item being written.

public:
	explicit BlueprintChunkWriter(std::vector<BlueprintChunkData> &chunks) : chunks(chunks) {}

	BlueprintWriter Begin(BlueprintItemKind kind)
	{
		this->item.clear();
		BlueprintWriter w{this->item};
		w.U8(to_underlying(kind));
		return w;
	}

	/** Free bytes in the current chunk. */
	size_t Free() const
	{
		if (this->chunks.empty()) return 0;
		return BlueprintChunkData::MAX_SIZE - this->chunks.back().data.size();
	}

	/** Start a new chunk. */
	void NewChunk() { this->chunks.emplace_back(); }

	/**
	 * Append the item that was written since #Begin.
	 * @return True when a new chunk was started for it.
	 */
	bool Commit()
	{
		bool fresh = false;
		if (this->item.size() > this->Free()) {
			this->NewChunk();
			fresh = true;
		}
		auto &data = this->chunks.back().data;
		data.insert(data.end(), this->item.begin(), this->item.end());
		return fresh;
	}
};

/** A rectangle of station or waypoint tiles with the same properties. */
struct BlueprintStationRect {
	uint16_t x, y, w, h; ///< Area.
	const BlueprintStationTile *tile; ///< Properties (of the first tile).
};

/**
 * Split the tiles of one station into rectangles of equal tiles.
 * Waypoints are built in rows of one tile wide along their axis.
 * @param tiles The tiles of the station, sorted by position.
 * @return The rectangles.
 */
static std::vector<BlueprintStationRect> SplitStationIntoRects(const std::vector<const BlueprintStationTile *> &tiles)
{
	std::map<std::pair<uint16_t, uint16_t>, const BlueprintStationTile *> at;
	for (const BlueprintStationTile *st : tiles) at[{st->x, st->y}] = st;
	std::set<std::pair<uint16_t, uint16_t>> used;

	auto same = [](const BlueprintStationTile *a, const BlueprintStationTile *b) {
		return a->railtype == b->railtype && a->axis == b->axis && a->waypoint == b->waypoint && a->spec == b->spec;
	};
	auto free_same = [&](uint x, uint y, const BlueprintStationTile *ref) {
		if (x > UINT16_MAX || y > UINT16_MAX) return false;
		auto it = at.find({static_cast<uint16_t>(x), static_cast<uint16_t>(y)});
		return it != at.end() && !used.contains(it->first) && same(it->second, ref);
	};

	std::vector<BlueprintStationRect> rects;
	for (const BlueprintStationTile *st : tiles) {
		if (used.contains({st->x, st->y})) continue;
		/* Waypoints: one tile along the axis; stations: at most 255 x 255 (command limits). */
		uint max_w = (st->waypoint && st->axis == Axis::X) ? 1 : UINT8_MAX;
		uint max_h = (st->waypoint && st->axis == Axis::Y) ? 1 : UINT8_MAX;

		uint w = 1;
		while (w < max_w && free_same(st->x + w, st->y, st)) w++;
		uint h = 1;
		while (h < max_h) {
			bool row = true;
			for (uint i = 0; i < w && row; i++) row = free_same(st->x + i, st->y + h, st);
			if (!row) break;
			h++;
		}
		for (uint j = 0; j < h; j++) {
			for (uint i = 0; i < w; i++) used.insert({static_cast<uint16_t>(st->x + i), static_cast<uint16_t>(st->y + j)});
		}
		rects.push_back({st->x, st->y, static_cast<uint16_t>(w), static_cast<uint16_t>(h), st});
	}
	return rects;
}

/**
 * Turn a blueprint into the commands that build it.
 * Items are ordered so that everything an item needs is built before it: tunnels, bridges, track,
 * stations and waypoints, depots, and signals last.
 * @param bp The blueprint, untransformed.
 * @param origin Tile of the north corner of the transformed blueprint.
 * @param options Transformation and rail type.
 * @return The plan.
 */
BlueprintPastePlan PlanBlueprintPaste(const Blueprint &bp, TileIndex origin, const BlueprintPasteOptions &options)
{
	BlueprintPastePlan plan;
	plan.origin = origin;

	Blueprint tbp = bp.Transformed(options.transform);
	if (tbp.IsEmpty()) {
		plan.error = "the blueprint is empty";
		return plan;
	}
	if (origin >= Map::Size() || TileX(origin) + tbp.width > Map::MaxX() || TileY(origin) + tbp.height > Map::MaxY()) {
		plan.error = "the blueprint does not fit on the map here";
		return plan;
	}

	/* Rail types and station specs of this game. */
	std::vector<RailType> railtypes;
	for (const RailTypeLabel &label : tbp.railtypes) {
		RailType rt = options.railtype;
		if (rt == INVALID_RAILTYPE) rt = GetRailTypeByLabel(label);
		if (rt == INVALID_RAILTYPE) rt = RAILTYPE_RAIL;
		railtypes.push_back(rt);
	}
	std::vector<const StationSpec *> specs;
	for (const BlueprintStationSpec &ref : tbp.specs) specs.push_back(StationClass::GetByGrf(ref.grfid, ref.local_id));

	BlueprintChunkWriter cw(plan.chunks);

	for (const BlueprintTunnelBridge &tb : tbp.tunnelbridges) {
		if (tb.bridge) continue;
		BlueprintWriter w = cw.Begin(BlueprintItemKind::Tunnel);
		w.U16(tb.x); w.U16(tb.y); w.U16(tb.end_x); w.U16(tb.end_y); w.U8(railtypes[tb.railtype]);
		cw.Commit();
		plan.items++;
	}
	for (const BlueprintTunnelBridge &tb : tbp.tunnelbridges) {
		if (!tb.bridge) continue;
		BlueprintWriter w = cw.Begin(BlueprintItemKind::Bridge);
		w.U16(tb.x); w.U16(tb.y); w.U16(tb.end_x); w.U16(tb.end_y); w.U8(railtypes[tb.railtype]); w.U8(static_cast<uint8_t>(tb.bridge_type));
		cw.Commit();
		plan.items++;
	}
	for (const BlueprintTrack &t : tbp.tracks) {
		BlueprintWriter w = cw.Begin(BlueprintItemKind::Track);
		w.U16(t.x); w.U16(t.y); w.U8(railtypes[t.railtype]); w.U8(t.tracks.base());
		cw.Commit();
		plan.items++;
	}
	/* Waypoints are built on existing rail. */
	for (const BlueprintStationTile &st : tbp.stations) {
		if (!st.waypoint) continue;
		BlueprintWriter w = cw.Begin(BlueprintItemKind::Track);
		w.U16(st.x); w.U16(st.y); w.U8(railtypes[st.railtype]); w.U8(TrackBits{AxisToTrack(st.axis)}.base());
		cw.Commit();
	}

	/* Stations and waypoints: the rectangles of one station stay in one chunk when possible. */
	std::map<uint16_t, std::vector<const BlueprintStationTile *>> groups;
	for (const BlueprintStationTile &st : tbp.stations) groups[st.group].push_back(&st);
	static constexpr size_t STATION_ITEM_SIZE = 15;
	size_t numbering_chunk = SIZE_MAX; // Chunk whose station groups are being numbered.
	uint8_t next_group = 0; // Next group number in that chunk; at most MAX_SIZE / STATION_ITEM_SIZE groups fit in a chunk.
	for (const auto &[group, tiles] : groups) {
		std::vector<BlueprintStationRect> rects = SplitStationIntoRects(tiles);
		size_t needed = rects.size() * STATION_ITEM_SIZE;
		if (needed > cw.Free() && needed <= BlueprintChunkData::MAX_SIZE) cw.NewChunk();

		bool first = true;
		size_t group_chunk = SIZE_MAX; // Chunk the current part of this station is in.
		uint8_t local_group = 0;
		for (const BlueprintStationRect &rect : rects) {
			const BlueprintStationTile &st = *rect.tile;
			StationClassID cls = st.waypoint ? STAT_CLASS_WAYP : STAT_CLASS_DFLT;
			uint16_t index = 0;
			const StationSpec *spec = st.spec < specs.size() ? specs[st.spec] : nullptr;
			if (spec != nullptr && IsWaypointClass(*StationClass::Get(spec->class_index)) == st.waypoint) {
				cls = spec->class_index;
				index = spec->index;
			}

			BlueprintWriter w = cw.Begin(st.waypoint ? BlueprintItemKind::Waypoint : BlueprintItemKind::Station);
			w.U16(rect.x); w.U16(rect.y); w.U8(static_cast<uint8_t>(rect.w)); w.U8(static_cast<uint8_t>(rect.h));
			w.U8(to_underlying(st.axis)); w.U8(railtypes[st.railtype]);
			w.U16(cls.base()); w.U16(index);
			w.U8(0); w.U8(0); // Group and flags, set below.
			cw.Commit();

			size_t chunk = plan.chunks.size() - 1;
			if (chunk != numbering_chunk) {
				numbering_chunk = chunk;
				next_group = 0;
			}
			uint8_t flags = 0;
			if (chunk != group_chunk) {
				/* The station starts in this chunk, or continues here: then it joins what was built before. */
				group_chunk = chunk;
				local_group = next_group++;
				if (!first) flags = 1 << to_underlying(BlueprintStationFlag::JoinNeighbour);
			}
			auto &data = plan.chunks.back().data;
			data[data.size() - 2] = local_group;
			data[data.size() - 1] = flags;

			first = false;
			plan.items++;
		}
	}

	for (const BlueprintDepot &d : tbp.depots) {
		BlueprintWriter w = cw.Begin(BlueprintItemKind::Depot);
		w.U16(d.x); w.U16(d.y); w.U8(railtypes[d.railtype]); w.U8(to_underlying(d.dir));
		cw.Commit();
		plan.items++;
	}
	for (const BlueprintSignal &s : tbp.signals) {
		Trackdir along = TrackToTrackdir(s.track);
		uint8_t present = (HasBit(s.trackdirs, 0) ? SignalAlongTrackdir(along) : 0) | (HasBit(s.trackdirs, 1) ? SignalAlongTrackdir(ReverseTrackdir(along)) : 0);
		BlueprintWriter w = cw.Begin(BlueprintItemKind::Signal);
		w.U16(s.x); w.U16(s.y); w.U8(to_underlying(s.track)); w.U8(to_underlying(s.type)); w.U8(to_underlying(s.variant)); w.U8(present);
		cw.Commit();
		plan.items++;
	}

	return plan;
}

/* --- The paste job --- */

static BlueprintPasteStatus _paste_status; ///< State of the current or last paste.
static std::deque<BlueprintChunkData> _paste_queue; ///< Chunks not sent yet.
static TileIndex _paste_origin = INVALID_TILE; ///< Origin of the chunks.
static CompanyID _paste_company = CompanyID::Invalid(); ///< Company that pastes.
static bool _paste_callback_seen = false; ///< The callback of the last posted command ran.

/** Number of paste commands that may wait for their result at the same time in a network game. */
static constexpr size_t PASTE_MAX_IN_FLIGHT_NETWORK = 6;
/** Number of paste commands sent per tick of the job in a single player game. */
static constexpr size_t PASTE_MAX_PER_RUN_LOCAL = 64;

/**
 * Get the state of the current or last paste.
 * @return The state.
 */
const BlueprintPasteStatus &GetBlueprintPasteStatus()
{
	return _paste_status;
}

/** Finish the paste job: report the result. */
static void FinishBlueprintPaste()
{
	_paste_status.active = false;
	_paste_queue.clear();
	IConsolePrint(_paste_status.failed == 0 && !_paste_status.out_of_money && !_paste_status.aborted ? CC_INFO : CC_WARNING,
		"Blueprint '{}' pasted: {} built, {} failed, cost {}{}{}{}.", _paste_status.name, _paste_status.built, _paste_status.failed, static_cast<int64_t>(_paste_status.cost),
		_paste_status.out_of_money ? ", stopped: out of money" : "",
		_paste_status.aborted ? ", aborted" : "",
		_paste_status.last_error != INVALID_STRING_ID && _paste_status.failed != 0 ? fmt::format(", last error: {}", GetString(_paste_status.last_error)) : "");
	InvalidateWindowClassesData(WindowClass::Blueprints);
}

/**
 * Result of one paste command.
 * @param result Cost of the built items, or the error when nothing was built.
 * @param extra_money Money that was missing to build the next item.
 * @param built Items built.
 * @param failed Items that failed.
 * @param last_error Error of the last failed item.
 */
void CcPasteBlueprint(Commands, const CommandCost &result, Money extra_money, uint32_t built, uint32_t failed, StringID last_error)
{
	_paste_callback_seen = true;
	if (!_paste_status.active) return;

	_paste_status.chunks_done++;
	_paste_status.built += built;
	_paste_status.failed += failed;
	if (result.Succeeded()) _paste_status.cost += result.GetCost();
	if (last_error != INVALID_STRING_ID) _paste_status.last_error = last_error;
	if (result.Failed() && result.GetErrorMessage() != INVALID_STRING_ID && result.GetErrorMessage() != STR_NULL) _paste_status.last_error = result.GetErrorMessage();
	if (extra_money != 0) {
		_paste_status.out_of_money = true;
		_paste_queue.clear();
	}

	if (_paste_queue.empty() && _paste_status.chunks_done >= _paste_status.chunks_sent) FinishBlueprintPaste();
	InvalidateWindowClassesData(WindowClass::Blueprints);
}

/**
 * Start pasting a blueprint. Only one paste runs at a time.
 * @param name Name of the blueprint (for messages).
 * @param plan The commands of the paste.
 * @param company Company that pastes.
 * @return False when the plan is empty or another paste is running.
 */
bool StartBlueprintPaste(std::string_view name, BlueprintPastePlan &&plan, CompanyID company)
{
	if (_paste_status.active || plan.chunks.empty()) return false;

	_paste_status = {};
	_paste_status.active = true;
	_paste_status.name = name;
	_paste_status.chunks_total = plan.chunks.size();
	_paste_queue.assign(std::make_move_iterator(plan.chunks.begin()), std::make_move_iterator(plan.chunks.end()));
	_paste_origin = plan.origin;
	_paste_company = company;
	RunBlueprintPasteJob();
	return true;
}

/** Stop sending the commands of the current paste; commands already sent still execute. */
void AbortBlueprintPaste()
{
	if (!_paste_status.active) return;
	_paste_status.aborted = true;
	_paste_queue.clear();
	if (_paste_status.chunks_done >= _paste_status.chunks_sent) FinishBlueprintPaste();
}

/** Send the next commands of the paste job. */
void RunBlueprintPasteJob()
{
	if (!_paste_status.active) return;

	size_t sent = 0;
	while (!_paste_queue.empty() && _paste_status.active) {
		size_t in_flight = _paste_status.chunks_sent - _paste_status.chunks_done;
		if (_networking ? in_flight >= PASTE_MAX_IN_FLIGHT_NETWORK : sent >= PASTE_MAX_PER_RUN_LOCAL) break;

		/* Commands of a company that no longer exists (or that we no longer play) are not sent. */
		if (!Company::IsValidID(_paste_company) || (!_network_server && _paste_company != _local_company)) {
			AbortBlueprintPaste();
			return;
		}

		BlueprintChunkData chunk = std::move(_paste_queue.front());
		_paste_queue.pop_front();
		_paste_status.chunks_sent++;
		sent++;

		_paste_callback_seen = false;
		Backup<CompanyID> cur_company(_current_company, _paste_company);
		bool ok = Command<Commands::PasteBlueprint>::Post(STR_NULL, CcPasteBlueprint, _paste_origin, chunk);
		cur_company.Restore();
		if (!ok && !_paste_callback_seen) {
			/* Refused before execution, e.g. while the game is paused. */
			_paste_status.chunks_done++;
			_paste_status.aborted = true;
			_paste_queue.clear();
			if (_paste_status.chunks_done >= _paste_status.chunks_sent) FinishBlueprintPaste();
			return;
		}
	}
}

/** Send the commands of the paste job a few at a time. */
static const IntervalTimer<TimerGameRealtime> _blueprint_paste_timer({std::chrono::milliseconds(30), TimerGameRealtime::Trigger::Always}, [](auto) {
	RunBlueprintPasteJob();
});
