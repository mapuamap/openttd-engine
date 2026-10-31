/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file blueprint.cpp Test the transformations and the file format of rail blueprints. */

#include "../stdafx.h"

#include "../3rdparty/catch2/catch.hpp"

#include "../blueprint.h"
#include "../track_func.h"

#include "../safeguards.h"

/** All eight transformations. */
static std::vector<BlueprintTransform> AllTransforms()
{
	std::vector<BlueprintTransform> all;
	for (bool mirror : {false, true}) {
		for (uint8_t rotation = 0; rotation < 4; rotation++) all.push_back({rotation, mirror});
	}
	return all;
}

/** Offset to the neighbouring tile across an edge. */
static std::pair<int, int> EdgeDelta(DiagDirection dir)
{
	switch (dir) {
		case DiagDirection::NE: return {-1, 0};
		case DiagDirection::SE: return {0, 1};
		case DiagDirection::SW: return {1, 0};
		case DiagDirection::NW: return {0, -1};
		default: NOT_REACHED();
	}
}

TEST_CASE("BlueprintTransformDiagDir")
{
	/* Clockwise on the screen: north-east, south-east, south-west, north-west. */
	CHECK(TransformDiagDir(DiagDirection::NE, {1, false}) == DiagDirection::SE);
	CHECK(TransformDiagDir(DiagDirection::NW, {1, false}) == DiagDirection::NE);
	CHECK(TransformDiagDir(DiagDirection::NE, {0, true}) == DiagDirection::SW);
	CHECK(TransformDiagDir(DiagDirection::SE, {0, true}) == DiagDirection::SE);
	CHECK(TransformAxis(Axis::X, {1, false}) == Axis::Y);
	CHECK(TransformAxis(Axis::X, {2, true}) == Axis::X);
}

TEST_CASE("BlueprintTransformPositionsMatchDirections")
{
	/* Moving from a tile across an edge and transforming must give the same tile as transforming
	 * the tile and moving across the transformed edge. */
	const uint w = 5, h = 3;
	for (BlueprintTransform tf : AllTransforms()) {
		for (DiagDirection dir : {DiagDirection::NE, DiagDirection::SE, DiagDirection::SW, DiagDirection::NW}) {
			auto [dx, dy] = EdgeDelta(dir);
			uint x = 2, y = 1;
			auto [tx, ty] = TransformPosition(x, y, w, h, tf);
			auto [nx, ny] = TransformPosition(x + dx, y + dy, w, h, tf);
			auto [tdx, tdy] = EdgeDelta(TransformDiagDir(dir, tf));
			CHECK(static_cast<int>(nx) == static_cast<int>(tx) + tdx);
			CHECK(static_cast<int>(ny) == static_cast<int>(ty) + tdy);
		}
		/* Corners stay inside the transformed area. */
		auto [tw, th] = TransformSize(w, h, tf);
		for (auto [cx, cy] : {std::pair<uint, uint>{0, 0}, {w - 1, 0}, {0, h - 1}, {w - 1, h - 1}}) {
			auto [px, py] = TransformPosition(cx, cy, w, h, tf);
			CHECK(px < tw);
			CHECK(py < th);
		}
	}
}

TEST_CASE("BlueprintTransformTracks")
{
	CHECK(TransformTrack(Track::X, {1, false}) == Track::Y);
	CHECK(TransformTrack(Track::Upper, {1, false}) == Track::Right);
	CHECK(TransformTrack(Track::Right, {1, false}) == Track::Lower);
	CHECK(TransformTrack(Track::Upper, {0, true}) == Track::Left);
	CHECK(TransformTrack(Track::X, {0, true}) == Track::X);

	for (BlueprintTransform tf : AllTransforms()) {
		/* A track keeps connecting the transformed edges. */
		for (Track t : EnumRange(Track::End)) {
			Track nt = TransformTrack(t, tf);
			for (Trackdir td : {TrackToTrackdir(t), ReverseTrackdir(TrackToTrackdir(t))}) {
				Trackdir ntd = TransformTrackdir(td, tf);
				CHECK(TrackdirToTrack(ntd) == nt);
				CHECK(TrackdirToExitdir(ntd) == TransformDiagDir(TrackdirToExitdir(td), tf));
			}
		}
		/* Track bits are a bijection. */
		std::set<uint8_t> seen;
		for (uint bits = 0; bits < 64; bits++) seen.insert(TransformTrackBits(TrackBits{static_cast<uint8_t>(bits)}, tf).base());
		CHECK(seen.size() == 64);
	}
}

/** A blueprint with every kind of item and no symmetry. */
static Blueprint MakeTestBlueprint()
{
	Blueprint bp;
	bp.name = "test";
	bp.width = 7;
	bp.height = 4;
	bp.railtypes = {RailTypeLabel{"RAIL"}, RailTypeLabel{"ELRL"}};
	bp.specs = {{GrfID{"ABCD"}, 3}};
	bp.tracks = {
		{0, 0, 0, TrackBits{Track::X}},
		{1, 0, 1, TrackBits{Track::X, Track::Upper}},
		{2, 1, 0, TrackBits{Track::Left, Track::Lower}},
		{6, 3, 1, TrackBits{Track::Y}},
	};
	bp.signals = {
		{0, 0, Track::X, SignalType::PathOneWay, SignalVariant::Electric, 1},
		{2, 1, Track::Left, SignalType::Entry, SignalVariant::Semaphore, 3},
		{6, 3, Track::Y, SignalType::Path, SignalVariant::Electric, 2},
	};
	bp.depots = {{3, 2, 0, DiagDirection::SW}};
	bp.tunnelbridges = {{0, 3, 5, 3, 1, true, 4}, {4, 0, 4, 2, 0, false, 0}};
	bp.stations = {
		{5, 0, 0, 0, Axis::X, false, UINT16_MAX},
		{6, 0, 0, 0, Axis::X, false, UINT16_MAX},
		{5, 1, 1, 1, Axis::Y, true, 0},
	};
	bp.Normalise();
	return bp;
}

TEST_CASE("BlueprintTransformGroup")
{
	Blueprint bp = MakeTestBlueprint();
	for (BlueprintTransform tf : AllTransforms()) {
		Blueprint once = bp.Transformed(tf);
		CHECK(once.CountItems() == bp.CountItems());

		/* Rotating further to a full turn gives the (mirrored) original back. */
		Blueprint back = once.Transformed({static_cast<uint8_t>((4 - tf.rotation) % 4), false});
		Blueprint expected = tf.mirror ? bp.Transformed({0, true}) : bp;
		CHECK(back.Compare(expected).empty());
	}

	/* Two quarter turns equal a half turn; mirroring twice is the identity. */
	CHECK(bp.Transformed({1, false}).Transformed({1, false}).Compare(bp.Transformed({2, false})).empty());
	CHECK(bp.Transformed({0, true}).Transformed({0, true}).Compare(bp).empty());
	/* The transformation is not trivial. */
	CHECK_FALSE(bp.Transformed({1, false}).Compare(bp).empty());
	CHECK_FALSE(bp.Transformed({0, true}).Compare(bp).empty());
}

TEST_CASE("BlueprintTransformSignals")
{
	/* A one-way signal for trains running north-east on an X track, turned clockwise: trains running south-east on a Y track. */
	Blueprint bp;
	bp.width = 1;
	bp.height = 1;
	bp.railtypes = {RailTypeLabel{"RAIL"}};
	bp.tracks = {{0, 0, 0, TrackBits{Track::X}}};
	Trackdir along = TrackToTrackdir(Track::X);
	REQUIRE(TrackdirToExitdir(along) == DiagDirection::NE);
	bp.signals = {{0, 0, Track::X, SignalType::PathOneWay, SignalVariant::Electric, 1}};

	Blueprint turned = bp.Transformed({1, false});
	REQUIRE(turned.signals.size() == 1);
	const BlueprintSignal &s = turned.signals[0];
	CHECK(s.track == Track::Y);
	Trackdir dir = HasBit(s.trackdirs, 0) ? TrackToTrackdir(s.track) : ReverseTrackdir(TrackToTrackdir(s.track));
	CHECK(s.trackdirs != 3);
	CHECK(TrackdirToExitdir(dir) == DiagDirection::SE);
}

TEST_CASE("BlueprintFileFormat")
{
	Blueprint bp = MakeTestBlueprint();
	std::vector<uint8_t> data = SerialiseBlueprint(bp);
	std::optional<Blueprint> loaded = DeserialiseBlueprint(data);
	REQUIRE(loaded.has_value());
	CHECK(loaded->Compare(bp).empty());
	CHECK(loaded->name == bp.name);

	/* Truncated or damaged data is refused, never read out of bounds. */
	for (size_t len = 0; len < data.size(); len++) {
		CHECK_FALSE(DeserialiseBlueprint(std::span(data.data(), len)).has_value());
	}
	std::vector<uint8_t> bad = data;
	bad[0] = 'X';
	CHECK_FALSE(DeserialiseBlueprint(bad).has_value());
}
