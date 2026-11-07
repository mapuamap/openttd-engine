/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/>.
 */

/** @file agent_transport_tools.cpp Test the construction tool dispatch of the agent bridge. */

#include "../stdafx.h"

#include "../3rdparty/catch2/catch.hpp"

#include "../agent_transport_tools.h"

#include "../company_base.h"
#include "../map_func.h"

#include "../safeguards.h"

namespace {

/** A company id that is never valid, so a write op stops at the company check. */
const CompanyID INVALID_COMPANY{15};

/** The company id #FakeCompany makes valid. */
const CompanyID TEST_COMPANY{0};

/**
 * Make one company id valid for the duration of a test.
 *
 * The module only asks `Company::IsValidID` before it parses the rest of a call, and the argument
 * errors below are all raised before any company field is read, so a placeholder pool entry is
 * enough and no game has to be started. The pool is restored afterwards, so the other test cases
 * see the same empty pool they would see without this helper.
 */
class FakeCompany {
public:
	FakeCompany()
	{
		this->data_was_empty = _company_pool.data.empty();
		if (this->data_was_empty) _company_pool.data.push_back(reinterpret_cast<Company *>(this));
		this->first_unused = _company_pool.first_unused;
		this->items = _company_pool.items;
		_company_pool.first_unused = 1;
		_company_pool.items = 1;
	}

	~FakeCompany()
	{
		_company_pool.first_unused = this->first_unused;
		_company_pool.items = this->items;
		if (this->data_was_empty) _company_pool.data.clear();
	}

private:
	size_t first_unused; ///< `first_unused` before the placeholder was added.
	size_t items;        ///< `items` before the placeholder was added.
	bool data_was_empty; ///< Whether the placeholder had to be added to `data`.
};

/**
 * Check whether a capability list holds an entry.
 * @param list The capability list.
 * @param entry The entry to look for.
 * @return True when the entry is present.
 */
bool HasEntry(const InGameAgent::Json &list, std::string_view entry)
{
	for (const InGameAgent::Json &item : list) {
		if (item.is_string() && item.get<std::string>() == entry) return true;
	}
	return false;
}

/**
 * Dispatch one op with an invalid company and return the result.
 * @param params Method parameters.
 * @return The tool result, which must be present because the op is owned.
 */
std::optional<InGameAgent::Json> Dispatch(const InGameAgent::Json &params)
{
	return InGameAgent::DispatchTransportTool("world.construct", params, INVALID_COMPANY);
}

/**
 * Dispatch one op with the test company and require the given argument error.
 * @param params Method parameters.
 * @param code Expected `error` code.
 * @param fragment Text the `message` must contain; empty to skip the check.
 */
void CheckError(const InGameAgent::Json &params, std::string_view code, std::string_view fragment = {})
{
	const std::optional<InGameAgent::Json> result = InGameAgent::DispatchTransportTool("world.construct", params, TEST_COMPANY);
	INFO("params: " << params.dump());
	REQUIRE(result.has_value());
	CHECK((*result)["ok"] == false);
	CHECK((*result)["error"] == code);
	if (!fragment.empty()) {
		REQUIRE((*result)["message"].is_string());
		CHECK((*result)["message"].get<std::string>().find(std::string(fragment)) != std::string::npos);
	}
}

/** One tile at 1,1, inside the map the tests allocate. */
InGameAgent::Json OneTile()
{
	return InGameAgent::Json::array({{{"x", 1}, {"y", 1}}});
}

/** Two tiles, one too many for the ops that build on a single tile. */
InGameAgent::Json TwoTiles()
{
	return InGameAgent::Json::array({{{"x", 1}, {"y", 1}}, {{"x", 2}, {"y", 1}}});
}

} // namespace

TEST_CASE("agent transport tools: capabilities")
{
	const InGameAgent::Json caps = InGameAgent::TransportToolCapabilities();
	CHECK(caps["module"] == "agent_transport_tools");

	REQUIRE(caps["methods"].is_array());
	REQUIRE(caps["methods"].size() == 1);
	const InGameAgent::Json &construct = caps["methods"][0];
	CHECK(construct["method"] == "world.construct");
	CHECK(construct["authority"] == "write");

	/* Only implemented operations are advertised. */
	for (const std::string_view op : {"rail", "rail.demolish",
			"signal", "signal.demolish", "signal.spacing", "signal.direction",
			"waypoint.rail", "waypoint.rail.demolish", "waypoint.buoy", "waypoint.buoy.demolish",
			"railconvert", "roadconvert",
			"station.rail", "station.rail.demolish", "station.airport", "station.dock",
			"airport", "dock", "depot.rail", "depot.ship",
			"bridge", "aqueduct", "tunnel",
			"canal", "canal.demolish", "lock", "lock.demolish",
			"tree", "tree.demolish", "object", "object.demolish",
			"terraform", "tram.demolish"}) {
		CHECK(HasEntry(construct["implemented"], op));
	}

	/* The families this module leaves to the road/vehicle module are named as gaps. */
	for (const std::string_view op : {"road", "tram", "demolish",
			"station.road", "station.airport.demolish", "station.dock.demolish",
			"waypoint.road",
			"depot.road", "depot.tram", "depot.air",
			"bridge.demolish", "tunnel.demolish",
			"object.area"}) {
		CHECK(HasEntry(construct["gaps"], op));
	}

	/* A family is not advertised as implemented and as a gap at the same time. */
	for (const InGameAgent::Json &entry : construct["implemented"]) {
		CHECK_FALSE(HasEntry(construct["gaps"], entry.get<std::string>()));
	}

	CHECK(!caps["unimplemented_methods"].empty());
	CHECK(!caps["notes"].empty());
}

TEST_CASE("agent transport tools: unowned operations fall through")
{
	/* Methods this module does not implement at all. */
	for (const std::string_view method : {"world.query", "world.vehicles", "world.orders", "world.info", "world.capture", "task.barrier"}) {
		CHECK_FALSE(InGameAgent::DispatchTransportTool(method, InGameAgent::Json::object(), INVALID_COMPANY).has_value());
	}

	/* Ops the road/vehicle module owns. */
	CHECK_FALSE(Dispatch({{"op", "road"}}).has_value());
	CHECK_FALSE(Dispatch({{"op", "demolish"}}).has_value());
	CHECK_FALSE(Dispatch({{"op", "tram"}}).has_value());
	CHECK_FALSE(Dispatch({{"op", "station"}, {"stop_type", "bus"}}).has_value());
	CHECK_FALSE(Dispatch({{"op", "waypoint"}, {"waypoint_type", "road"}}).has_value());

	/* Road and tram depots stay with the road module. */
	CHECK_FALSE(Dispatch({{"op", "depot"}, {"road_type", "road"}}).has_value());
	CHECK_FALSE(Dispatch({{"op", "depot"}, {"depot_type", "road"}}).has_value());
	CHECK_FALSE(Dispatch({{"op", "depot"}, {"depot_type", "tram"}}).has_value());

	/* A depot call that names no kind, or an unknown one, is left to the road module, which owns
	 * the default and rejects what it does not know; this module never claims a road depot. */
	CHECK_FALSE(Dispatch({{"op", "depot"}}).has_value());
	CHECK_FALSE(Dispatch({{"op", "depot"}, {"tiles", OneTile()}}).has_value());
	CHECK_FALSE(Dispatch({{"op", "depot"}, {"depot_type", "monorail"}}).has_value());

	/* An unknown op is not this module's either. */
	CHECK_FALSE(Dispatch({{"op", "bogus"}}).has_value());
}

TEST_CASE("agent transport tools: argument validation")
{
	/* params must be an object. */
	auto result = InGameAgent::DispatchTransportTool("world.construct", InGameAgent::Json::array(), INVALID_COMPANY);
	REQUIRE(result.has_value());
	CHECK((*result)["ok"] == false);
	CHECK((*result)["error"] == "invalid_arguments");

	/* op is required. */
	result = Dispatch(InGameAgent::Json::object());
	REQUIRE(result.has_value());
	CHECK((*result)["error"] == "invalid_arguments");

	/* demolish must be a boolean. */
	result = Dispatch({{"op", "rail"}, {"demolish", "yes"}});
	REQUIRE(result.has_value());
	CHECK((*result)["error"] == "invalid_arguments");

	/* dry_run must be a boolean. */
	result = Dispatch({{"op", "rail"}, {"dry_run", 1}});
	REQUIRE(result.has_value());
	CHECK((*result)["error"] == "invalid_arguments");
}

TEST_CASE("agent transport tools: owned ops need a real company")
{
	/* Every op this module owns stops at the company check before it touches the world. */
	for (const std::string_view op : {"rail", "signal", "station", "airport", "dock", "depot", "bridge", "aqueduct",
			"tunnel", "canal", "terraform", "waypoint", "railconvert", "roadconvert", "lock", "tree", "object"}) {
		InGameAgent::Json params = InGameAgent::Json::object();
		params["op"] = op;
		if (op == "station") params["station_type"] = "rail";
		if (op == "depot") params["depot_type"] = "rail";
		const std::optional<InGameAgent::Json> result = Dispatch(params);
		REQUIRE(result.has_value());
		CHECK((*result)["ok"] == false);
		CHECK((*result)["error"] == "invalid_company");
	}

	/* A depot named through `transport_type` is this module's too, with `water` spelled `ship`. */
	for (const std::string_view kind : {"rail", "water", "air"}) {
		const std::optional<InGameAgent::Json> result = Dispatch({{"op", "depot"}, {"transport_type", std::string(kind)}});
		REQUIRE(result.has_value());
		CHECK((*result)["error"] == "invalid_company");
	}

	/* Tram removal is the one tram case this module owns. */
	const std::optional<InGameAgent::Json> tram = Dispatch({{"op", "tram"}, {"demolish", true}});
	REQUIRE(tram.has_value());
	CHECK((*tram)["error"] == "invalid_company");
}

TEST_CASE("agent transport tools: waypoints, conversions and the new tile ops validate first")
{
	FakeCompany company;
	Map::Allocate(64, 64);

	/* A malformed or unknown waypoint kind is rejected here, not silently turned into rail. */
	CheckError({{"op", "waypoint"}, {"waypoint_type", "bogus"}}, "invalid_arguments", "waypoint_type");
	CheckError({{"op", "waypoint"}, {"waypoint_type", 7}}, "invalid_arguments", "waypoint_type");

	/* A buoy is a water waypoint on exactly one tile. */
	CheckError({{"op", "waypoint"}, {"waypoint_type", "buoy"}}, "invalid_arguments", "buoy");
	CheckError({{"op", "waypoint"}, {"waypoint_type", "buoy"}, {"tiles", TwoTiles()}}, "invalid_arguments", "buoy");

	/* A rail waypoint sits on a straight track and spans 1 to 255 tiles. */
	CheckError({{"op", "waypoint"}, {"tiles", OneTile()}, {"track", "upper"}}, "invalid_arguments", "track");
	/* The axis of the waypoint is read before its length, so an explicit `track` is needed to reach
	 * the length and keep_rail checks on a tile that carries no straight rail. */
	CheckError({{"op", "waypoint"}, {"tiles", OneTile()}, {"track", "x"}, {"length", 300}}, "invalid_arguments", "length");
	CheckError({{"op", "waypoint"}, {"tiles", OneTile()}, {"track", "x"}, {"length", 0}}, "invalid_arguments", "length");
	CheckError({{"op", "waypoint"}, {"demolish", true}, {"tiles", OneTile()}, {"track", "x"}, {"keep_rail", 1}}, "invalid_arguments", "keep_rail");
	CheckError({{"op", "waypoint"}, {"from", {{"x", 1}, {"y", 1}}}, {"to", {{"x", 3}, {"y", 4}}}}, "invalid_arguments", "from/to");

	/* A conversion has to name the type it converts to. */
	CheckError({{"op", "railconvert"}}, "invalid_arguments", "rail_type");
	CheckError({{"op", "roadconvert"}}, "invalid_arguments", "road_type");
	CheckError({{"op", "roadconvert"}, {"road_type", "bogus"}}, "invalid_arguments", "road_type");

	/* A lock is one tile. */
	CheckError({{"op", "lock"}}, "invalid_arguments", "lock");
	CheckError({{"op", "lock"}, {"tiles", TwoTiles()}}, "invalid_arguments", "lock");

	/* A tree index is a byte; the engine then checks it against the climate. */
	CheckError({{"op", "tree"}, {"tree", 300}}, "invalid_arguments", "tree");
	CheckError({{"op", "tree"}, {"demolish", true}}, "invalid_arguments", "tree removal");
	CheckError({{"op", "tree"}, {"demolish", true}, {"from", {{"x", 1}, {"y", 1}}}, {"to", {{"x", 5}, {"y", 3}}}},
			"invalid_arguments", "tree removal");

	/* An object type has to be inside the object range before the spec is looked up. */
	CheckError({{"op", "object"}}, "invalid_arguments", "object_type");
	CheckError({{"op", "object"}, {"object_type", 99999}}, "invalid_arguments", "object_type");
	CheckError({{"op", "object"}, {"demolish", true}}, "invalid_arguments", "object removal");
	CheckError({{"op", "object"}, {"demolish", true}, {"tiles", InGameAgent::Json::array()}}, "invalid_arguments", "object removal");

	/* Signal spacing is the engine's own 1 to 20 tile density. */
	CheckError({{"op", "signal"}, {"track", "x"}, {"from", {{"x", 1}, {"y", 1}}}, {"to", {{"x", 9}, {"y", 1}}}, {"spacing", 40}},
			"invalid_arguments", "spacing");
	CheckError({{"op", "signal"}, {"track", "x"}, {"from", {{"x", 1}, {"y", 1}}}, {"to", {{"x", 9}, {"y", 1}}}, {"spacing", 0}},
			"invalid_arguments", "spacing");
	CheckError({{"op", "signal"}, {"track", "x"}, {"signal_direction", "sideways"}, {"tiles", OneTile()}},
			"invalid_arguments", "signal_direction");

	/* An aqueduct is a water bridge, so the transport type cannot be changed away from water. */
	CheckError({{"op", "aqueduct"}, {"transport_type", "rail"}, {"from", {{"x", 1}, {"y", 1}}}, {"to", {{"x", 5}, {"y", 1}}}},
			"invalid_arguments", "aqueduct");
	CheckError({{"op", "aqueduct"}, {"from", {{"x", 1}, {"y", 1}}}, {"to", {{"x", 5}, {"y", 1}}}}, "invalid_arguments", "bridge_type");

	/* Clearing water runs the landscape clear command per tile, so the run has to be straight. */
	CheckError({{"op", "canal"}, {"demolish", true}, {"from", {{"x", 1}, {"y", 1}}}, {"to", {{"x", 5}, {"y", 3}}}},
			"invalid_arguments", "canal needs");

	/* Removing part of a rail station keeps the station, so it takes a tile or a straight run. */
	CheckError({{"op", "station"}, {"station_type", "rail"}, {"demolish", true}}, "invalid_arguments", "station removal");
	CheckError({{"op", "station"}, {"station_type", "rail"}, {"demolish", true}, {"from", {{"x", 1}, {"y", 1}}}, {"to", {{"x", 5}, {"y", 3}}}},
			"invalid_arguments", "from/to");

	/* Ops with no removal command of their own say so instead of pretending to remove something. */
	CheckError({{"op", "airport"}, {"demolish", true}, {"tiles", OneTile()}}, "unsupported_op");
	CheckError({{"op", "dock"}, {"demolish", true}, {"tiles", OneTile()}}, "unsupported_op");
	CheckError({{"op", "tunnel"}, {"demolish", true}, {"tiles", OneTile()}}, "unsupported_op");
	CheckError({{"op", "bridge"}, {"demolish", true}}, "unsupported_op");
	CheckError({{"op", "terraform"}, {"demolish", true}}, "unsupported_op");
	CheckError({{"op", "railconvert"}, {"demolish", true}}, "unsupported_op");
	CheckError({{"op", "roadconvert"}, {"demolish", true}}, "unsupported_op");

	/* Only the rail part of a station can be removed; an airport or a dock is cleared whole. */
	CheckError({{"op", "station"}, {"station_type", "airport"}, {"demolish", true}, {"tiles", OneTile()}}, "unsupported_op");
	CheckError({{"op", "station"}, {"station_type", "dock"}, {"demolish", true}, {"tiles", OneTile()}}, "unsupported_op");
}
