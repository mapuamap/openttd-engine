/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/gpl-2.0>.
 */

/** @file test_agent_command_catalog.cpp Test the engine command catalog and the generic argument decoding. */

#include "../stdafx.h"

#include "../3rdparty/catch2/catch.hpp"

#include "../agent_command_catalog.h"
#include "../command_func.h"
#include "../command_type.h"
#include "../company_type.h"

#include "../safeguards.h"

using namespace InGameAgent;

/** Build a JSON array of command arguments, the way the backend sends them. */
template <typename... Ts>
static Json Arguments(Ts... values)
{
	Json args = Json::array();
	(args.push_back(values), ...);
	return args;
}

/** Read the catalog as the bridge would, for a read that needs no company. */
static Json ReadCatalog(const Json &params)
{
	const std::optional<Json> result = DispatchCommandCatalog("world.commands", params, CompanyID::Invalid());
	REQUIRE(result.has_value());
	return *result;
}

/** Call world.command, which validates the arguments before it needs a company. */
static Json CallCommand(const Json &params)
{
	const std::optional<Json> result = DispatchCommandCatalog("world.command", params, CompanyID::Invalid());
	REQUIRE(result.has_value());
	return *result;
}

/** One {entity, id} expected ref, the way the bridge sends them. */
static Json Ref(const char *entity, uint64_t id)
{
	return Json{{"entity", entity}, {"id", id}};
}

/** Find a catalog entry by command name. */
static const Json &FindEntry(const Json &commands, std::string_view name)
{
	for (const Json &entry : commands) {
		if (entry["name"].get<std::string>() == name) return entry;
	}
	FAIL("command " << name << " is missing from the catalog");
	return commands[0];
}

TEST_CASE("AgentCommandCatalogCoversEveryCommand")
{
	const Json catalog = ReadCatalog({{"op", "list"}});
	REQUIRE(catalog["ok"].get<bool>());
	CHECK(catalog["total"].get<size_t>() == to_underlying(Commands::End));
	CHECK(catalog["count"].get<size_t>() == to_underlying(Commands::End));

	/* Every engine command appears exactly once, with its own id, name and alias. */
	const Json &commands = catalog["commands"];
	REQUIRE(commands.size() == to_underlying(Commands::End));
	for (size_t i = 0; i < commands.size(); i++) {
		const Json &entry = commands[i];
		CHECK(entry["id"].get<size_t>() == i);
		CHECK_FALSE(entry["name"].get<std::string>().empty());
		CHECK_FALSE(entry["alias"].get<std::string>().empty());
		CHECK(entry.contains("flags"));
		CHECK(entry.contains("pause"));
		CHECK(entry.contains("company"));
		CHECK(entry["support"].contains("status"));
		/* The name the catalog reports is the engine's own name for that command. */
		CHECK(entry["name"].get<std::string>() == std::string(GetCommandName(static_cast<Commands>(i))));
	}

	/* The catalog must not claim more dispatchable commands than the engine has. */
	const size_t dispatchable = catalog["dispatchable"].get<size_t>();
	CHECK(dispatchable <= to_underlying(Commands::End));
	/* The generic decoder has to cover the plain commands, not just a handful. */
	CHECK(dispatchable > 100);
}

TEST_CASE("AgentCommandCatalogDescribesArguments")
{
	const Json catalog = ReadCatalog({{"op", "describe"}});
	const Json &commands = catalog["commands"];
	REQUIRE(commands.size() == to_underlying(Commands::End));

	/* A command whose single argument is a Money is decoded as a strong id. */
	const Json &cheat = FindEntry(commands, "CmdMoneyCheat");
	REQUIRE(cheat["arguments"].size() == 1);
	CHECK(cheat["arguments"][0]["type"].get<std::string>() == "Money");
	CHECK(cheat["arguments"][0]["kind"].get<std::string>() == "strong_id");
	CHECK(cheat["arguments"][0]["supported"].get<bool>());
	CHECK(cheat["support"]["status"].get<std::string>() == "dispatchable");
	CHECK(cheat["returns"].empty());

	/* A tile command keeps the engine's argument order and types. */
	const Json &road = FindEntry(commands, "CmdBuildRoad");
	REQUIRE(road["arguments"].size() == 5);
	CHECK(road["arguments"][0]["type"].get<std::string>() == "TileIndex");
	CHECK(road["arguments"][1]["kind"].get<std::string>() == "enum_bit_set");
	CHECK(road["arguments"][1]["type"].get<std::string>() == "EnumBitSet<RoadBit>");
	CHECK(road["arguments"][4]["type"].get<std::string>() == "TownID");
	CHECK_FALSE(road["location_argument"].get<bool>());
	CHECK(road["flags"].size() > 0);

	/* A command that takes its location outside its arguments says so. */
	const Json &town_action = FindEntry(commands, "CmdDoTownAction");
	CHECK(town_action["location_argument"].get<bool>());
	CHECK(town_action["type"].get<std::string>() == "LandscapeConstruction");

	/* Commands whose arguments are engine structs are catalogued as an explicit gap. */
	const Json &blueprint = FindEntry(commands, "CmdPasteBlueprint");
	CHECK_FALSE(blueprint["support"]["supported"].get<bool>());
	CHECK(blueprint["support"]["status"].get<std::string>() == "gap");
	CHECK(blueprint["support"]["reason"].get<std::string>().find("BlueprintChunkData") != std::string::npos);
	CHECK_FALSE(blueprint["arguments"][1]["supported"].get<bool>());

	/* Extra return values of a proc are reported, they are just not captured by the common callback. */
	const Json &build_vehicle = FindEntry(commands, "CmdBuildVehicle");
	REQUIRE(build_vehicle["returns"].size() == 4);
	CHECK(build_vehicle["returns"][0].get<std::string>() == "VehicleID");
}

TEST_CASE("AgentCommandCatalogDescribesOneCommand")
{
	const Json single = ReadCatalog({{"op", "describe"}, {"command", "CmdIncreaseLoan"}});
	REQUIRE(single["count"].get<size_t>() == 1);
	const Json &entry = single["commands"][0];
	CHECK(entry["name"].get<std::string>() == "CmdIncreaseLoan");
	CHECK(entry["alias"].get<std::string>() == "IncreaseLoan");
	CHECK(entry["type"].get<std::string>() == "MoneyManagement");
	REQUIRE(entry["arguments"].size() == 2);

	/* The alias and the numeric id name the same command. */
	const Json by_alias = ReadCatalog({{"op", "describe"}, {"command", "IncreaseLoan"}});
	CHECK(by_alias["commands"][0]["id"] == entry["id"]);
	const Json by_id = ReadCatalog({{"op", "describe"}, {"command", entry["id"].get<size_t>()}});
	CHECK(by_id["commands"][0]["name"] == entry["name"]);

	CHECK_FALSE(ReadCatalog({{"op", "describe"}, {"command", "CmdNoSuchCommand"}})["ok"].get<bool>());
	CHECK_FALSE(ReadCatalog({{"op", "nonsense"}})["ok"].get<bool>());
}

/** Call the read-only argument decoder of one command. */
static Json ValidateCommand(const char *command, const Json &args)
{
	return ReadCatalog({{"op", "validate"}, {"command", command}, {"args", args}});
}

TEST_CASE("AgentCommandCatalogDecodesArguments")
{
	/* Money is decoded through its base type, and reported back as the integer it holds. */
	const Json money = ValidateCommand("CmdMoneyCheat", Arguments(100));
	REQUIRE(money["ok"].get<bool>());
	CHECK(money["command"].get<std::string>() == "CmdMoneyCheat");
	CHECK(money["arguments"][0] == 100);

	/* A tile command: strong id, bitset, enum and another strong id, in the engine's order. */
	const Json road = ValidateCommand("CmdBuildRoad", Arguments(1000, 1, 0, 0, 0));
	REQUIRE(road["ok"].get<bool>());
	CHECK(road["arguments"].size() == 5);
	CHECK(road["arguments"][0] == 1000);
	CHECK(road["arguments"][1] == 1);

	/* A bitset can also be given as the bit indices of the enum values it holds. */
	const Json road_bits = ValidateCommand("CmdBuildRoad", Arguments(1000, Arguments(0, 1), 0, 0, 0));
	REQUIRE(road_bits["ok"].get<bool>());
	CHECK(road_bits["arguments"][1] == 3);

	/* Strings, and the alias of a command name. */
	const Json rename = ReadCatalog({{"op", "validate"}, {"command", "RenameCompany"}, {"args", Arguments("Test Co")}});
	REQUIRE(rename["ok"].get<bool>());
	CHECK(rename["arguments"][0].get<std::string>() == "Test Co");

	/* An enum and a boolean. */
	const Json pause = ValidateCommand("CmdPause", Arguments(1, true));
	REQUIRE(pause["ok"].get<bool>());
	CHECK(pause["arguments"][0] == 1);
	CHECK(pause["arguments"][1] == true);

	/* A command whose arguments are an engine struct is an explicit gap, not a silent success. */
	const Json blueprint = ValidateCommand("CmdPasteBlueprint", Arguments(1000, 0));
	CHECK_FALSE(blueprint["ok"].get<bool>());
	CHECK(blueprint["error"].get<std::string>() == "unsupported_argument_types");

	CHECK(ValidateCommand("CmdNoSuchCommand", Arguments())["error"].get<std::string>() == "unknown_command");
	CHECK(ReadCatalog({{"op", "validate"}})["error"].get<std::string>() == "invalid_params");
	CHECK(ReadCatalog({{"op", "validate"}, {"command", "CmdMoneyCheat"}, {"args", 5}})["error"].get<std::string>() == "invalid_params");
}

TEST_CASE("AgentCommandCatalogRejectsBadArgumentsBeforePosting")
{
	/* Wrong number of arguments. */
	const Json arity = ValidateCommand("CmdMoneyCheat", Arguments());
	CHECK(arity["error"].get<std::string>() == "invalid_arguments");
	CHECK(arity["message"].get<std::string>().find("argument(s)") != std::string::npos);

	/* Wrong type: a string where an enum is expected. */
	const Json wrong_type = ValidateCommand("CmdPause", Arguments("not-a-mode", true));
	CHECK(wrong_type["error"].get<std::string>() == "invalid_arguments");
	CHECK(wrong_type["message"].get<std::string>().find("argument 0") != std::string::npos);

	/* Wrong type: a number where a boolean is expected. */
	const Json wrong_bool = ValidateCommand("CmdPause", Arguments(1, 1));
	CHECK(wrong_bool["error"].get<std::string>() == "invalid_arguments");

	/* Overflow: the value does not fit the argument type. */
	const Json overflow = ValidateCommand("CmdSetCompanyManagerFace", Arguments(5000000000ULL, 0));
	CHECK(overflow["error"].get<std::string>() == "invalid_arguments");
	CHECK(overflow["message"].get<std::string>().find("does not fit") != std::string::npos);

	/* Overflow of a bitset: RoadBits is stored in one byte. */
	const Json bitset_overflow = ValidateCommand("CmdBuildRoad", Arguments(1000, 256, 0, 0, 0));
	CHECK(bitset_overflow["error"].get<std::string>() == "invalid_arguments");
	CHECK(bitset_overflow["message"].get<std::string>().find("argument 1") != std::string::npos);

	/* A bit index outside the enum of the bitset. */
	const Json bit_out_of_range = ValidateCommand("CmdBuildRoad", Arguments(1000, Arguments(0, 64), 0, 0, 0));
	CHECK(bit_out_of_range["error"].get<std::string>() == "invalid_arguments");

	/* A negative value for an unsigned argument. */
	const Json negative = ValidateCommand("CmdBuildRoad", Arguments(1000, 1, -1, 0, 0));
	CHECK(negative["error"].get<std::string>() == "invalid_arguments");

	/* A missing argument is an arity error, not a defaulted zero. */
	const Json partial = ValidateCommand("CmdBuildRoad", Arguments(1000));
	CHECK(partial["error"].get<std::string>() == "invalid_arguments");

	/* world.command itself: a valid call decodes, has to declare its guard, and then needs a real
	 * company, which this call does not have. */
	const Json valid = CallCommand({{"command", "CmdMoneyCheat"}, {"args", Arguments(100)}});
	CHECK_FALSE(valid["ok"].get<bool>());
	CHECK(valid["error"].get<std::string>() == "missing_expected_targets");
	const Json guarded = CallCommand({
		{"command", "CmdMoneyCheat"},
		{"args", Arguments(100)},
		{"expected_targets", Arguments(Ref("company", 255))},
	});
	CHECK(guarded["error"].get<std::string>() == "invalid_company");

	CHECK(CallCommand({{"command", "CmdNoSuchCommand"}, {"args", Arguments()}})["error"].get<std::string>() == "unknown_command");
	CHECK(CallCommand({{"command", "CmdPasteBlueprint"}, {"args", Arguments(1000, 0)}})["error"].get<std::string>() == "unsupported_argument_types");
	CHECK(CallCommand({{"args", Arguments()}})["error"].get<std::string>() == "invalid_params");
	CHECK(CallCommand({{"command", "CmdMoneyCheat"}, {"args", 5}})["error"].get<std::string>() == "invalid_params");
	CHECK(CallCommand({{"command", "CmdMoneyCheat"}, {"args", Arguments(1)}, {"dry_run", 1}})["error"].get<std::string>() == "invalid_params");
	CHECK(CallCommand({{"command", "CmdMoneyCheat"}, {"args", Arguments(1)}, {"location", -1}})["error"].get<std::string>() == "invalid_params");
}

TEST_CASE("AgentCommandCatalogDecodesStructuredArguments")
{
	/* EncodedString as literal text: encoded through the engine's own raw-text string. */
	const Json text = ValidateCommand("CmdTownSetText", Arguments(5, "Hello town"));
	REQUIRE(text["ok"].get<bool>());
	CHECK(text["arguments"][0] == 5);
	CHECK(text["arguments"][1]["text"].get<std::string>() == "Hello town");
	CHECK(text["targets"][0]["entity"].get<std::string>() == "town");
	CHECK(text["targets"][0]["id"] == 5);

	/* EncodedString as a literal text object and as a StringID with parameters. */
	const Json text_object = ValidateCommand("CmdTownSetText", Arguments(5, Json{{"text", "Object text"}}));
	REQUIRE(text_object["ok"].get<bool>());
	CHECK(text_object["arguments"][1]["text"].get<std::string>() == "Object text");

	const Json string_id = ValidateCommand("CmdTownSetText", Arguments(5, Json{{"string_id", 1234}}));
	REQUIRE(string_id["ok"].get<bool>());
	CHECK(string_id["arguments"][1]["string_id"] == 1234);
	CHECK(string_id["arguments"][1]["params"].empty());

	const Json with_params = ValidateCommand("CmdTownSetText", Arguments(5, Json{
		{"string_id", 1234},
		{"params", Arguments("a", 5, Json{{"id", 7}}, Json{{"text", "b"}})},
	}));
	REQUIRE(with_params["ok"].get<bool>());
	REQUIRE(with_params["arguments"][1]["params"].size() == 4);
	CHECK(with_params["arguments"][1]["params"][0].get<std::string>() == "a");
	CHECK(with_params["arguments"][1]["params"][1] == 5);
	CHECK(with_params["arguments"][1]["params"][2]["id"] == 7);
	CHECK(with_params["arguments"][1]["params"][3]["text"].get<std::string>() == "b");

	/* Invalid nested EncodedStrings. */
	CHECK(ValidateCommand("CmdTownSetText", Arguments(5, Json::object()))["error"].get<std::string>() == "invalid_arguments");
	CHECK(ValidateCommand("CmdTownSetText", Arguments(5, Json{{"text", "a"}, {"string_id", 1}}))["error"].get<std::string>() == "invalid_arguments");
	CHECK(ValidateCommand("CmdTownSetText", Arguments(5, Json{{"text", "a"}, {"params", Arguments(1)}}))["error"].get<std::string>() == "invalid_arguments");
	CHECK(ValidateCommand("CmdTownSetText", Arguments(5, Json{{"string_id", "x"}}))["error"].get<std::string>() == "invalid_arguments");
	CHECK(ValidateCommand("CmdTownSetText", Arguments(5, 7))["error"].get<std::string>() == "invalid_arguments");

	/* Source: a town, an industry or a headquarters, both of them guarded as targets. */
	const Json subsidy = ValidateCommand("CmdCreateSubsidy", Arguments(0, Json{{"type", "town"}, {"id", 5}}, Json{{"type", "industry"}, {"id", 7}}));
	REQUIRE(subsidy["ok"].get<bool>());
	CHECK(subsidy["arguments"][1]["type"].get<std::string>() == "town");
	CHECK(subsidy["arguments"][2]["id"] == 7);
	REQUIRE(subsidy["targets"].size() == 2);
	CHECK(subsidy["targets"][0]["entity"].get<std::string>() == "town");
	CHECK(subsidy["targets"][0]["id"] == 5);
	CHECK(subsidy["targets"][1]["entity"].get<std::string>() == "industry");
	CHECK(subsidy["targets"][1]["id"] == 7);

	CHECK(ValidateCommand("CmdCreateSubsidy", Arguments(0, Json{{"type", "town"}}, Json{{"type", "town"}, {"id", 1}}))["error"].get<std::string>() == "invalid_arguments");
	CHECK(ValidateCommand("CmdCreateSubsidy", Arguments(0, Json{{"type", "moon"}, {"id", 1}}, Json{{"type", "town"}, {"id", 1}}))["error"].get<std::string>() == "invalid_arguments");
	CHECK(ValidateCommand("CmdCreateSubsidy", Arguments(0, Json{{"type", "town"}, {"id", 65535}}, Json{{"type", "town"}, {"id", 1}}))["error"].get<std::string>() == "invalid_arguments");
	CHECK(ValidateCommand("CmdCreateSubsidy", Arguments(0, Json{{"type", "town"}, {"id", 70000}}, Json{{"type", "town"}, {"id", 1}}))["error"].get<std::string>() == "invalid_arguments");

	/* VehicleListIdentifier: the index is the id the list is about. */
	const Json list = ValidateCommand("CmdMassStartStopVehicle", Arguments(1000, true, false, Json{
		{"list", "group"}, {"vtype", 0}, {"company", 1}, {"index", 3},
	}));
	REQUIRE(list["ok"].get<bool>());
	CHECK(list["arguments"][3]["list"].get<std::string>() == "group");
	CHECK(list["arguments"][3]["company"] == 1);
	REQUIRE(list["targets"].size() == 1);
	CHECK(list["targets"][0]["entity"].get<std::string>() == "group");
	CHECK(list["targets"][0]["id"] == 3);

	CHECK(ValidateCommand("CmdMassStartStopVehicle", Arguments(1000, true, false, Json{{"list", "nonsense"}, {"vtype", 0}, {"company", 1}, {"index", 3}}))["error"].get<std::string>() == "invalid_arguments");
	CHECK(ValidateCommand("CmdMassStartStopVehicle", Arguments(1000, true, false, Json{{"list", "group"}, {"company", 1}, {"index", 3}}))["error"].get<std::string>() == "invalid_arguments");
	CHECK(ValidateCommand("CmdMassStartStopVehicle", Arguments(1000, true, false, Json{{"list", "group"}, {"vtype", 4}, {"company", 1}, {"index", 3}}))["error"].get<std::string>() == "invalid_arguments");
	CHECK(ValidateCommand("CmdMassStartStopVehicle", Arguments(1000, true, false, Json{{"list", "group"}, {"vtype", 0}, {"company", 1}}))["error"].get<std::string>() == "invalid_arguments");

	/* Order: the destination is a target too, so a nested id is guarded as well. */
	const Json order = ValidateCommand("CmdInsertOrder", Arguments(10, 0, Json{{"type", "goto_station"}, {"destination", 5}}));
	REQUIRE(order["ok"].get<bool>());
	CHECK(order["arguments"][2]["destination"] == 5);
	CHECK(order["arguments"][2]["destination_entity"].get<std::string>() == "station");
	REQUIRE(order["targets"].size() == 2);
	CHECK(order["targets"][0]["entity"].get<std::string>() == "vehicle");
	CHECK(order["targets"][0]["id"] == 10);
	CHECK(order["targets"][1]["entity"].get<std::string>() == "station");
	CHECK(order["targets"][1]["id"] == 5);
	CHECK(order["targets"][1]["via"].get<std::string>() == "destination");

	/* An order to a depot, with the depot flags of that order type. */
	const Json depot_order = ValidateCommand("CmdInsertOrder", Arguments(10, 0, Json{
		{"type", "goto_depot"}, {"destination", 2}, {"depot_order", 1}, {"action", 0}, {"wait_time", 100},
	}));
	REQUIRE(depot_order["ok"].get<bool>());
	CHECK(depot_order["arguments"][2]["destination_entity"].get<std::string>() == "depot");
	CHECK(depot_order["targets"][1]["entity"].get<std::string>() == "depot");
	CHECK(depot_order["targets"][1]["id"] == 2);

	/* Orders without a destination, and one that is only a marker. */
	REQUIRE(ValidateCommand("CmdInsertOrder", Arguments(10, 0, Json{{"type", "loading"}, {"ordered", true}}))["ok"].get<bool>());
	REQUIRE(ValidateCommand("CmdInsertOrder", Arguments(10, 0, Json{{"type", "leave_station"}}))["ok"].get<bool>());

	/* Invalid nested Orders: the field checks have to be precise, not just "some error". */
	const auto order_error = [](const Json &order_value) {
		return ValidateCommand("CmdInsertOrder", Arguments(10, 0, order_value))["message"].get<std::string>();
	};
	CHECK(ValidateCommand("CmdInsertOrder", Arguments(10, 0, Json{{"type", "nonsense"}}))["error"].get<std::string>() == "invalid_arguments");
	CHECK(order_error(Json{{"type", "goto_station"}}).find("needs 'destination'") != std::string::npos);
	CHECK(order_error(Json{{"type", "loading"}, {"destination", 5}}).find("takes no 'destination'") != std::string::npos);
	CHECK(order_error(Json{{"type", "goto_station"}, {"destination", 5}, {"ordered", true}}).find("takes no 'ordered'") != std::string::npos);
	CHECK(order_error(Json{{"type", "goto_station"}, {"destination", 5}, {"depot_order", 1}}).find("takes no 'depot_order'") != std::string::npos);
	CHECK(order_error(Json{{"type", "goto_station"}, {"destination", 5}, {"nonsense", 1}}).find("unknown order field") != std::string::npos);
	CHECK(order_error(Json{{"type", "goto_station"}, {"destination", 70000}}).find("does not fit") != std::string::npos);
	CHECK(order_error(Json{{"type", "goto_station"}, {"destination", -1}}).find("does not fit") != std::string::npos);
	CHECK(order_error(Json{{"type", "goto_depot"}, {"destination", 2}, {"depot_order", 300}}).find("argument") != std::string::npos);

	/* An aircraft hangar is a station, not a depot, so the caller says what the destination is and
	 * the target follows the explicit value instead of the order type. */
	const Json hangar = ValidateCommand("CmdInsertOrder", Arguments(10, 0, Json{
		{"type", "goto_depot"}, {"destination", 7}, {"destination_entity", "station"},
	}));
	REQUIRE(hangar["ok"].get<bool>());
	CHECK(hangar["arguments"][2]["destination_entity"].get<std::string>() == "station");
	CHECK(hangar["targets"][1]["entity"].get<std::string>() == "station");
	CHECK(hangar["targets"][1]["id"] == 7);

	/* An explicit destination kind that the order type cannot have is rejected, not ignored. */
	CHECK(order_error(Json{{"type", "goto_station"}, {"destination", 5}, {"destination_entity", "depot"}}).find("cannot have destination_entity") != std::string::npos);
	CHECK(order_error(Json{{"type", "goto_depot"}, {"destination", 2}, {"destination_entity", "waypoint"}}).find("cannot have destination_entity") != std::string::npos);
	CHECK(order_error(Json{{"type", "goto_depot"}, {"destination", 2}, {"destination_entity", 5}}).find("must be a string") != std::string::npos);

	/* Enums that declare an end marker reject values outside the enum, not just values that do not
	 * fit the storage type. */
	const Json news_type = ValidateCommand("CmdCustomNewsItem", Arguments(15, 1, Json{{"type", "none"}}, "x"));
	REQUIRE(news_type["ok"].get<bool>());
	const Json bad_news_type = ValidateCommand("CmdCustomNewsItem", Arguments(16, 1, Json{{"type", "none"}}, "x"));
	CHECK(bad_news_type["error"].get<std::string>() == "invalid_arguments");
	CHECK(bad_news_type["message"].get<std::string>().find("not a valid NewsType") != std::string::npos);
	CHECK(ValidateCommand("CmdCustomNewsItem", Arguments(200, 1, Json{{"type", "none"}}, "x"))["error"].get<std::string>() == "invalid_arguments");
	REQUIRE(ValidateCommand("CmdGoalQuestion", Arguments(1, 0, false, 0, 3, "q"))["ok"].get<bool>());
	const Json bad_question_type = ValidateCommand("CmdGoalQuestion", Arguments(1, 0, false, 0, 4, "q"));
	CHECK(bad_question_type["error"].get<std::string>() == "invalid_arguments");
	CHECK(bad_question_type["message"].get<std::string>().find("not a valid GoalQuestionType") != std::string::npos);
	CHECK(ValidateCommand("CmdInsertOrder", Arguments(10, 0, 5))["error"].get<std::string>() == "invalid_arguments");

	/* A std::variant argument (the news reference) names its alternative and its value. */
	const Json news = ValidateCommand("CmdCustomNewsItem", Arguments(0, 1, Json{{"type", "town"}, {"value", 5}}, "News text"));
	REQUIRE(news["ok"].get<bool>());
	CHECK(news["arguments"][2]["type"].get<std::string>() == "town");
	CHECK(news["arguments"][2]["value"] == 5);
	REQUIRE(news["targets"].size() == 2);
	CHECK(news["targets"][0]["entity"].get<std::string>() == "company");
	CHECK(news["targets"][0]["id"] == 1);
	CHECK(news["targets"][1]["entity"].get<std::string>() == "town");
	CHECK(news["targets"][1]["id"] == 5);
	CHECK(news["targets"][1]["via"].get<std::string>() == "value");

	/* The empty alternative is the engine's monostate. */
	const Json no_reference = ValidateCommand("CmdCustomNewsItem", Arguments(0, 1, Json{{"type", "none"}}, "News text"));
	REQUIRE(no_reference["ok"].get<bool>());
	REQUIRE(no_reference["targets"].size() == 1);
	CHECK(no_reference["targets"][0]["entity"].get<std::string>() == "company");

	CHECK(ValidateCommand("CmdCustomNewsItem", Arguments(0, 1, Json{{"type", "moon"}, {"value", 5}}, "x"))["error"].get<std::string>() == "invalid_arguments");
	CHECK(ValidateCommand("CmdCustomNewsItem", Arguments(0, 1, Json{{"type", "none"}, {"value", 5}}, "x"))["error"].get<std::string>() == "invalid_arguments");
	CHECK(ValidateCommand("CmdCustomNewsItem", Arguments(0, 1, Json{{"type", "town"}, {"value", "x"}}, "x"))["error"].get<std::string>() == "invalid_arguments");
	CHECK(ValidateCommand("CmdCustomNewsItem", Arguments(0, 1, 5, "x"))["error"].get<std::string>() == "invalid_arguments");
}

TEST_CASE("AgentCommandCatalogGuardsExpectedTargets")
{
	const Json order_args = Arguments(10, 0, Json{{"type", "goto_station"}, {"destination", 5}});
	/* Mirror a write: with a company, the actor company is a target as well. */
	const auto validate = [](const Json &args, const Json &expected, int company = -1) {
		Json params = {
			{"op", "validate"},
			{"command", "CmdInsertOrder"},
			{"args", args},
			{"expected_targets", expected},
		};
		if (company >= 0) params["company"] = company;
		return ReadCatalog(params);
	};

	/* The complete list passes: exactly the entities the arguments name. */
	REQUIRE(validate(order_args, Arguments(Ref("vehicle", 10), Ref("station", 5)))["ok"].get<bool>());

	/* A target the arguments name but the guard does not is missing, so a write can never touch an
	 * object the bridge did not resolve. */
	const Json missing = validate(order_args, Arguments(Ref("vehicle", 10)));
	CHECK(missing["error"].get<std::string>() == "target_mismatch");
	REQUIRE(missing["detail"]["missing"].size() == 1);
	CHECK(missing["detail"]["missing"][0]["entity"].get<std::string>() == "station");
	CHECK(missing["detail"]["missing"][0]["id"] == 5);

	/* Zero targets on a command that names two entities is missing both of them. */
	const Json zero = validate(order_args, Json::array());
	CHECK(zero["error"].get<std::string>() == "target_mismatch");
	CHECK(zero["detail"]["missing"].size() == 2);

	/* A guarded ref the arguments do not name is rejected too: an unrelated valid ref cannot
	 * authorize a reused target. */
	const Json unrelated = validate(order_args, Arguments(Ref("vehicle", 10), Ref("station", 5), Ref("station", 9)));
	CHECK(unrelated["error"].get<std::string>() == "target_mismatch");
	REQUIRE(unrelated["detail"]["unexpected"].size() == 1);
	CHECK(unrelated["detail"]["unexpected"][0]["entity"].get<std::string>() == "station");
	CHECK(unrelated["detail"]["unexpected"][0]["id"] == 9);
	CHECK_FALSE(unrelated["detail"].contains("missing"));

	/* A different, perfectly valid vehicle id is a mismatch in both directions. */
	const Json reused = validate(order_args, Arguments(Ref("vehicle", 11), Ref("station", 5)));
	CHECK(reused["error"].get<std::string>() == "target_mismatch");
	REQUIRE(reused["detail"].contains("missing"));
	REQUIRE(reused["detail"]["missing"].size() == 1);
	CHECK(reused["detail"]["missing"][0]["id"] == 10);
	REQUIRE(reused["detail"].contains("unexpected"));
	REQUIRE(reused["detail"]["unexpected"].size() == 1);
	CHECK(reused["detail"]["unexpected"][0]["id"] == 11);

	/* The nested order destination is bound to the actual argument, not just to "a station". */
	const Json wrong_destination = validate(order_args, Arguments(Ref("vehicle", 10), Ref("station", 6)));
	CHECK(wrong_destination["error"].get<std::string>() == "target_mismatch");

	/* The actor company is a target: a guard without it is incomplete. */
	const Json without_company = validate(order_args, Arguments(Ref("vehicle", 10), Ref("station", 5)), 3);
	CHECK(without_company["error"].get<std::string>() == "target_mismatch");
	REQUIRE(without_company["detail"]["missing"].size() == 1);
	CHECK(without_company["detail"]["missing"][0]["entity"].get<std::string>() == "company");
	CHECK(without_company["detail"]["missing"][0]["id"] == 3);
	REQUIRE(validate(order_args, Arguments(Ref("company", 3), Ref("vehicle", 10), Ref("station", 5)), 3)["ok"].get<bool>());

	/* A command without an entity argument is guarded by its company alone. */
	const auto validate_build = [](const Json &expected, int company) {
		return ReadCatalog({
			{"op", "validate"},
			{"command", "CmdLandscapeClear"},
			{"args", Arguments(1000)},
			{"expected_targets", expected},
			{"company", company},
		});
	};
	REQUIRE(validate_build(Arguments(Ref("company", 3)), 3)["ok"].get<bool>());
	CHECK(validate_build(Json::array(), 3)["error"].get<std::string>() == "target_mismatch");
	CHECK(validate_build(Arguments(Ref("town", 5), Ref("company", 3)), 3)["error"].get<std::string>() == "target_mismatch");

	/* A write may not omit the guard at all: the bridge only checks the refs it was given. */
	const Json no_guard = CallCommand({
		{"command", "CmdLandscapeClear"},
		{"args", Arguments(1000)},
	});
	CHECK(no_guard["error"].get<std::string>() == "missing_expected_targets");
	const Json with_guard = CallCommand({
		{"command", "CmdLandscapeClear"},
		{"args", Arguments(1000)},
		{"expected_targets", Arguments(Ref("company", 255))},
	});
	/* The guard passed and the call stopped at the company check, which needs a real company. */
	CHECK(with_guard["error"].get<std::string>() == "invalid_company");
}

TEST_CASE("AgentCommandCatalogReportsTargetsAndGaps")
{
	const Json catalog = ReadCatalog({{"op", "list"}});
	const Json &commands = catalog["commands"];

	/* A command that only acts on the company itself has no entity target. */
	const Json &clear = FindEntry(commands, "CmdLandscapeClear");
	CHECK(clear["targets"].empty());
	CHECK(clear["target_scope"].get<std::string>() == "company");

	/* A build command names the town it builds in, and nothing else. */
	const Json &build_road = FindEntry(commands, "CmdBuildRoad");
	REQUIRE(build_road["targets"].size() == 1);
	CHECK(build_road["targets"][0]["index"] == 4);
	CHECK(build_road["targets"][0]["target"]["entity"].get<std::string>() == "town");

	/* A command that renames a town names exactly that town. */
	const Json &rename_town = FindEntry(commands, "CmdRenameTown");
	REQUIRE(rename_town["targets"].size() == 1);
	CHECK(rename_town["targets"][0]["index"] == 0);
	CHECK(rename_town["targets"][0]["target"]["entity"].get<std::string>() == "town");
	CHECK(rename_town["target_scope"].get<std::string>() == "entities");

	/* The variant argument lists the alternatives the decoder accepts. */
	const Json described = ReadCatalog({{"op", "describe"}, {"command", "CmdCustomNewsItem"}});
	const Json &news_arguments = described["commands"][0]["arguments"];
	REQUIRE(news_arguments.size() == 4);
	CHECK(news_arguments[2]["kind"].get<std::string>() == "variant");
	CHECK(news_arguments[2]["supported"].get<bool>());
	const Json &alternatives = news_arguments[2]["schema"]["type"];
	CHECK(alternatives.size() == 7);
	for (const char *alternative : {"none", "tile", "vehicle", "station", "industry", "town", "engine"}) {
		CHECK(std::ranges::find(alternatives, alternative) != alternatives.end());
	}
	CHECK(news_arguments[2]["target"]["id_path"].get<std::string>() == "value");
	CHECK(news_arguments[2]["target"]["entity_by"]["map"]["town"].get<std::string>() == "town");

	/* The backend derives its expected refs from the published contract, not from a guess. */
	const Json &refs = described["commands"][0]["expected_refs"];
	CHECK(refs["required_for_write"].get<bool>());
	CHECK(refs["set_equality"].get<bool>());
	CHECK(refs["includes_actor_company"].get<bool>());
	CHECK(refs["sources"].is_array());
	const Json &order_refs = FindEntry(commands, "CmdInsertOrder")["expected_refs"];
	REQUIRE(order_refs["sources"].size() == 2);
	CHECK(order_refs["sources"][0]["index"] == 0);
	CHECK(order_refs["sources"][0]["target"]["entity"].get<std::string>() == "vehicle");
	CHECK(order_refs["sources"][0]["target"]["id_path"].is_null());
	CHECK(order_refs["sources"][1]["index"] == 2);
	CHECK(order_refs["sources"][1]["target"]["id_path"].get<std::string>() == "destination");
	CHECK(order_refs["sources"][1]["target"]["entity_by"]["map"]["depot"].get<std::string>() == "depot");
	CHECK(order_refs["sources"][1]["target"]["entity_by"]["map"]["station"].get<std::string>() == "station");

	/* The structured argument types are dispatchable now. */
	for (const char *name : {"CmdTownSetText", "CmdCreateSubsidy", "CmdInsertOrder", "CmdMassStartStopVehicle", "CmdSendVehicleToDepot"}) {
		const Json &entry = FindEntry(commands, name);
		CHECK(entry["support"]["supported"].get<bool>());
		CHECK(entry["support"]["status"].get<std::string>() == "dispatchable");
	}

	/* The capability document says the same, and still reports the one real gap. */
	const Json caps = CommandCatalogCapabilities();
	const size_t dispatchable = caps["commands"]["dispatchable"].get<size_t>();
	/* The milestone number: only the two BlueprintChunkData commands are gaps. */
	CHECK(dispatchable == 156);
	CHECK(caps["commands"]["gaps"].get<size_t>() == 2);
	CHECK(caps["commands"]["gaps"].get<size_t>() == caps["commands"]["total"].get<size_t>() - dispatchable);

	bool blueprint_gap = false;
	for (const Json &gap : caps["gaps"]) {
		if (gap["gap"] == "blueprint_chunk_data") {
			blueprint_gap = true;
			CHECK(gap["commands"].size() == 2);
		}
	}
	CHECK(blueprint_gap);
	CHECK(caps["targets"].contains("guard"));
	CHECK(ValidateCommand("CmdPasteBlueprint", Arguments(1000, 0))["error"].get<std::string>() == "unsupported_argument_types");
}

TEST_CASE("AgentCommandCatalogReportsTruthfulCapabilities")
{
	const Json caps = CommandCatalogCapabilities();
	REQUIRE(caps["ok"].get<bool>());

	/* The counts come from the engine's own command table. */
	CHECK(caps["commands"]["total"].get<size_t>() == to_underlying(Commands::End));
	CHECK(caps["commands"]["engine_end"].get<size_t>() == to_underlying(Commands::End));
	const size_t total = caps["commands"]["total"].get<size_t>();
	const size_t dispatchable = caps["commands"]["dispatchable"].get<size_t>();
	CHECK(caps["commands"]["gaps"].get<size_t>() == total - dispatchable);

	/* Every gap is named, and the gap commands really are the undecodable ones. */
	const Json &complex = caps["gaps"][2];
	REQUIRE(complex["gap"].get<std::string>() == "complex_argument_types");
	CHECK(complex["commands"].size() == total - dispatchable);
	for (const Json &gap : complex["commands"]) {
		CHECK_FALSE(gap["reason"].get<std::string>().empty());
	}

	/* The catalog lists the methods it answers. */
	bool has_list = false;
	bool has_write = false;
	for (const Json &method : caps["methods"]) {
		if (method["method"] == "world.commands" && method["op"] == "list") has_list = true;
		if (method["method"] == "world.command") has_write = true;
	}
	CHECK(has_list);
	CHECK(has_write);

	/* Other methods are not ours. */
	CHECK_FALSE(DispatchCommandCatalog("world.info", Json::object(), CompanyID::Invalid()).has_value());
}
