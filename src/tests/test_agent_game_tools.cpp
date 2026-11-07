/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file test_agent_game_tools.cpp Test the dispatch, the argument validation and the reported gaps
 * of the agent game tools.
 *
 * No game is running in this test binary, so no real command is ever executed here: these cases
 * cover dispatch, the validation that runs before a command is posted, and gap reporting. The
 * command path itself (Command::Post, the created ids and the actual costs) needs a running game.
 */

#include "../stdafx.h"

#include "../3rdparty/catch2/catch.hpp"

#include "../agent_game_tools.h"
#include "../cargotype.h"

#include "../safeguards.h"

using namespace InGameAgent;

/**
 * Is a value present in a JSON array of strings?
 * @param array The array to search.
 * @param value The value to look for.
 * @return True when the array holds the value.
 */
static bool Contains(const Json &array, std::string_view value)
{
	for (const Json &item : array) {
		if (item.is_string() && item.get<std::string>() == value) return true;
	}
	return false;
}

/**
 * Find a capability entry by method name.
 * @param capabilities The capability document.
 * @param method The method to look for.
 * @return The entry, or an empty object when the method is not listed.
 */
static Json FindCapability(const Json &capabilities, std::string_view method)
{
	for (const Json &entry : capabilities["methods"]) {
		if (entry["method"].is_string() && entry["method"].get<std::string>() == method) return entry;
	}
	return Json::object();
}

/**
 * Build a `{x, y}` tile object.
 * @param x X coordinate.
 * @param y Y coordinate.
 * @return The tile object.
 */
static Json Tile(int x, int y)
{
	return Json{{"x", x}, {"y", y}};
}

TEST_CASE("agent_game_tools: capabilities list the implemented methods and their gaps")
{
	const Json capabilities = GameToolCapabilities();
	CHECK(capabilities["module"] == "agent_game_tools");
	REQUIRE(capabilities["methods"].is_array());
	CHECK(capabilities["unimplemented_methods"].is_array());

	const Json construct = FindCapability(capabilities, "world.construct");
	REQUIRE(!construct.empty());
	CHECK(construct["authority"] == "write");
	CHECK(Contains(construct["implemented"], "road"));
	CHECK(Contains(construct["implemented"], "station"));
	CHECK(Contains(construct["implemented"], "depot"));
	CHECK(Contains(construct["gaps"], "rail"));

	const Json vehicles = FindCapability(capabilities, "world.vehicles");
	REQUIRE(!vehicles.empty());
	CHECK(Contains(vehicles["implemented"], "build"));
	CHECK(Contains(vehicles["implemented"], "start"));
	CHECK(Contains(vehicles["implemented"], "stop"));
	CHECK(Contains(vehicles["implemented"], "refit"));
	CHECK(Contains(vehicles["implemented"], "clone"));
	CHECK(vehicles["gaps"].empty());

	const Json orders = FindCapability(capabilities, "world.orders");
	REQUIRE(!orders.empty());
	CHECK(Contains(orders["implemented"], "set"));
	CHECK(Contains(orders["implemented"], "append"));
	CHECK(Contains(orders["implemented"], "timetable"));
	CHECK(orders["gaps"].empty());

	const Json query = FindCapability(capabilities, "world.query");
	REQUIRE(!query.empty());
	CHECK(query["authority"] == "read");
	CHECK(Contains(query["implemented"], "vehicles"));
	CHECK(Contains(query["implemented"], "engines"));
	CHECK(Contains(query["implemented"], "orders"));
	CHECK(Contains(query["implemented"], "groups"));
	CHECK(Contains(query["implemented"], "cargos"));
	CHECK_FALSE(Contains(query["gaps"], "groups"));
	CHECK_FALSE(Contains(query["gaps"], "cargos"));
}

TEST_CASE("agent_game_tools: a method outside the game tools is not claimed")
{
	CHECK_FALSE(DispatchGameTool("world.unknown", Json::object(), CompanyID::Invalid()).has_value());
	CHECK_FALSE(DispatchGameTool("", Json::object(), CompanyID::Invalid()).has_value());
}

TEST_CASE("agent_game_tools: a method another module owns is not claimed")
{
	/* The bridge kernel, transport and admin own these; claiming them would shadow their work. */
	CHECK_FALSE(DispatchGameTool("world.info", Json::object(), CompanyID::Invalid()).has_value());
	CHECK_FALSE(DispatchGameTool("world.pause", Json::object(), CompanyID::Invalid()).has_value());
	CHECK_FALSE(DispatchGameTool("world.capture", Json::object(), CompanyID::Invalid()).has_value());
	CHECK_FALSE(DispatchGameTool("task.barrier", Json::object(), CompanyID::Invalid()).has_value());
}

TEST_CASE("agent_game_tools: params must be an object")
{
	const std::optional<Json> result = DispatchGameTool("world.query", Json::array(), CompanyID::Invalid());
	REQUIRE(result.has_value());
	CHECK((*result)["ok"] == false);
	CHECK((*result)["error"] == "invalid_arguments");
}

TEST_CASE("agent_game_tools: world.query needs a kind and leaves other kinds to their module")
{
	const std::optional<Json> missing = DispatchGameTool("world.query", Json::object(), CompanyID::Invalid());
	REQUIRE(missing.has_value());
	CHECK((*missing)["error"] == "invalid_arguments");

	CHECK_FALSE(DispatchGameTool("world.query", {{"kind", "towns"}}, CompanyID::Invalid()).has_value());
	CHECK_FALSE(DispatchGameTool("world.query", {{"kind", "stations"}}, CompanyID::Invalid()).has_value());
}

TEST_CASE("agent_game_tools: the vehicle and engine inventory is read-only and empty without a game")
{
	const std::optional<Json> vehicles = DispatchGameTool("world.query", {{"kind", "vehicles"}}, CompanyID::Invalid());
	REQUIRE(vehicles.has_value());
	CHECK((*vehicles)["ok"] == true);
	CHECK((*vehicles)["kind"] == "vehicles");
	CHECK((*vehicles)["rows"].is_array());
	CHECK((*vehicles)["total"] == 0);

	const std::optional<Json> engines = DispatchGameTool("world.query", {{"kind", "engines"}}, CompanyID::Invalid());
	REQUIRE(engines.has_value());
	CHECK((*engines)["ok"] == true);
	CHECK((*engines)["kind"] == "engines");
	CHECK((*engines)["rows"].is_array());

	const std::optional<Json> orders = DispatchGameTool("world.query", {{"kind", "orders"}}, CompanyID::Invalid());
	REQUIRE(orders.has_value());
	CHECK((*orders)["ok"] == true);
	CHECK((*orders)["kind"] == "orders");
	CHECK((*orders)["rows"].is_array());
	CHECK((*orders)["total"] == 0);
}

TEST_CASE("agent_game_tools: query filters are validated instead of silently ignored")
{
	const std::optional<Json> unknown_filter = DispatchGameTool("world.query", {{"kind", "vehicles"}, {"filters", {{"colour", "red"}}}}, CompanyID::Invalid());
	REQUIRE(unknown_filter.has_value());
	CHECK((*unknown_filter)["ok"] == false);
	CHECK((*unknown_filter)["error"] == "invalid_arguments");
	CHECK((*unknown_filter)["detail"]["filter"] == "colour");
	CHECK((*unknown_filter)["detail"]["kind"] == "vehicles");

	/* A filter that only another kind understands is an error, not a dropped filter. */
	const std::optional<Json> wrong_kind = DispatchGameTool("world.query", {{"kind", "engines"}, {"filters", {{"in_depot", true}}}}, CompanyID::Invalid());
	REQUIRE(wrong_kind.has_value());
	CHECK((*wrong_kind)["error"] == "invalid_arguments");
	CHECK((*wrong_kind)["detail"]["filter"] == "in_depot");

	const std::optional<Json> bad_type = DispatchGameTool("world.query", {{"kind", "vehicles"}, {"filters", {{"vehicle_type", "monorail"}}}}, CompanyID::Invalid());
	REQUIRE(bad_type.has_value());
	CHECK((*bad_type)["error"] == "invalid_arguments");

	const std::optional<Json> bad_cargo = DispatchGameTool("world.query", {{"kind", "vehicles"}, {"filters", {{"cargo_id", 99}}}}, CompanyID::Invalid());
	REQUIRE(bad_cargo.has_value());
	CHECK((*bad_cargo)["error"] == "invalid_arguments");
	CHECK((*bad_cargo)["detail"]["cargo_id"] == 99);

	const std::optional<Json> bad_bool = DispatchGameTool("world.query", {{"kind", "vehicles"}, {"filters", {{"stopped", "yes"}}}}, CompanyID::Invalid());
	REQUIRE(bad_bool.has_value());
	CHECK((*bad_bool)["error"] == "invalid_arguments");

	const std::optional<Json> bad_limit = DispatchGameTool("world.query", {{"kind", "vehicles"}, {"limit", "many"}}, CompanyID::Invalid());
	REQUIRE(bad_limit.has_value());
	CHECK((*bad_limit)["error"] == "invalid_arguments");
}

TEST_CASE("agent_game_tools: ops of the write families that another module owns are not claimed")
{
	CHECK_FALSE(DispatchGameTool("world.construct", {{"op", "rail"}}, CompanyID::Invalid()).has_value());
	CHECK_FALSE(DispatchGameTool("world.construct", {{"op", "signal"}}, CompanyID::Invalid()).has_value());
	CHECK_FALSE(DispatchGameTool("world.vehicles", {{"op", "autoreplace"}}, CompanyID::Invalid()).has_value());
	CHECK_FALSE(DispatchGameTool("world.orders", {{"op", "delete"}}, CompanyID::Invalid()).has_value());
}

TEST_CASE("agent_game_tools: a parameter the op does not read is rejected, not silently dropped")
{
	const std::optional<Json> refit = DispatchGameTool("world.vehicles", {{"op", "refit"}, {"vehicle_id", 0}, {"cargo_id", 0}, {"colour", "red"}}, CompanyID(3));
	REQUIRE(refit.has_value());
	CHECK((*refit)["ok"] == false);
	CHECK((*refit)["error"] == "invalid_arguments");
	CHECK((*refit)["detail"]["method"] == "world.vehicles");
	CHECK((*refit)["detail"]["op"] == "refit");
	CHECK((*refit)["detail"]["unknown"] == Json::array({"colour"}));
	CHECK(Contains((*refit)["detail"]["supported"], "cargo_id"));

	const std::optional<Json> timetable = DispatchGameTool("world.orders", {{"op", "timetable"}, {"vehicle_id", 0}, {"timetable", Json::array()}, {"orders", Json::array()}}, CompanyID(3));
	REQUIRE(timetable.has_value());
	CHECK((*timetable)["error"] == "invalid_arguments");
	CHECK((*timetable)["detail"]["unknown"] == Json::array({"orders"}));

	/* `expected_refs` belongs to the parent bridge and is accepted everywhere. */
	const std::optional<Json> expected_refs = DispatchGameTool("world.vehicles", {{"op", "refit"}, {"vehicle_id", 0}, {"cargo_id", 0}, {"expected_refs", Json::array()}}, CompanyID(3));
	REQUIRE(expected_refs.has_value());
	CHECK((*expected_refs)["error"] == "invalid_company");
}

TEST_CASE("agent_game_tools: a write method needs a valid company")
{
	const std::optional<Json> result = DispatchGameTool("world.construct", {{"op", "road"}, {"from", Tile(0, 0)}, {"to", Tile(0, 0)}}, CompanyID(3));
	REQUIRE(result.has_value());
	CHECK((*result)["ok"] == false);
	CHECK((*result)["error"] == "invalid_company");
	CHECK((*result)["detail"]["company_id"] == 3);

	/* A dry run still needs a company to estimate for. */
	const std::optional<Json> dry_run = DispatchGameTool("world.construct", {{"op", "road"}, {"from", Tile(0, 0)}, {"to", Tile(0, 0)}, {"dry_run", true}}, CompanyID(3));
	REQUIRE(dry_run.has_value());
	CHECK((*dry_run)["error"] == "invalid_company");
}

TEST_CASE("agent_game_tools: a station rectangle wider than 255 tiles is rejected, not truncated")
{
	const std::optional<Json> result = DispatchGameTool("world.construct", {{"op", "station"}, {"from", Tile(0, 0)}, {"to", Tile(300, 0)}}, CompanyID(3));
	REQUIRE(result.has_value());
	CHECK((*result)["ok"] == false);
	CHECK((*result)["error"] == "invalid_arguments");
	CHECK((*result)["detail"]["width"] == 301);
	CHECK((*result)["detail"]["max"] == 255);

	/* The span is checked before the company, so a bad rectangle never reaches a command. */
	const std::optional<Json> tall = DispatchGameTool("world.construct", {{"op", "station"}, {"from", Tile(0, 0)}, {"to", Tile(0, 256)}}, CompanyID(3));
	REQUIRE(tall.has_value());
	CHECK((*tall)["error"] == "invalid_arguments");
	CHECK((*tall)["detail"]["length"] == 257);
}

TEST_CASE("agent_game_tools: a multi-tile station or depot request is rejected instead of using tiles[0]")
{
	const Json tiles = Json::array({Tile(0, 0), Tile(1, 0)});

	const std::optional<Json> station = DispatchGameTool("world.construct", {{"op", "station"}, {"tiles", tiles}}, CompanyID(3));
	REQUIRE(station.has_value());
	CHECK((*station)["ok"] == false);
	CHECK((*station)["error"] == "unsupported_multi_item");
	CHECK((*station)["detail"]["count"] == 2);
	CHECK((*station)["detail"]["supported"] == 1);

	const std::optional<Json> depot = DispatchGameTool("world.construct", {{"op", "depot"}, {"tiles", tiles}}, CompanyID(3));
	REQUIRE(depot.has_value());
	CHECK((*depot)["error"] == "unsupported_multi_item");
	CHECK((*depot)["detail"]["count"] == 2);
}

TEST_CASE("agent_game_tools: road needs a usable tile list or a straight run")
{
	const std::optional<Json> nothing = DispatchGameTool("world.construct", {{"op", "road"}}, CompanyID(3));
	REQUIRE(nothing.has_value());
	CHECK((*nothing)["error"] == "invalid_arguments");

	const std::optional<Json> half_run = DispatchGameTool("world.construct", {{"op", "road"}, {"from", Tile(0, 0)}}, CompanyID(3));
	REQUIRE(half_run.has_value());
	CHECK((*half_run)["error"] == "invalid_arguments");

	const std::optional<Json> diagonal = DispatchGameTool("world.construct", {{"op", "road"}, {"from", Tile(0, 0)}, {"to", Tile(1, 1)}}, CompanyID(3));
	REQUIRE(diagonal.has_value());
	CHECK((*diagonal)["error"] == "invalid_arguments");

	const std::optional<Json> bad_tile = DispatchGameTool("world.construct", {{"op", "road"}, {"tiles", Json::array({"nope"})}}, CompanyID(3));
	REQUIRE(bad_tile.has_value());
	CHECK((*bad_tile)["error"] == "invalid_arguments");
}

TEST_CASE("agent_game_tools: optional parameters are validated instead of silently ignored")
{
	const Json tiles = Json::array({Tile(0, 0)});

	const std::optional<Json> dry_run = DispatchGameTool("world.construct", {{"op", "road"}, {"tiles", tiles}, {"dry_run", "yes"}}, CompanyID(3));
	REQUIRE(dry_run.has_value());
	CHECK((*dry_run)["error"] == "invalid_arguments");

	const std::optional<Json> road_type = DispatchGameTool("world.construct", {{"op", "road"}, {"tiles", tiles}, {"road_type", "monorail"}}, CompanyID(3));
	REQUIRE(road_type.has_value());
	CHECK((*road_type)["error"] == "invalid_arguments");

	const std::optional<Json> direction = DispatchGameTool("world.construct", {{"op", "depot"}, {"tiles", tiles}, {"direction", "up"}}, CompanyID(3));
	REQUIRE(direction.has_value());
	CHECK((*direction)["error"] == "invalid_arguments");

	const std::optional<Json> stop_type = DispatchGameTool("world.construct", {{"op", "station"}, {"tiles", tiles}, {"stop_type", "air"}}, CompanyID(3));
	REQUIRE(stop_type.has_value());
	CHECK((*stop_type)["error"] == "invalid_arguments");

	/* `demolish` is only honoured by op=demolish; elsewhere it is a gap, not a silent no-op. */
	const std::optional<Json> demolish = DispatchGameTool("world.construct", {{"op", "station"}, {"tiles", tiles}, {"demolish", true}}, CompanyID(3));
	REQUIRE(demolish.has_value());
	CHECK((*demolish)["error"] == "unsupported_op");
}

TEST_CASE("agent_game_tools: vehicle build parameters are validated before anything is created")
{
	const std::optional<Json> no_engine = DispatchGameTool("world.vehicles", {{"op", "build"}}, CompanyID(3));
	REQUIRE(no_engine.has_value());
	CHECK((*no_engine)["error"] == "invalid_arguments");

	const std::optional<Json> count = DispatchGameTool("world.vehicles", {{"op", "build"}, {"engine_id", 0}, {"depot_id", 0}, {"count", 101}}, CompanyID(3));
	REQUIRE(count.has_value());
	CHECK((*count)["error"] == "invalid_arguments");
	CHECK((*count)["detail"]["max"] == 100);

	const std::optional<Json> zero_count = DispatchGameTool("world.vehicles", {{"op", "build"}, {"engine_id", 0}, {"depot_id", 0}, {"count", 0}}, CompanyID(3));
	REQUIRE(zero_count.has_value());
	CHECK((*zero_count)["error"] == "invalid_arguments");

	const std::optional<Json> cargo = DispatchGameTool("world.vehicles", {{"op", "build"}, {"engine_id", 0}, {"depot_id", 0}, {"cargo_id", 99}}, CompanyID(3));
	REQUIRE(cargo.has_value());
	CHECK((*cargo)["error"] == "invalid_params");
	CHECK((*cargo)["detail"]["cargo_id"] == 99);

	const std::optional<Json> name = DispatchGameTool("world.vehicles", {{"op", "build"}, {"engine_id", 0}, {"depot_id", 0}, {"name", std::string(129, 'x')}}, CompanyID(3));
	REQUIRE(name.has_value());
	CHECK((*name)["error"] == "invalid_arguments");
	CHECK((*name)["detail"]["max"] == 128);

	/* `share_orders_from` is read, so a wrong type is an error and not an ignored parameter. */
	const std::optional<Json> share = DispatchGameTool("world.vehicles", {{"op", "build"}, {"engine_id", 0}, {"depot_id", 0}, {"share_orders_from", "x"}}, CompanyID(3));
	REQUIRE(share.has_value());
	CHECK((*share)["error"] == "invalid_params");
}

TEST_CASE("agent_game_tools: order lists are validated before the vehicle is touched")
{
	const std::optional<Json> missing = DispatchGameTool("world.orders", {{"op", "set"}, {"vehicle_id", 0}}, CompanyID(3));
	REQUIRE(missing.has_value());
	CHECK((*missing)["error"] == "invalid_arguments");

	const std::optional<Json> empty = DispatchGameTool("world.orders", {{"op", "set"}, {"vehicle_id", 0}, {"orders", Json::array()}}, CompanyID(3));
	REQUIRE(empty.has_value());
	CHECK((*empty)["error"] == "invalid_arguments");

	const std::optional<Json> type = DispatchGameTool("world.orders", {{"op", "set"}, {"vehicle_id", 0}, {"orders", Json::array({Json{{"type", "bogus"}, {"id", 1}}})}}, CompanyID(3));
	REQUIRE(type.has_value());
	CHECK((*type)["error"] == "invalid_arguments");

	const std::optional<Json> no_id = DispatchGameTool("world.orders", {{"op", "append"}, {"vehicle_id", 0}, {"orders", Json::array({Json{{"type", "station"}}})}}, CompanyID(3));
	REQUIRE(no_id.has_value());
	CHECK((*no_id)["error"] == "invalid_arguments");

	const std::optional<Json> refit = DispatchGameTool("world.orders", {{"op", "append"}, {"vehicle_id", 0}, {"orders", Json::array({Json{{"type", "station"}, {"id", 1}, {"refit_cargo_id", 99}}})}}, CompanyID(3));
	REQUIRE(refit.has_value());
	CHECK((*refit)["error"] == "invalid_params");

	const std::optional<Json> shared = DispatchGameTool("world.orders", {{"op", "shared"}, {"vehicle_id", 0}}, CompanyID(3));
	REQUIRE(shared.has_value());
	CHECK((*shared)["error"] == "invalid_arguments");

	const std::optional<Json> group = DispatchGameTool("world.orders", {{"op", "group"}, {"vehicle_id", 0}}, CompanyID(3));
	REQUIRE(group.has_value());
	CHECK((*group)["error"] == "invalid_arguments");
}

TEST_CASE("agent_game_tools: refit parameters are validated before anything is mutated")
{
	const std::optional<Json> no_vehicle = DispatchGameTool("world.vehicles", {{"op", "refit"}, {"cargo_id", 0}}, CompanyID(3));
	REQUIRE(no_vehicle.has_value());
	CHECK((*no_vehicle)["error"] == "invalid_arguments");

	const std::optional<Json> no_cargo = DispatchGameTool("world.vehicles", {{"op", "refit"}, {"vehicle_id", 0}}, CompanyID(3));
	REQUIRE(no_cargo.has_value());
	CHECK((*no_cargo)["error"] == "invalid_arguments");

	const std::optional<Json> bad_cargo = DispatchGameTool("world.vehicles", {{"op", "refit"}, {"vehicle_id", 0}, {"cargo_id", 99}}, CompanyID(3));
	REQUIRE(bad_cargo.has_value());
	CHECK((*bad_cargo)["error"] == "invalid_params");
	CHECK((*bad_cargo)["detail"]["cargo_id"] == 99);
	CHECK((*bad_cargo)["detail"]["max"] == 63);

	const std::optional<Json> bad_flag = DispatchGameTool("world.vehicles", {{"op", "refit"}, {"vehicle_id", 0}, {"cargo_id", 0}, {"only_this", "yes"}}, CompanyID(3));
	REQUIRE(bad_flag.has_value());
	CHECK((*bad_flag)["error"] == "invalid_arguments");

	/* A usable request reaches the company check, so refit is dispatched and not a gap. */
	const std::optional<Json> company = DispatchGameTool("world.vehicles", {{"op", "refit"}, {"vehicle_id", 0}, {"cargo_id", 0}}, CompanyID(3));
	REQUIRE(company.has_value());
	CHECK((*company)["error"] == "invalid_company");
}

TEST_CASE("agent_game_tools: clone parameters are validated before anything is created")
{
	const std::optional<Json> no_vehicle = DispatchGameTool("world.vehicles", {{"op", "clone"}}, CompanyID(3));
	REQUIRE(no_vehicle.has_value());
	CHECK((*no_vehicle)["error"] == "invalid_arguments");

	const std::optional<Json> count = DispatchGameTool("world.vehicles", {{"op", "clone"}, {"vehicle_id", 0}, {"count", 101}}, CompanyID(3));
	REQUIRE(count.has_value());
	CHECK((*count)["error"] == "invalid_arguments");
	CHECK((*count)["detail"]["max"] == 100);

	const std::optional<Json> zero_count = DispatchGameTool("world.vehicles", {{"op", "clone"}, {"vehicle_id", 0}, {"count", 0}}, CompanyID(3));
	REQUIRE(zero_count.has_value());
	CHECK((*zero_count)["error"] == "invalid_arguments");

	const std::optional<Json> share = DispatchGameTool("world.vehicles", {{"op", "clone"}, {"vehicle_id", 0}, {"share_orders", "yes"}}, CompanyID(3));
	REQUIRE(share.has_value());
	CHECK((*share)["error"] == "invalid_arguments");

	const std::optional<Json> company = DispatchGameTool("world.vehicles", {{"op", "clone"}, {"vehicle_id", 0}}, CompanyID(3));
	REQUIRE(company.has_value());
	CHECK((*company)["error"] == "invalid_company");
}

TEST_CASE("agent_game_tools: timetable entries are validated before the first command")
{
	const std::optional<Json> no_vehicle = DispatchGameTool("world.orders", {{"op", "timetable"}, {"timetable", Json::array({Json{{"index", 0}, {"wait_time", 10}}})}}, CompanyID(3));
	REQUIRE(no_vehicle.has_value());
	CHECK((*no_vehicle)["error"] == "invalid_arguments");

	const std::optional<Json> no_entries = DispatchGameTool("world.orders", {{"op", "timetable"}, {"vehicle_id", 0}}, CompanyID(3));
	REQUIRE(no_entries.has_value());
	CHECK((*no_entries)["error"] == "invalid_arguments");

	const std::optional<Json> empty = DispatchGameTool("world.orders", {{"op", "timetable"}, {"vehicle_id", 0}, {"timetable", Json::array()}}, CompanyID(3));
	REQUIRE(empty.has_value());
	CHECK((*empty)["error"] == "invalid_arguments");

	const std::optional<Json> not_object = DispatchGameTool("world.orders", {{"op", "timetable"}, {"vehicle_id", 0}, {"timetable", Json::array({1})}}, CompanyID(3));
	REQUIRE(not_object.has_value());
	CHECK((*not_object)["error"] == "invalid_arguments");

	const std::optional<Json> unknown_field = DispatchGameTool("world.orders", {{"op", "timetable"}, {"vehicle_id", 0}, {"timetable", Json::array({Json{{"index", 0}, {"wait_time", 10}, {"colour", "red"}}})}}, CompanyID(3));
	REQUIRE(unknown_field.has_value());
	CHECK((*unknown_field)["error"] == "invalid_arguments");
	CHECK((*unknown_field)["detail"]["unknown"] == Json::array({"colour"}));

	const std::optional<Json> no_field = DispatchGameTool("world.orders", {{"op", "timetable"}, {"vehicle_id", 0}, {"timetable", Json::array({Json{{"index", 0}}})}}, CompanyID(3));
	REQUIRE(no_field.has_value());
	CHECK((*no_field)["error"] == "invalid_arguments");

	const std::optional<Json> bad_value = DispatchGameTool("world.orders", {{"op", "timetable"}, {"vehicle_id", 0}, {"timetable", Json::array({Json{{"index", 0}, {"wait_time", 70000}}})}}, CompanyID(3));
	REQUIRE(bad_value.has_value());
	CHECK((*bad_value)["error"] == "invalid_arguments");
	CHECK((*bad_value)["detail"]["max"] == 65535);

	const std::optional<Json> company = DispatchGameTool("world.orders", {{"op", "timetable"}, {"vehicle_id", 0}, {"timetable", Json::array({Json{{"index", 0}, {"wait_time", 10}}})}}, CompanyID(3));
	REQUIRE(company.has_value());
	CHECK((*company)["error"] == "invalid_company");
}

TEST_CASE("agent_game_tools: world.query answers the canonical kinds and their singular aliases")
{
	const Json capabilities = GameToolCapabilities();
	REQUIRE(capabilities["query_aliases"].is_object());
	CHECK(capabilities["query_aliases"].size() == 10);
	CHECK(capabilities["query_aliases"]["vehicles"] == "vehicles");
	CHECK(capabilities["query_aliases"]["vehicle"] == "vehicles");
	CHECK(capabilities["query_aliases"]["engines"] == "engines");
	CHECK(capabilities["query_aliases"]["engine"] == "engines");
	CHECK(capabilities["query_aliases"]["orders"] == "orders");
	CHECK(capabilities["query_aliases"]["order"] == "orders");
	CHECK(capabilities["query_aliases"]["groups"] == "groups");
	CHECK(capabilities["query_aliases"]["group"] == "groups");
	CHECK(capabilities["query_aliases"]["cargos"] == "cargos");
	CHECK(capabilities["query_aliases"]["cargo"] == "cargos");

	for (const auto &[kind, canonical] : {std::pair{"vehicle", "vehicles"}, {"vehicles", "vehicles"},
			{"engine", "engines"}, {"engines", "engines"}, {"order", "orders"}, {"orders", "orders"},
			{"group", "groups"}, {"groups", "groups"}, {"cargo", "cargos"}, {"cargos", "cargos"}}) {
		CAPTURE(kind);
		const std::optional<Json> result = DispatchGameTool("world.query", {{"kind", kind}}, CompanyID::Invalid());
		REQUIRE(result.has_value());
		CHECK((*result)["ok"] == true);
		/* The result always names the canonical kind, whichever spelling the caller used. */
		CHECK((*result)["kind"] == canonical);
	}
}

TEST_CASE("agent_game_tools: a company id that does not fit is rejected instead of truncated")
{
	/* 300 truncated to a uint8_t would silently read company 44's rows. */
	const std::optional<Json> top_level = DispatchGameTool("world.query", {{"kind", "vehicles"}, {"company_id", 300}}, CompanyID::Invalid());
	REQUIRE(top_level.has_value());
	CHECK((*top_level)["ok"] == false);
	CHECK((*top_level)["error"] == "invalid_arguments");
	CHECK((*top_level)["detail"]["company_id"] == 300);
	CHECK((*top_level)["detail"]["max"] == 255);

	const std::optional<Json> filter = DispatchGameTool("world.query", {{"kind", "engines"}, {"filters", {{"company_id", 300}}}}, CompanyID::Invalid());
	REQUIRE(filter.has_value());
	CHECK((*filter)["error"] == "invalid_arguments");
	CHECK((*filter)["detail"]["company_id"] == 300);

	const std::optional<Json> negative = DispatchGameTool("world.query", {{"kind", "orders"}, {"filters", {{"company_id", -1}}}}, CompanyID::Invalid());
	REQUIRE(negative.has_value());
	CHECK((*negative)["error"] == "invalid_arguments");

	/* A company id that does fit is used as it is. */
	const std::optional<Json> valid = DispatchGameTool("world.query", {{"kind", "vehicles"}, {"company_id", 2}}, CompanyID::Invalid());
	REQUIRE(valid.has_value());
	CHECK((*valid)["ok"] == true);
	CHECK((*valid)["total"] == 0);
}

TEST_CASE("agent_game_tools: every filter a kind documents is read without an error")
{
	const std::optional<Json> vehicles = DispatchGameTool("world.query", {{"kind", "vehicles"}, {"filters", {
			{"company_id", 0}, {"vehicle_type", "road"}, {"vehicle_id", 0}, {"group_id", 0}, {"cargo_id", 0},
			{"name_contains", "bus"}, {"in_depot", true}, {"stopped", false}}}}, CompanyID::Invalid());
	REQUIRE(vehicles.has_value());
	CHECK((*vehicles)["ok"] == true);

	const std::optional<Json> engines = DispatchGameTool("world.query", {{"kind", "engines"}, {"filters", {
			{"company_id", 0}, {"vehicle_type", "train"}, {"engine_id", 0}, {"cargo_id", 0},
			{"name_contains", "loco"}, {"refittable", true}}}}, CompanyID::Invalid());
	REQUIRE(engines.has_value());
	CHECK((*engines)["ok"] == true);

	const std::optional<Json> orders = DispatchGameTool("world.query", {{"kind", "orders"}, {"filters", {
			{"company_id", 0}, {"vehicle_id", 0}}}}, CompanyID::Invalid());
	REQUIRE(orders.has_value());
	CHECK((*orders)["ok"] == true);
}

TEST_CASE("agent_game_tools: a world.query parameter the kind does not read is rejected")
{
	const std::optional<Json> unknown = DispatchGameTool("world.query", {{"kind", "vehicles"}, {"colour", "red"}}, CompanyID::Invalid());
	REQUIRE(unknown.has_value());
	CHECK((*unknown)["ok"] == false);
	CHECK((*unknown)["error"] == "invalid_arguments");
	CHECK((*unknown)["detail"]["method"] == "world.query");
	CHECK((*unknown)["detail"]["op"] == "vehicles");
	CHECK((*unknown)["detail"]["unknown"] == Json::array({"colour"}));

	/* `limit`, `offset`, `filters` and `company_id` are read, so they are accepted. */
	const std::optional<Json> known = DispatchGameTool("world.query", {{"kind", "orders"}, {"limit", 5}, {"offset", 0}, {"company_id", 1}, {"filters", {{"vehicle_id", 0}}}}, CompanyID::Invalid());
	REQUIRE(known.has_value());
	CHECK((*known)["ok"] == true);
}

TEST_CASE("agent_game_tools: the page of a query is bounded and the applied page is reported")
{
	/* A caller cannot ask for the whole world in one answer. */
	const std::optional<Json> clamped = DispatchGameTool("world.query", {{"kind", "vehicles"}, {"limit", 1000}, {"offset", -5}}, CompanyID::Invalid());
	REQUIRE(clamped.has_value());
	CHECK((*clamped)["ok"] == true);
	CHECK((*clamped)["limit"] == 500);
	CHECK((*clamped)["offset"] == 0);
	CHECK((*clamped)["total"] == 0);
	CHECK((*clamped)["has_more"] == false);

	const std::optional<Json> smallest = DispatchGameTool("world.query", {{"kind", "engines"}, {"limit", 0}}, CompanyID::Invalid());
	REQUIRE(smallest.has_value());
	CHECK((*smallest)["limit"] == 1);

	const std::optional<Json> defaulted = DispatchGameTool("world.query", {{"kind", "groups"}}, CompanyID::Invalid());
	REQUIRE(defaulted.has_value());
	CHECK((*defaulted)["limit"] == 100);
	CHECK((*defaulted)["offset"] == 0);
}

TEST_CASE("agent_game_tools: the ownership scope of a query is explicit")
{
	/* No company named and no company of the call: every owner, and the answer says so. */
	const std::optional<Json> all = DispatchGameTool("world.query", {{"kind", "vehicles"}}, CompanyID::Invalid());
	REQUIRE(all.has_value());
	CHECK((*all)["scope"]["company_id"].is_null());

	/* The company of the call scopes a company scoped kind, and the scope is reported. */
	const std::optional<Json> scoped = DispatchGameTool("world.query", {{"kind", "vehicles"}}, CompanyID(2));
	REQUIRE(scoped.has_value());
	CHECK((*scoped)["scope"]["company_id"] == 2);

	/* `company_id: null` asks for every owner even when the call names a company. */
	const std::optional<Json> unscoped = DispatchGameTool("world.query", {{"kind", "vehicles"}, {"company_id", nullptr}}, CompanyID(2));
	REQUIRE(unscoped.has_value());
	CHECK((*unscoped)["scope"]["company_id"].is_null());

	/* An explicit company wins over the company of the call, in the filter and at the top level. */
	const std::optional<Json> filtered = DispatchGameTool("world.query", {{"kind", "vehicles"}, {"filters", {{"company_id", 1}}}}, CompanyID(2));
	REQUIRE(filtered.has_value());
	CHECK((*filtered)["scope"]["company_id"] == 1);

	/* A global kind is never scoped to a company, even when the call names one. */
	const std::optional<Json> cargos = DispatchGameTool("world.query", {{"kind", "cargos"}}, CompanyID(2));
	REQUIRE(cargos.has_value());
	CHECK((*cargos)["scope"]["company_id"].is_null());
}

TEST_CASE("agent_game_tools: group and cargo filters are validated instead of ignored")
{
	const std::optional<Json> unknown = DispatchGameTool("world.query", {{"kind", "groups"}, {"filters", {{"in_depot", true}}}}, CompanyID::Invalid());
	REQUIRE(unknown.has_value());
	CHECK((*unknown)["error"] == "invalid_arguments");
	CHECK((*unknown)["detail"]["kind"] == "groups");
	CHECK((*unknown)["detail"]["filter"] == "in_depot");

	/* A group id outside the group pool is rejected instead of wrapping onto another group. */
	const std::optional<Json> bad_group = DispatchGameTool("world.query", {{"kind", "groups"}, {"filters", {{"group_id", 0xFF000}}}}, CompanyID::Invalid());
	REQUIRE(bad_group.has_value());
	CHECK((*bad_group)["error"] == "invalid_arguments");
	CHECK((*bad_group)["detail"]["max"] == 63999);

	const std::optional<Json> bad_protection = DispatchGameTool("world.query", {{"kind", "groups"}, {"filters", {{"replace_protection", "yes"}}}}, CompanyID::Invalid());
	REQUIRE(bad_protection.has_value());
	CHECK((*bad_protection)["error"] == "invalid_arguments");

	const std::optional<Json> bad_label = DispatchGameTool("world.query", {{"kind", "cargos"}, {"filters", {{"label", "PASSENGER"}}}}, CompanyID::Invalid());
	REQUIRE(bad_label.has_value());
	CHECK((*bad_label)["error"] == "invalid_arguments");

	const std::optional<Json> bad_freight = DispatchGameTool("world.query", {{"kind", "cargos"}, {"filters", {{"is_freight", 1}}}}, CompanyID::Invalid());
	REQUIRE(bad_freight.has_value());
	CHECK((*bad_freight)["error"] == "invalid_arguments");

	/* An unbounded name filter is rejected, so a query cannot scan for an arbitrary pattern. */
	const std::optional<Json> long_name = DispatchGameTool("world.query", {{"kind", "vehicles"}, {"filters", {{"name_contains", std::string(65, 'a')}}}}, CompanyID::Invalid());
	REQUIRE(long_name.has_value());
	CHECK((*long_name)["error"] == "invalid_arguments");
	CHECK((*long_name)["detail"]["max_length"] == 64);
}

TEST_CASE("agent_game_tools: vehicle ids are read at their real width")
{
	/* 70000 is a valid vehicle id; a uint16_t conversion would silently turn it into another id. */
	const std::optional<Json> wide = DispatchGameTool("world.query", {{"kind", "vehicles"}, {"filters", {{"vehicle_id", 70000}}}}, CompanyID::Invalid());
	REQUIRE(wide.has_value());
	CHECK((*wide)["ok"] == true);

	const std::optional<Json> outside = DispatchGameTool("world.query", {{"kind", "vehicles"}, {"filters", {{"vehicle_id", 0xFF000}}}}, CompanyID::Invalid());
	REQUIRE(outside.has_value());
	CHECK((*outside)["error"] == "invalid_arguments");
	CHECK((*outside)["detail"]["max"] == 0xFEFFF);
}

TEST_CASE("agent_game_tools: the cargo inventory reads the real cargo table when it is initialised")
{
	const std::optional<Json> result = DispatchGameTool("world.query", {{"kind", "cargos"}}, CompanyID::Invalid());
	REQUIRE(result.has_value());
	CHECK((*result)["ok"] == true);
	CHECK((*result)["kind"] == "cargos");
	REQUIRE((*result)["rows"].is_array());

	if (!CargoSpec::Get(0)->IsValid()) {
		/* No game initialised the cargo table in this binary; the rows can only be checked in a
		 * real game, because the cargo names come from the language pack the game loads. */
		WARN("the cargo table is not initialised in this test binary");
		CHECK((*result)["total"] == 0);
		return;
	}

	REQUIRE((*result)["total"] > 0);
	const Json &first = (*result)["rows"].front();
	REQUIRE(first["label"].is_string());
	CHECK(first["label"].get<std::string>().size() == 4);
	REQUIRE(first["name"].is_string());
	CHECK_FALSE(first["name"].get<std::string>().empty());
	CHECK(first["is_freight"].is_boolean());

	/* The label filter selects exactly the cargo the first row described. */
	const std::string label = first["label"].get<std::string>();
	const std::optional<Json> by_label = DispatchGameTool("world.query", {{"kind", "cargos"}, {"filters", {{"label", label}}}}, CompanyID::Invalid());
	REQUIRE(by_label.has_value());
	CHECK((*by_label)["ok"] == true);
	REQUIRE((*by_label)["total"] == 1);
	CHECK((*by_label)["rows"].front()["label"] == label);
	CHECK((*by_label)["rows"].front()["name"] == first["name"]);
}

/**
 * Dispatch a write op with one replaced parameter value and return the result.
 * @param method Engine method name.
 * @param op Op of the method.
 * @param base Parameters of a usable request.
 * @param key Parameter to replace.
 * @param value Value to use.
 * @return The tool result.
 */
static Json WriteWith(const char *method, const char *op, Json base, const char *key, Json value)
{
	base["op"] = op;
	base[key] = std::move(value);
	const std::optional<Json> result = DispatchGameTool(method, base, CompanyID(3));
	REQUIRE(result.has_value());
	return *result;
}

TEST_CASE("agent_game_tools: a write id is checked against its pool before the cast")
{
	/* The largest valid vehicle id is accepted and reaches the company check. */
	const Json max_vehicle = WriteWith("world.vehicles", "start", {{"vehicle_id", 0}}, "vehicle_id", 0xFEFFF);
	CHECK(max_vehicle["error"] == "invalid_company");

	/* One past the last vehicle id would wrap onto another vehicle. */
	const Json over_vehicle = WriteWith("world.vehicles", "start", {{"vehicle_id", 0}}, "vehicle_id", 0xFF000);
	CHECK(over_vehicle["error"] == "invalid_params");
	CHECK(over_vehicle["detail"]["vehicle_id"] == 0xFF000);
	CHECK(over_vehicle["detail"]["max"] == 0xFEFFF);

	/* 2^32 + a real id used to become that real id after a 32 bit cast. */
	const Json wide_vehicle = WriteWith("world.vehicles", "start", {{"vehicle_id", 0}}, "vehicle_id", int64_t{4294967296} + 3);
	CHECK(wide_vehicle["error"] == "invalid_params");
	CHECK(wide_vehicle["detail"]["vehicle_id"] == int64_t{4294967296} + 3);

	const Json negative_vehicle = WriteWith("world.vehicles", "start", {{"vehicle_id", 0}}, "vehicle_id", -1);
	CHECK(negative_vehicle["error"] == "invalid_params");
	CHECK(negative_vehicle["detail"]["max"] == 0xFEFFF);

	/* A float and a boolean are not ids, they must not become 1 or 0. */
	const Json float_vehicle = WriteWith("world.vehicles", "start", {{"vehicle_id", 0}}, "vehicle_id", 1.5);
	CHECK(float_vehicle["error"] == "invalid_params");

	const Json bool_vehicle = WriteWith("world.vehicles", "start", {{"vehicle_id", 0}}, "vehicle_id", true);
	CHECK(bool_vehicle["error"] == "invalid_params");

	/* The same rule for every other pool a write op names. */
	const Json max_engine = WriteWith("world.vehicles", "build", {{"engine_id", 0}, {"depot_id", 0}}, "engine_id", 63999);
	CHECK(max_engine["error"] == "invalid_company");

	const Json over_engine = WriteWith("world.vehicles", "build", {{"engine_id", 0}, {"depot_id", 0}}, "engine_id", 64000);
	CHECK(over_engine["error"] == "invalid_params");
	CHECK(over_engine["detail"]["max"] == 63999);

	const Json over_depot = WriteWith("world.vehicles", "build", {{"engine_id", 0}, {"depot_id", 0}}, "depot_id", 64000);
	CHECK(over_depot["error"] == "invalid_params");
	CHECK(over_depot["detail"]["max"] == 63999);

	const Json over_share = WriteWith("world.vehicles", "build", {{"engine_id", 0}, {"depot_id", 0}}, "share_orders_from", 0xFF000);
	CHECK(over_share["error"] == "invalid_params");

	const Json over_sell_depot = WriteWith("world.vehicles", "sell", {{"vehicle_id", 0}}, "depot_id", 64000);
	CHECK(over_sell_depot["error"] == "invalid_params");

	const Json over_clone_depot = WriteWith("world.vehicles", "clone", {{"vehicle_id", 0}}, "depot_id", 64000);
	CHECK(over_clone_depot["error"] == "invalid_params");

	const Json max_group = WriteWith("world.orders", "group", {{"vehicle_id", 0}}, "group_id", 63999);
	CHECK(max_group["error"] == "invalid_company");

	const Json over_group = WriteWith("world.orders", "group", {{"vehicle_id", 0}}, "group_id", 64000);
	CHECK(over_group["error"] == "invalid_params");
	CHECK(over_group["detail"]["max"] == 63999);

	const Json over_source = WriteWith("world.orders", "shared", {{"vehicle_id", 0}, {"share_orders_from", 0}}, "share_orders_from", 0xFF000);
	CHECK(over_source["error"] == "invalid_params");
	CHECK(over_source["detail"]["max"] == 0xFEFFF);
}

TEST_CASE("agent_game_tools: order ids and subtypes are checked against their pools before the cast")
{
	/* A station id and a depot id share the 16 bit station pool; 0xFA00 used to become station 0. */
	const Json over_station = WriteWith("world.orders", "set", {{"vehicle_id", 0}, {"orders", Json::array()}}, "orders",
			Json::array({Json{{"type", "station"}, {"id", 64000}}}));
	CHECK(over_station["error"] == "invalid_params");
	CHECK(over_station["detail"]["max"] == 63999);

	const Json over_waypoint = WriteWith("world.orders", "set", {{"vehicle_id", 0}, {"orders", Json::array()}}, "orders",
			Json::array({Json{{"type", "waypoint"}, {"id", 64000}}}));
	CHECK(over_waypoint["error"] == "invalid_params");

	const Json over_order_depot = WriteWith("world.orders", "set", {{"vehicle_id", 0}, {"orders", Json::array()}}, "orders",
			Json::array({Json{{"type", "depot"}, {"id", 64000}}}));
	CHECK(over_order_depot["error"] == "invalid_params");

	const Json negative_station = WriteWith("world.orders", "set", {{"vehicle_id", 0}, {"orders", Json::array()}}, "orders",
			Json::array({Json{{"type", "station"}, {"id", -1}}}));
	CHECK(negative_station["error"] == "invalid_params");

	const Json float_station = WriteWith("world.orders", "set", {{"vehicle_id", 0}, {"orders", Json::array()}}, "orders",
			Json::array({Json{{"type", "station"}, {"id", 1.5}}}));
	CHECK(float_station["error"] == "invalid_params");

	const Json bool_station = WriteWith("world.orders", "set", {{"vehicle_id", 0}, {"orders", Json::array()}}, "orders",
			Json::array({Json{{"type", "station"}, {"id", true}}}));
	CHECK(bool_station["error"] == "invalid_params");

	/* The last valid station id is accepted and reaches the company check. */
	const Json max_station = WriteWith("world.orders", "set", {{"vehicle_id", 0}, {"orders", Json::array()}}, "orders",
			Json::array({Json{{"type", "station"}, {"id", 63999}}}));
	CHECK(max_station["error"] == "invalid_company");

	/* The cargo subtype of an order is bounded by the cargo table, not by 8 bits. */
	const Json over_order_cargo = WriteWith("world.orders", "set", {{"vehicle_id", 0}, {"orders", Json::array()}}, "orders",
			Json::array({Json{{"type", "station"}, {"id", 0}, {"refit_cargo_id", 64}}}));
	CHECK(over_order_cargo["error"] == "invalid_params");
	CHECK(over_order_cargo["detail"]["max"] == 63);

	/* A timetable index is a VehicleOrderID: 0xFF is the invalid marker, so 0xFE is the last one. */
	const Json max_index = WriteWith("world.orders", "timetable", {{"vehicle_id", 0}, {"timetable", Json::array()}}, "timetable",
			Json::array({Json{{"index", 0xFE}, {"wait_time", 10}}}));
	CHECK(max_index["error"] == "invalid_company");

	const Json over_index = WriteWith("world.orders", "timetable", {{"vehicle_id", 0}, {"timetable", Json::array()}}, "timetable",
			Json::array({Json{{"index", 0xFF}, {"wait_time", 10}}}));
	CHECK(over_index["error"] == "invalid_params");
	CHECK(over_index["detail"]["max"] == 0xFE);

	/* A value that used to be truncated to 16 bits is rejected, not wrapped. */
	const Json over_wait = WriteWith("world.orders", "timetable", {{"vehicle_id", 0}, {"timetable", Json::array()}}, "timetable",
			Json::array({Json{{"index", 0}, {"wait_time", 65536}}}));
	CHECK(over_wait["error"] == "invalid_arguments");
	CHECK(over_wait["detail"]["max"] == 65535);
}
