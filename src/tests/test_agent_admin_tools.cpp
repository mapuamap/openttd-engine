/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file test_agent_admin_tools.cpp Test the dispatch, the argument validation, the setting
 * descriptors and the reported gaps of the agent administrative tools.
 *
 * No game is running in this test binary, so the cases here cover dispatch, the validation that
 * runs before a command is posted, the setting descriptors that are read from the real setting
 * tables, and gap reporting. A game setting can be changed without a game, so the settings cases
 * also run the real command path; the company, town and project commands need a running game and
 * are only validated here.
 */

#include "../stdafx.h"

#include "../3rdparty/catch2/catch.hpp"

#include "../agent_admin_tools.h"
#include "../town_growth.h"
#include "../town_growth_type.h"

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
 * Find one setting row by name.
 * @param result The result of world.settings.get.
 * @param name The setting name.
 * @return The row, or an empty object when the setting is not in the result.
 */
static Json FindSetting(const Json &result, std::string_view name)
{
	for (const Json &row : result["settings"]) {
		if (row["name"].is_string() && row["name"].get<std::string>() == name) return row;
	}
	return Json::object();
}

/**
 * Dispatch a call that is expected to be handled by this module.
 * @param method The method name.
 * @param params The parameters.
 * @return The result document.
 */
static Json Dispatch(std::string_view method, const Json &params)
{
	const std::optional<Json> result = DispatchAdminTool(method, params, CompanyID::Invalid());
	REQUIRE(result.has_value());
	return *result;
}

TEST_CASE("Agent admin tools: capabilities")
{
	const Json capabilities = AdminToolCapabilities();
	CHECK(capabilities["module"] == "agent_admin_tools");

	const Json settings_get = FindCapability(capabilities, "world.settings.get");
	REQUIRE(!settings_get.empty());
	CHECK(settings_get["authority"] == "read");
	CHECK(Contains(settings_get["implemented"], "game"));
	CHECK(Contains(settings_get["implemented"], "company"));

	const Json settings_set = FindCapability(capabilities, "world.settings.set");
	REQUIRE(!settings_set.empty());
	CHECK(settings_set["authority"] == "write");
	CHECK(Contains(settings_set["implemented"], "one_of_many"));
	CHECK(Contains(settings_set["gaps"], "client_scope"));

	const Json company = FindCapability(capabilities, "world.company");
	REQUIRE(!company.empty());
	CHECK(Contains(company["implemented"], "loan_increase"));
	CHECK(Contains(company["implemented"], "create"));

	const Json towns = FindCapability(capabilities, "world.towns");
	REQUIRE(!towns.empty());
	CHECK(Contains(towns["implemented"], "needs"));
	CHECK(Contains(towns["implemented"], "invest"));
	CHECK(Contains(towns["implemented"], "force_need"));

	const Json project = FindCapability(capabilities, "world.project");
	REQUIRE(!project.empty());
	CHECK(Contains(project["implemented"], "power"));
	CHECK(Contains(project["implemented"], "station_town"));
	CHECK(Contains(project["gaps"], "save"));

	/* The methods of the other modules are not claimed here. */
	CHECK(FindCapability(capabilities, "world.pause").empty());
	CHECK(FindCapability(capabilities, "world.objects").empty());
	CHECK(FindCapability(capabilities, "world.vehicles").empty());
}

TEST_CASE("Agent admin tools: dispatch")
{
	/* Methods of the other modules are not handled here. */
	CHECK_FALSE(DispatchAdminTool("world.pause", Json::object(), CompanyID::Invalid()).has_value());
	CHECK_FALSE(DispatchAdminTool("world.objects", Json::object(), CompanyID::Invalid()).has_value());
	CHECK_FALSE(DispatchAdminTool("world.vehicles", Json::object(), CompanyID::Invalid()).has_value());
	CHECK_FALSE(DispatchAdminTool("", Json::object(), CompanyID::Invalid()).has_value());

	/* Parameters must be an object. */
	const Json result = Dispatch("world.company", Json::array());
	CHECK(result["ok"] == false);
	CHECK(result["error"] == "invalid_arguments");

	/* A missing operation is an argument error, an unknown one is a reported gap. */
	CHECK(Dispatch("world.company", Json::object())["error"] == "invalid_arguments");
	const Json gap = Dispatch("world.company", Json::object({{"op", "company_password"}}));
	CHECK(gap["ok"] == false);
	CHECK(gap["error"] == "unsupported_op");
	CHECK(gap["detail"]["op"] == "company_password");
}

TEST_CASE("Agent admin tools: mutation classification")
{
	CHECK(IsAdminMutation("world.settings.set", Json::object()));
	CHECK_FALSE(IsAdminMutation("world.settings.get", Json::object()));

	CHECK_FALSE(IsAdminMutation("world.company", Json::object({{"op", "info"}})));
	CHECK(IsAdminMutation("world.company", Json::object({{"op", "loan_increase"}})));
	CHECK(IsAdminMutation("world.company", Json::object()));

	CHECK_FALSE(IsAdminMutation("world.towns", Json::object({{"op", "needs"}})));
	CHECK(IsAdminMutation("world.towns", Json::object({{"op", "invest"}})));

	CHECK_FALSE(IsAdminMutation("world.project", Json::object({{"op", "power"}})));
	CHECK_FALSE(IsAdminMutation("world.project", Json::object({{"op", "district"}})));
	CHECK(IsAdminMutation("world.project", Json::object({{"op", "power_fuel"}})));

	/* Methods of the other modules are not mutations of this one. */
	CHECK_FALSE(IsAdminMutation("world.pause", Json::object()));
}

TEST_CASE("Agent admin tools: settings descriptors")
{
	/* A plain integer setting with a range. */
	const Json result = Dispatch("world.settings.get", Json::object({{"names", Json::array({"difficulty.number_towns"})}}));
	REQUIRE(result["ok"] == true);
	REQUIRE(result["settings"].size() == 1);
	const Json row = result["settings"][0];
	CHECK(row["name"] == "difficulty.number_towns");
	CHECK(row["scope"] == "game");
	CHECK(row["type"] == "int");
	CHECK(row["min"] == 0);
	CHECK(row["max"] == 4);
	CHECK(row["value"].is_number_integer());
	CHECK(row["editable"].is_boolean());
	CHECK(row["newgame_only"] == true);
	CHECK(row["formatted"].is_string());

	/* An unknown name is reported, not silently dropped. */
	const Json unknown = Dispatch("world.settings.get", Json::object({{"names", Json::array({"no.such.setting"})}}));
	CHECK(unknown["ok"] == true);
	CHECK(unknown["settings"].empty());
	REQUIRE(unknown["unknown"].size() == 1);
	CHECK(unknown["unknown"][0] == "no.such.setting");

	/* A prefix lists the settings of one group. */
	const Json prefixed = Dispatch("world.settings.get", Json::object({{"prefix", "difficulty."}, {"limit", 500}}));
	REQUIRE(prefixed["ok"] == true);
	REQUIRE(prefixed["settings"].size() > 0);
	for (const Json &entry : prefixed["settings"]) {
		CHECK(entry["name"].get<std::string>().starts_with("difficulty."));
	}
	CHECK(prefixed["total"] >= prefixed["settings"].size());

	/* The limit and the offset are validated and applied. */
	const Json limited = Dispatch("world.settings.get", Json::object({{"prefix", "difficulty."}, {"limit", 2}}));
	REQUIRE(limited["ok"] == true);
	CHECK(limited["settings"].size() == 2);
	const Json offset = Dispatch("world.settings.get", Json::object({{"prefix", "difficulty."}, {"limit", 2}, {"offset", 1}}));
	REQUIRE(offset["ok"] == true);
	REQUIRE(offset["settings"].size() == 2);
	CHECK(offset["settings"][0]["name"] == limited["settings"][1]["name"]);

	CHECK(Dispatch("world.settings.get", Json::object({{"limit", 0}}))["error"] == "invalid_arguments");
	CHECK(Dispatch("world.settings.get", Json::object({{"limit", 100000}}))["error"] == "invalid_arguments");
	CHECK(Dispatch("world.settings.get", Json::object({{"offset", -1}}))["error"] == "invalid_arguments");
	CHECK(Dispatch("world.settings.get", Json::object({{"prefix", 5}}))["error"] == "invalid_arguments");
	CHECK(Dispatch("world.settings.get", Json::object({{"names", "difficulty.number_towns"}}))["error"] == "invalid_arguments");

	/* A one-of-many setting reports the values it accepts. */
	const Json one_of_many = Dispatch("world.settings.get", Json::object({{"names", Json::array({"difficulty.train_flip_reverse_allowed"})}}));
	REQUIRE(one_of_many["ok"] == true);
	const Json one_of_many_row = FindSetting(one_of_many, "difficulty.train_flip_reverse_allowed");
	REQUIRE(!one_of_many_row.empty());
	CHECK(one_of_many_row["type"] == "one_of_many");
	CHECK(one_of_many_row["allowed"].is_array());
	CHECK(one_of_many_row["allowed"].size() > 0);
}

TEST_CASE("Agent admin tools: settings set validation")
{
	/* The name and the value are required. */
	CHECK(Dispatch("world.settings.set", Json::object({{"value", 1}}))["error"] == "invalid_arguments");
	CHECK(Dispatch("world.settings.set", Json::object({{"name", "difficulty.number_towns"}}))["error"] == "invalid_arguments");
	CHECK(Dispatch("world.settings.set", Json::object({{"name", 5}, {"value", 1}}))["error"] == "invalid_arguments");

	/* An unknown setting is reported as such. */
	const Json unknown = Dispatch("world.settings.set", Json::object({{"name", "no.such.setting"}, {"value", 1}}));
	CHECK(unknown["ok"] == false);
	CHECK(unknown["error"] == "unknown_setting");

	/* The range of the setting is enforced before anything is posted. */
	const Json out_of_range = Dispatch("world.settings.set", Json::object({{"name", "difficulty.number_towns"}, {"value", 99}}));
	CHECK(out_of_range["ok"] == false);
	CHECK(out_of_range["error"] == "value_out_of_range");
	CHECK(out_of_range["detail"]["min"] == 0);
	CHECK(out_of_range["detail"]["max"] == 4);

	const Json not_a_number = Dispatch("world.settings.set", Json::object({{"name", "difficulty.number_towns"}, {"value", "many"}}));
	CHECK(not_a_number["error"] == "invalid_value");

	/* A company id only applies to per-company settings. */
	const Json wrong_scope = Dispatch("world.settings.set", Json::object({{"name", "difficulty.number_towns"}, {"value", 1}, {"company_id", 0}}));
	CHECK(wrong_scope["ok"] == false);
	CHECK(wrong_scope["error"] == "invalid_arguments");

	/* A per-company setting needs an existing company. */
	const Json no_company = Dispatch("world.settings.set", Json::object({{"name", "company.engine_renew"}, {"value", true}}));
	CHECK(no_company["ok"] == false);
	CHECK(no_company["error"] == "unknown_company");

	/* The scopes this module does not write are reported as gaps, not as success. */
	const Json client = Dispatch("world.settings.set", Json::object({{"name", "gui.autosave_interval"}, {"value", 1}}));
	CHECK(client["ok"] == false);
	CHECK(client["error"] == "unsupported_op");
	CHECK(client["detail"]["op"] == "client_scope");

	const Json string_setting = Dispatch("world.settings.set", Json::object({{"name", "locale.digit_group_separator"}, {"value", " "}}));
	CHECK(string_setting["ok"] == false);
	CHECK(string_setting["error"] == "unsupported_op");
	CHECK(string_setting["detail"]["op"] == "string_setting");

	/* A valid change runs the real command path and reports what the engine applied. */
	const Json posted = Dispatch("world.settings.set", Json::object({{"name", "difficulty.number_towns"}, {"value", 3}}));
	CHECK(posted["name"] == "difficulty.number_towns");
	CHECK(posted["scope"] == "game");
	REQUIRE(posted["old_value"].is_number_integer());
	CHECK(posted["ok"] == true);
	CHECK(posted["value"] == 3);
	CHECK(posted["changed"] == true);
	CHECK(posted["cost"] == 0);
	CHECK(posted["formatted"].is_string());

	/* The value is read back from the setting, so a second call sees the new value. */
	const Json again = Dispatch("world.settings.get", Json::object({{"names", Json::array({"difficulty.number_towns"})}}));
	REQUIRE(again["ok"] == true);
	CHECK(again["settings"][0]["value"] == 3);

	/* Put the setting back, so the other test cases see the value they expect. */
	const Json restored = Dispatch("world.settings.set", Json::object({{"name", "difficulty.number_towns"}, {"value", posted["old_value"]}}));
	CHECK(restored["ok"] == true);
	CHECK(restored["value"] == posted["old_value"]);
}

TEST_CASE("Agent admin tools: company validation")
{
	/* Reading a company needs an existing company. */
	const Json info = Dispatch("world.company", Json::object({{"op", "info"}, {"company_id", 0}}));
	CHECK(info["ok"] == false);
	CHECK(info["error"] == "unknown_company");

	CHECK(Dispatch("world.company", Json::object({{"op", "info"}, {"company_id", 99}}))["error"] == "unknown_company");
	CHECK(Dispatch("world.company", Json::object({{"op", "info"}, {"company_id", "x"}}))["error"] == "invalid_arguments");

	/* A loan change must be a multiple of the loan interval. */
	const Json loan = Dispatch("world.company", Json::object({{"op", "loan_increase"}, {"company_id", 0}, {"amount", 12345}}));
	CHECK(loan["ok"] == false);
	CHECK(loan["error"] == "invalid_amount");
	CHECK(loan["detail"]["loan_interval"] == LOAN_INTERVAL);

	/* The maximum loan needs an amount unless it is reset. */
	CHECK(Dispatch("world.company", Json::object({{"op", "set_max_loan"}, {"company_id", 0}}))["error"] == "invalid_arguments");
	const Json max_loan = Dispatch("world.company", Json::object({{"op", "set_max_loan"}, {"company_id", 0}, {"amount", -1}}));
	CHECK(max_loan["error"] == "invalid_amount");

	/* Giving money needs a different, existing receiver and a positive amount. */
	CHECK(Dispatch("world.company", Json::object({{"op", "give_money"}, {"company_id", 0}}))["error"] == "invalid_arguments");
	/* The self transfer rule needs two existing companies, so the company check comes first. */
	CHECK(Dispatch("world.company", Json::object({{"op", "give_money"}, {"company_id", 0}, {"to_company_id", 0}, {"amount", 100}}))["error"] == "unknown_company");
	CHECK(Dispatch("world.company", Json::object({{"op", "give_money"}, {"company_id", 0}, {"to_company_id", 1}, {"amount", 0}}))["error"] == "invalid_amount");

	/* Deleting needs a company id. */
	CHECK(Dispatch("world.company", Json::object({{"op", "delete"}}))["error"] == "invalid_arguments");
}

TEST_CASE("Agent admin tools: town validation")
{
	/* Reading the needs of a town needs an existing town. */
	CHECK(Dispatch("world.towns", Json::object({{"op", "needs"}}))["error"] == "invalid_arguments");
	const Json needs = Dispatch("world.towns", Json::object({{"op", "needs"}, {"town_id", 0}}));
	CHECK(needs["ok"] == false);
	CHECK(needs["error"] == "unknown_town");

	/* The need name and the level are validated before the town is looked up. */
	const Json bad_need = Dispatch("world.towns", Json::object({{"op", "force_need"}, {"need", "sunshine"}, {"level", 1}}));
	CHECK(bad_need["ok"] == false);
	CHECK(bad_need["error"] == "invalid_arguments");
	CHECK(bad_need["detail"]["allowed"].is_array());

	const Json bad_level = Dispatch("world.towns", Json::object({{"op", "force_need"}, {"need", "goods"}, {"level", 99}}));
	CHECK(bad_level["ok"] == false);
	CHECK(bad_level["error"] == "invalid_arguments");
	CHECK(bad_level["detail"]["max"] == TOWN_NEED_LEVEL_MAX);

	/* The investment index is validated before the town is looked up. */
	const Json bad_index = Dispatch("world.towns", Json::object({{"op", "invest"}, {"amount_index", 99}}));
	CHECK(bad_index["ok"] == false);
	CHECK(bad_index["error"] == "invalid_arguments");
	CHECK(bad_index["detail"]["max"] == static_cast<int64_t>(TownGrowthBalance::INVESTMENT_AMOUNTS.size()) - 1);

	CHECK(Dispatch("world.towns", Json::object({{"op", "invest"}}))["error"] == "invalid_arguments");
	CHECK(Dispatch("world.towns", Json::object({{"op", "invest"}, {"amount_index", 0}}))["error"] == "invalid_arguments");

	/* The expand modes are validated before the town is looked up. */
	const Json bad_mode = Dispatch("world.towns", Json::object({{"op", "expand"}, {"modes", Json::array({"towers"})}}));
	CHECK(bad_mode["ok"] == false);
	CHECK(bad_mode["error"] == "invalid_arguments");
	CHECK(Dispatch("world.towns", Json::object({{"op", "expand"}, {"modes", Json::array()}}))["error"] == "invalid_arguments");
	CHECK(Dispatch("world.towns", Json::object({{"op", "expand"}, {"modes", Json::array({"buildings"})}}))["error"] == "invalid_arguments");

	/* The rating range is enforced. */
	const Json bad_rating = Dispatch("world.towns", Json::object({{"op", "rating"}, {"town_id", 0}, {"rating", 5000}}));
	CHECK(bad_rating["ok"] == false);
	CHECK(bad_rating["error"] == "unknown_town");

	/* The investment amount must not be negative. */
	const Json negative = Dispatch("world.towns", Json::object({{"op", "set_investment"}, {"amount", -1}}));
	CHECK(negative["ok"] == false);
	CHECK(negative["error"] == "invalid_amount");

	/* An unknown operation is a reported gap. */
	const Json gap = Dispatch("world.towns", Json::object({{"op", "district"}}));
	CHECK(gap["ok"] == false);
	CHECK(gap["error"] == "unsupported_op");
	CHECK(gap["detail"]["op"] == "district");
}

TEST_CASE("Agent admin tools: project validation")
{
	/* The power grid can be listed without a game; there are no plants then. */
	const Json power = Dispatch("world.project", Json::object({{"op", "power"}}));
	REQUIRE(power["ok"] == true);
	CHECK(power["plants"].is_array());
	CHECK(power["plants"].empty());
	CHECK(power["electricity_available"].is_boolean());

	/* An unknown industry is reported. */
	const Json industry = Dispatch("world.project", Json::object({{"op", "power"}, {"industry_id", 0}}));
	CHECK(industry["ok"] == false);
	CHECK(industry["error"] == "unknown_industry");

	/* Fuel needs a positive amount that fits the stock. */
	CHECK(Dispatch("world.project", Json::object({{"op", "power_fuel"}}))["error"] == "invalid_arguments");
	CHECK(Dispatch("world.project", Json::object({{"op", "power_fuel"}, {"amount", 0}}))["error"] == "invalid_amount");
	CHECK(Dispatch("world.project", Json::object({{"op", "power_fuel"}, {"amount", 70000}}))["error"] == "invalid_amount");

	/* Assigning a station to a town needs both ids. */
	CHECK(Dispatch("world.project", Json::object({{"op", "station_town"}}))["error"] == "invalid_arguments");
	const Json station = Dispatch("world.project", Json::object({{"op", "station_town"}, {"station_id", 0}, {"town_id", 0}}));
	CHECK(station["ok"] == false);
	CHECK(station["error"] == "unknown_station");

	/* The district of a tile needs a tile inside the map. */
	CHECK(Dispatch("world.project", Json::object({{"op", "district"}, {"town_id", 0}}))["error"] == "invalid_arguments");
	const Json tile = Dispatch("world.project", Json::object({{"op", "district"}, {"town_id", 0}, {"tile", Json{{"x", -1}, {"y", 0}}}}));
	CHECK(tile["ok"] == false);
	CHECK(tile["error"] == "invalid_arguments");

	/* The durable save lifecycle is not implemented here. */
	const Json save = Dispatch("world.project", Json::object({{"op", "save"}}));
	CHECK(save["ok"] == false);
	CHECK(save["error"] == "unsupported_op");
	CHECK(save["detail"]["op"] == "save");
}

TEST_CASE("Agent admin tools: extended capability inventory")
{
	const Json capabilities = AdminToolCapabilities();

	/* The query operations are listed apart from the write operations. */
	REQUIRE(capabilities["query_ops"].is_array());
	CHECK(Contains(capabilities["query_ops"], "world.industry op=needs"));
	CHECK(Contains(capabilities["query_ops"], "world.industry op=production"));
	CHECK(Contains(capabilities["query_ops"], "world.industry op=cargo"));
	CHECK(Contains(capabilities["query_ops"], "world.signs op=list"));
	CHECK(Contains(capabilities["query_ops"], "world.landscape op=object_types"));
	CHECK(Contains(capabilities["query_ops"], "world.towns op=power"));
	CHECK(Contains(capabilities["query_ops"], "world.project op=catchment"));

	/* The industry family now covers the queries, the funding and the control commands. */
	const Json industry = FindCapability(capabilities, "world.industry");
	REQUIRE(!industry.empty());
	CHECK(Contains(industry["implemented"], "list"));
	CHECK(Contains(industry["implemented"], "info"));
	CHECK(Contains(industry["implemented"], "needs"));
	CHECK(Contains(industry["implemented"], "production"));
	CHECK(Contains(industry["implemented"], "cargo"));
	CHECK(Contains(industry["implemented"], "fund"));
	CHECK(Contains(industry["implemented"], "fund_area"));
	CHECK(Contains(industry["implemented"], "prospect"));
	CHECK(Contains(industry["implemented"], "set_production"));
	CHECK(Contains(industry["implemented"], "set_flags"));
	CHECK(Contains(industry["implemented"], "set_exclusivity"));
	CHECK(Contains(industry["gaps"], "industry_text"));

	/* Signs, objects, trees and the cheats are new families of this module. */
	const Json signs = FindCapability(capabilities, "world.signs");
	REQUIRE(!signs.empty());
	CHECK(Contains(signs["implemented"], "list"));
	CHECK(Contains(signs["implemented"], "place"));
	CHECK(Contains(signs["implemented"], "rename"));
	CHECK(Contains(signs["implemented"], "delete"));
	CHECK(Contains(signs["implemented"], "move"));

	const Json landscape = FindCapability(capabilities, "world.landscape");
	REQUIRE(!landscape.empty());
	CHECK(Contains(landscape["implemented"], "object_types"));
	CHECK(Contains(landscape["implemented"], "object"));
	CHECK(Contains(landscape["implemented"], "object_area"));
	CHECK(Contains(landscape["implemented"], "tree"));
	CHECK(Contains(landscape["implemented"], "tree_area"));

	const Json cheat = FindCapability(capabilities, "world.cheat");
	REQUIRE(!cheat.empty());
	CHECK(Contains(cheat["implemented"], "money"));
	CHECK(Contains(cheat["implemented"], "bank_balance"));

	/* The town and company families gained the founding, the actions and the company identity. */
	const Json towns = FindCapability(capabilities, "world.towns");
	REQUIRE(!towns.empty());
	CHECK(Contains(towns["implemented"], "found"));
	CHECK(Contains(towns["implemented"], "rename"));
	CHECK(Contains(towns["implemented"], "town_action"));
	CHECK(Contains(towns["implemented"], "list"));

	const Json company = FindCapability(capabilities, "world.company");
	REQUIRE(!company.empty());
	CHECK(Contains(company["implemented"], "colour"));
	CHECK(Contains(company["implemented"], "livery_set"));
	CHECK(Contains(company["implemented"], "manager_face"));
	CHECK(Contains(company["implemented"], "rename"));
	CHECK(Contains(company["implemented"], "rename_president"));
	CHECK(Contains(company["implemented"], "buy"));

	/* The station catchment is a project operation, and the save lifecycle stays a gap. */
	const Json project = FindCapability(capabilities, "world.project");
	REQUIRE(!project.empty());
	CHECK(Contains(project["implemented"], "catchment"));
	CHECK(Contains(project["implemented"], "catchment_upgrade"));
	CHECK(Contains(project["implemented"], "settings"));
	CHECK(Contains(project["gaps"], "save"));
	CHECK(Contains(project["gaps"], "load"));
}

TEST_CASE("Agent admin tools: extended mutation classification")
{
	/* The query operations of the new families do not change the world. */
	CHECK_FALSE(IsAdminMutation("world.industry", Json::object({{"op", "list"}})));
	CHECK_FALSE(IsAdminMutation("world.industry", Json::object({{"op", "types"}})));
	CHECK_FALSE(IsAdminMutation("world.industry", Json::object({{"op", "info"}})));
	CHECK_FALSE(IsAdminMutation("world.industry", Json::object({{"op", "needs"}})));
	CHECK_FALSE(IsAdminMutation("world.industry", Json::object({{"op", "production"}})));
	CHECK_FALSE(IsAdminMutation("world.industry", Json::object({{"op", "cargo"}})));
	CHECK_FALSE(IsAdminMutation("world.signs", Json::object({{"op", "list"}})));
	CHECK_FALSE(IsAdminMutation("world.signs", Json::object({{"op", "info"}})));
	CHECK_FALSE(IsAdminMutation("world.landscape", Json::object({{"op", "object_types"}})));
	CHECK_FALSE(IsAdminMutation("world.company", Json::object({{"op", "livery"}})));
	CHECK_FALSE(IsAdminMutation("world.towns", Json::object({{"op", "list"}})));
	CHECK_FALSE(IsAdminMutation("world.towns", Json::object({{"op", "growth"}})));
	CHECK_FALSE(IsAdminMutation("world.towns", Json::object({{"op", "settings"}})));
	CHECK_FALSE(IsAdminMutation("world.project", Json::object({{"op", "catchment"}})));
	CHECK_FALSE(IsAdminMutation("world.project", Json::object({{"op", "settings"}})));

	/* Every write operation of the new families is a mutation. */
	CHECK(IsAdminMutation("world.industry", Json::object({{"op", "fund"}})));
	CHECK(IsAdminMutation("world.industry", Json::object({{"op", "fund_area"}})));
	CHECK(IsAdminMutation("world.industry", Json::object({{"op", "prospect"}})));
	CHECK(IsAdminMutation("world.industry", Json::object({{"op", "set_production"}})));
	CHECK(IsAdminMutation("world.industry", Json::object({{"op", "set_flags"}})));
	CHECK(IsAdminMutation("world.industry", Json::object({{"op", "set_exclusivity"}})));
	CHECK(IsAdminMutation("world.signs", Json::object({{"op", "place"}})));
	CHECK(IsAdminMutation("world.signs", Json::object({{"op", "rename"}})));
	CHECK(IsAdminMutation("world.signs", Json::object({{"op", "delete"}})));
	CHECK(IsAdminMutation("world.signs", Json::object({{"op", "move"}})));
	CHECK(IsAdminMutation("world.landscape", Json::object({{"op", "object"}})));
	CHECK(IsAdminMutation("world.landscape", Json::object({{"op", "tree"}})));
	CHECK(IsAdminMutation("world.cheat", Json::object({{"op", "money"}})));
	CHECK(IsAdminMutation("world.cheat", Json::object({{"op", "bank_balance"}})));
	CHECK(IsAdminMutation("world.towns", Json::object({{"op", "found"}})));
	CHECK(IsAdminMutation("world.towns", Json::object({{"op", "rename"}})));
	CHECK(IsAdminMutation("world.towns", Json::object({{"op", "town_action"}})));
	CHECK(IsAdminMutation("world.project", Json::object({{"op", "catchment_upgrade"}})));

	/* An unknown operation of a write family is treated as a mutation, so the write gate is never skipped. */
	CHECK(IsAdminMutation("world.industry", Json::object({{"op", "no_such_op"}})));
	/* A call without an operation may write, so it is classified as a mutation. */
	CHECK(IsAdminMutation("world.industry", Json::object()));
	CHECK(IsAdminMutation("world.cheat", Json::object()));

	/* The methods of the other modules stay non-mutations of this one. */
	CHECK_FALSE(IsAdminMutation("world.construct", Json::object()));
	CHECK_FALSE(IsAdminMutation("world.query", Json::object()));
}

TEST_CASE("Agent admin tools: industry validation")
{
	/* The industry list runs on the empty pool of the test binary. */
	const Json list = Dispatch("world.industry", Json::object({{"op", "list"}}));
	REQUIRE(list["ok"] == true);
	CHECK(list["industries"].is_array());
	CHECK(list["industries"].empty());
	CHECK(list["types"].is_array());

	/* The list is bounded, so one call can never return the whole world. */
	CHECK(list["total"] == 0);
	CHECK(list["offset"] == 0);
	CHECK(list["limit"] == 64);
	CHECK(Dispatch("world.industry", Json::object({{"op", "list"}, {"limit", 0}}))["error"] == "invalid_arguments");
	CHECK(Dispatch("world.industry", Json::object({{"op", "list"}, {"limit", 100000}}))["error"] == "invalid_arguments");
	CHECK(Dispatch("world.industry", Json::object({{"op", "list"}, {"offset", -1}}))["error"] == "invalid_arguments");
	const Json windowed = Dispatch("world.industry", Json::object({{"op", "list"}, {"limit", 2}, {"offset", 1}}));
	REQUIRE(windowed["ok"] == true);
	CHECK(windowed["limit"] == 2);
	CHECK(windowed["offset"] == 1);

	/* A query needs an industry id, and the world of the test binary has no industry. */
	CHECK(Dispatch("world.industry", Json::object({{"op", "needs"}}))["error"] == "invalid_arguments");
	CHECK(Dispatch("world.industry", Json::object({{"op", "cargo"}, {"industry_id", "x"}}))["error"] == "invalid_arguments");
	CHECK(Dispatch("world.industry", Json::object({{"op", "info"}, {"industry_id", 99999}}))["error"] == "unknown_industry");
	const Json unknown = Dispatch("world.industry", Json::object({{"op", "production"}, {"industry_id", 0}}));
	CHECK(unknown["ok"] == false);
	CHECK(unknown["error"] == "unknown_industry");

	/* Funding needs a type the current game offers, and the type is validated before the tile. */
	CHECK(Dispatch("world.industry", Json::object({{"op", "fund"}}))["error"] == "invalid_arguments");
	const Json bad_type = Dispatch("world.industry", Json::object({{"op", "fund"}, {"industry_type", 9999}, {"tile", Json{{"x", 1}, {"y", 1}}}}));
	CHECK(bad_type["ok"] == false);
	CHECK(bad_type["error"] == "unknown_industry_type");
	CHECK(Dispatch("world.industry", Json::object({{"op", "prospect"}}))["error"] == "invalid_arguments");
	CHECK(Dispatch("world.industry", Json::object({{"op", "fund_area"}, {"industry_type", 9999}}))["error"] == "unknown_industry_type");

	/* The control commands need an existing industry. */
	CHECK(Dispatch("world.industry", Json::object({{"op", "set_production"}, {"industry_id", 0}, {"prod_level", 32}}))["error"] == "unknown_industry");
	CHECK(Dispatch("world.industry", Json::object({{"op", "set_flags"}, {"industry_id", 0}, {"flags", Json::array({"no_closure"})}}))["error"] == "unknown_industry");
	CHECK(Dispatch("world.industry", Json::object({{"op", "set_exclusivity"}, {"industry_id", 0}, {"consumer", true}}))["error"] == "unknown_industry");
	CHECK(Dispatch("world.industry", Json::object({{"op", "set_production"}, {"prod_level", 32}}))["error"] == "invalid_arguments");
}

TEST_CASE("Agent admin tools: signs landscape and cheat validation")
{
	/* The sign list runs on the empty pool of the test binary. */
	const Json signs = Dispatch("world.signs", Json::object({{"op", "list"}}));
	REQUIRE(signs["ok"] == true);
	CHECK(signs["signs"].is_array());
	CHECK(signs["signs"].empty());
	CHECK(signs["total"] == 0);
	CHECK(Dispatch("world.signs", Json::object({{"op", "list"}, {"limit", 0}}))["error"] == "invalid_arguments");

	/* A sign id is required and must exist. */
	CHECK(Dispatch("world.signs", Json::object({{"op", "info"}}))["error"] == "invalid_arguments");
	const Json unknown_sign = Dispatch("world.signs", Json::object({{"op", "info"}, {"sign_id", 0}}));
	CHECK(unknown_sign["ok"] == false);
	CHECK(unknown_sign["error"] == "unknown_sign");

	/* The tile and the text length are validated before the sign command is posted. */
	CHECK(Dispatch("world.signs", Json::object({{"op", "place"}}))["error"] == "invalid_arguments");
	const Json long_text = Dispatch("world.signs", Json::object({{"op", "place"}, {"tile", Json{{"x", 1}, {"y", 1}}},
			{"text", "a sign text that is far longer than the limit"}}));
	CHECK(long_text["ok"] == false);
	CHECK(long_text["error"] == "invalid_arguments");
	CHECK(long_text["detail"]["max"] == MAX_LENGTH_SIGN_NAME_CHARS - 1);

	/* Renaming needs a text, and the sign must exist before the text is checked. */
	CHECK(Dispatch("world.signs", Json::object({{"op", "rename"}, {"sign_id", 0}}))["error"] == "unknown_sign");
	CHECK(Dispatch("world.signs", Json::object({{"op", "delete"}}))["error"] == "invalid_arguments");
	CHECK(Dispatch("world.signs", Json::object({{"op", "move"}, {"sign_id", 0}}))["error"] == "unknown_sign");

	/* The object types can be listed; building one needs a valid type and a tile. */
	const Json object_types = Dispatch("world.landscape", Json::object({{"op", "object_types"}}));
	REQUIRE(object_types["ok"] == true);
	CHECK(object_types["types"].is_array());
	CHECK(object_types["total"].is_number_integer());
	CHECK(Dispatch("world.landscape", Json::object({{"op", "object_types"}, {"limit", 0}}))["error"] == "invalid_arguments");
	CHECK(Dispatch("world.landscape", Json::object({{"op", "object"}}))["error"] == "invalid_arguments");
	const Json bad_object = Dispatch("world.landscape", Json::object({{"op", "object"}, {"object_type", 999999}, {"tile", Json{{"x", 1}, {"y", 1}}}}));
	CHECK(bad_object["ok"] == false);
	CHECK(bad_object["error"] == "unknown_object_type");

	/* Planting trees needs a tile, and the tree type is range checked before the command is posted. */
	CHECK(Dispatch("world.landscape", Json::object({{"op", "tree"}}))["error"] == "invalid_arguments");
	const Json bad_tree = Dispatch("world.landscape", Json::object({{"op", "tree"}, {"tile", Json{{"x", 1}, {"y", 1}}}, {"tree_type", 300}}));
	CHECK(bad_tree["ok"] == false);
	CHECK(bad_tree["error"] == "invalid_arguments");
	CHECK(Dispatch("world.landscape", Json::object({{"op", "tree"}, {"tile", Json{{"x", -1}, {"y", 1}}}}))["error"] == "invalid_arguments");

	/* The money cheat needs a positive amount, and a bank balance needs a real company. */
	CHECK(Dispatch("world.cheat", Json::object({{"op", "money"}}))["error"] == "invalid_arguments");
	CHECK(Dispatch("world.cheat", Json::object({{"op", "money"}, {"amount", 0}}))["error"] == "invalid_amount");
	CHECK(Dispatch("world.cheat", Json::object({{"op", "money"}, {"amount", -5}}))["error"] == "invalid_amount");
	const Json balance = Dispatch("world.cheat", Json::object({{"op", "bank_balance"}, {"delta", 100}}));
	CHECK(balance["ok"] == false);
	CHECK(balance["error"] == "unknown_company");
}

TEST_CASE("Agent admin tools: town and company identity validation")
{
	/* The town list runs on the empty pool of the test binary. */
	const Json towns = Dispatch("world.towns", Json::object({{"op", "list"}}));
	REQUIRE(towns["ok"] == true);
	CHECK(towns["towns"].is_array());
	CHECK(towns["towns"].empty());
	CHECK(towns["total"] == 0);
	CHECK(Dispatch("world.towns", Json::object({{"op", "list"}, {"limit", 100000}}))["error"] == "invalid_arguments");

	/* The town queries need an existing town. */
	CHECK(Dispatch("world.towns", Json::object({{"op", "growth"}}))["error"] == "invalid_arguments");
	CHECK(Dispatch("world.towns", Json::object({{"op", "settings"}, {"town_id", 0}}))["error"] == "unknown_town");
	CHECK(Dispatch("world.towns", Json::object({{"op", "power"}, {"town_id", 0}}))["error"] == "unknown_town");

	/* Founding needs a tile, and the size and layout names are checked before the command is posted. */
	CHECK(Dispatch("world.towns", Json::object({{"op", "found"}}))["error"] == "invalid_arguments");
	const Json bad_size = Dispatch("world.towns", Json::object({{"op", "found"}, {"tile", Json{{"x", 1}, {"y", 1}}}, {"size", "huge"}}));
	CHECK(bad_size["ok"] == false);
	CHECK(bad_size["error"] == "invalid_arguments");
	CHECK(bad_size["detail"]["allowed"].is_array());
	const Json bad_layout = Dispatch("world.towns", Json::object({{"op", "found"}, {"tile", Json{{"x", 1}, {"y", 1}}}, {"layout", "spiral"}}));
	CHECK(bad_layout["ok"] == false);
	CHECK(bad_layout["error"] == "invalid_arguments");
	const Json long_name = Dispatch("world.towns", Json::object({{"op", "found"}, {"tile", Json{{"x", 1}, {"y", 1}}},
			{"name", "a town name that is far longer than the limit"}}));
	CHECK(long_name["ok"] == false);
	CHECK(long_name["error"] == "invalid_arguments");
	CHECK(long_name["detail"]["max"] == MAX_LENGTH_TOWN_NAME_CHARS - 1);

	/* Renaming and the town actions need an existing town. */
	CHECK(Dispatch("world.towns", Json::object({{"op", "rename"}, {"town_id", 0}, {"text", "New name"}}))["error"] == "unknown_town");
	CHECK(Dispatch("world.towns", Json::object({{"op", "town_action"}, {"town_id", 0}, {"action", "bribe"}}))["error"] == "unknown_town");
	const Json bad_action = Dispatch("world.towns", Json::object({{"op", "town_action"}, {"town_id", 0}, {"action", "teleport"}}));
	CHECK(bad_action["ok"] == false);
	CHECK(bad_action["error"] == "unknown_town");

	/* The company identity operations need an existing company. */
	CHECK(Dispatch("world.company", Json::object({{"op", "livery"}}))["error"] == "unknown_company");
	CHECK(Dispatch("world.company", Json::object({{"op", "colour"}, {"colour", "red"}}))["error"] == "unknown_company");
	CHECK(Dispatch("world.company", Json::object({{"op", "livery_set"}, {"scheme", "steam"}, {"colour", "red"}}))["error"] == "unknown_company");
	CHECK(Dispatch("world.company", Json::object({{"op", "manager_face"}, {"style", 0}, {"bits", 0}}))["error"] == "unknown_company");
	CHECK(Dispatch("world.company", Json::object({{"op", "rename"}, {"text", "Renamed"}}))["error"] == "unknown_company");
	CHECK(Dispatch("world.company", Json::object({{"op", "rename_president"}, {"text", "Renamed"}}))["error"] == "unknown_company");

	/* Buying a company needs the target, and a company cannot buy itself. */
	CHECK(Dispatch("world.company", Json::object({{"op", "buy"}}))["error"] == "invalid_arguments");
	CHECK(Dispatch("world.company", Json::object({{"op", "buy"}, {"company_id", 0}}))["error"] == "unknown_company");

	/* The catchment query lists the stations of the empty world and needs an id for one station. */
	const Json catchment = Dispatch("world.project", Json::object({{"op", "catchment"}}));
	REQUIRE(catchment["ok"] == true);
	CHECK(catchment["stations"].is_array());
	CHECK(catchment["stations"].empty());
	CHECK(catchment["total"] == 0);
	CHECK(Dispatch("world.project", Json::object({{"op", "catchment"}, {"offset", -1}}))["error"] == "invalid_arguments");
	CHECK(Dispatch("world.project", Json::object({{"op", "catchment"}, {"station_id", 0}}))["error"] == "unknown_station");
	CHECK(Dispatch("world.project", Json::object({{"op", "catchment_upgrade"}}))["error"] == "invalid_arguments");

	/* The project settings report the settings that drive the catchment and the town growth. */
	const Json settings = Dispatch("world.project", Json::object({{"op", "settings"}}));
	REQUIRE(settings["ok"] == true);
	CHECK(!FindSetting(settings, "station.modified_catchment").empty());
}

TEST_CASE("Agent admin tools: normal command path for a boolean setting")
{
	/* A boolean game setting goes through the same Post path and is read back from the setting. */
	const Json before = Dispatch("world.settings.get", Json::object({{"names", Json::array({"construction.build_on_slopes"})}}));
	REQUIRE(before["ok"] == true);
	const Json row = FindSetting(before, "construction.build_on_slopes");
	REQUIRE(!row.empty());
	CHECK(row["type"] == "bool");
	REQUIRE(row["value"].is_number_integer());
	const int64_t old_value = row["value"].get<int64_t>();
	const int64_t new_value = old_value == 0 ? 1 : 0;

	const Json changed = Dispatch("world.settings.set", Json::object({{"name", "construction.build_on_slopes"}, {"value", new_value}}));
	REQUIRE(changed["ok"] == true);
	CHECK(changed["changed"] == true);
	CHECK(changed["value"] == new_value);
	CHECK(changed["cost"] == 0);

	const Json restored = Dispatch("world.settings.set", Json::object({{"name", "construction.build_on_slopes"}, {"value", old_value}}));
	REQUIRE(restored["ok"] == true);
	CHECK(restored["value"] == old_value);
}

