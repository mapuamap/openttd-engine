/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/>.
 */

/** @file agent_admin_tools.cpp Administrative and game-settings tool methods of the in-game agent. */

#include "stdafx.h"
#include "agent_admin_tools.h"

#include "cargotype.h"
#include "command_func.h"
#include "company_base.h"
#include "company_cmd.h"
#include "company_func.h"
#include "company_manager_face.h"
#include "core/backup_type.hpp"
#include "core/enum_type.hpp"
#include "economy_cmd.h"
#include "economy_type.h"
#include "gfx_type.h"
#include "industry.h"
#include "industry_cmd.h"
#include "industrytype.h"
#include "livery.h"
#include "map_func.h"
#include "misc_cmd.h"
#include "newgrf_object.h"
#include "object_cmd.h"
#include "object_type.h"
#include "core/random_func.hpp"
#include "settings_cmd.h"
#include "settings_internal.h"
#include "settings_table.h"
#include "settings_type.h"
#include "signs_base.h"
#include "signs_cmd.h"
#include "signs_func.h"
#include "station_base.h"
#include "station_cmd.h"
#include "station_type.h"
#include "string_func.h"
#include "strings_func.h"
#include "town.h"
#include "town_cmd.h"
#include "town_districts.h"
#include "town_growth.h"
#include "town_growth_type.h"
#include "town_type.h"
#include "tree_cmd.h"
#include "tree_map.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

#include "table/strings.h"

#include "safeguards.h"

namespace InGameAgent {

namespace {

/** Result of the last posted command, filled in by AgentAdminCommandCallback. */
struct PostedCommand {
	bool captured = false; ///< The engine called the callback, so the command really ran.
	CommandCost cost{}; ///< The result the engine reported.
};

/** The last posted command of this module. The command path is synchronous in single player. */
PostedCommand _posted;

/** Result of the last posted Commands::FoundTown, which also returns the new town. */
struct PostedFoundTown {
	bool captured = false; ///< The engine called the callback, so the command really ran.
	CommandCost cost{}; ///< The result the engine reported.
	Money town_cost = 0; ///< The price of the founded town.
	TownID town = TownID::Invalid(); ///< The new town, or an invalid id.
};

/** The last posted Commands::FoundTown of this module. */
PostedFoundTown _posted_found_town;

/** Result of the last posted Commands::PlaceSign, which also returns the new sign. */
struct PostedPlaceSign {
	bool captured = false; ///< The engine called the callback, so the command really ran.
	CommandCost cost{}; ///< The result the engine reported.
	SignID sign = SignID::Invalid(); ///< The new sign, or an invalid id.
};

/** The last posted Commands::PlaceSign of this module. */
PostedPlaceSign _posted_place_sign;

/** Result of the last posted Commands::BuildIndustryArea, which also returns how many it built. */
struct PostedIndustryArea {
	bool captured = false; ///< The engine called the callback, so the command really ran.
	CommandCost cost{}; ///< The result the engine reported.
	uint32_t count = 0; ///< The number of industries the command built.
};

/** The last posted Commands::BuildIndustryArea of this module. */
PostedIndustryArea _posted_industry_area;

/** Maximum number of setting names accepted by one world.settings.get call. */
constexpr size_t MAX_SETTING_NAMES = 64;
/** Default number of settings returned by one world.settings.get call. */
constexpr size_t DEFAULT_SETTING_LIMIT = 50;
/** Maximum number of settings returned by one world.settings.get call. */
constexpr size_t MAX_SETTING_LIMIT = 500;
/** Default number of rows returned by one bounded list query. */
constexpr size_t DEFAULT_LIST_LIMIT = 64;
/** Maximum number of rows returned by one bounded list query. */
constexpr size_t MAX_LIST_LIMIT = 512;

/**
 * Build an error document.
 * @param code Machine readable error code.
 * @param message Human readable explanation.
 * @param detail Extra machine readable context.
 * @return The result document.
 */
Json MakeError(std::string_view code, std::string_view message, Json detail = Json::object())
{
	Json result = Json::object();
	result["ok"] = false;
	result["error"] = std::string(code);
	result["message"] = std::string(message);
	if (!detail.empty()) result["detail"] = std::move(detail);
	return result;
}

/**
 * Build the document of an operation this module does not implement yet.
 * @param method The method name.
 * @param op The operation name, empty when the whole method is missing.
 * @return The result document.
 */
Json MakeGap(std::string_view method, std::string_view op)
{
	Json detail = Json::object();
	detail["method"] = std::string(method);
	if (!op.empty()) detail["op"] = std::string(op);
	return MakeError(op.empty() ? "unsupported_method" : "unsupported_op",
			"the admin tool module does not implement this yet", std::move(detail));
}

/** State of an optional parameter. */
enum class ParamState {
	Absent, ///< The parameter was not given.
	Invalid, ///< The parameter was given but has the wrong type or value.
	Present, ///< The parameter was given and is usable.
};

/**
 * Parse a whole string as a base 10 integer.
 * @param text The text to parse.
 * @param value Receives the number.
 * @return True when the whole text is one integer.
 */
bool ParseInt64(std::string_view text, int64_t &value)
{
	if (text.empty()) return false;
	const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
	return error == std::errc() && end == text.data() + text.size();
}

/**
 * Read an optional integer parameter.
 * @param params The method parameters.
 * @param key The parameter name.
 * @param value Receives the number.
 * @return Whether the parameter was absent, invalid or usable.
 */
ParamState ReadInt(const Json &params, std::string_view key, int64_t &value)
{
	const auto it = params.find(std::string(key));
	if (it == params.end() || it->is_null()) return ParamState::Absent;
	if (it->is_number_integer()) {
		value = it->get<int64_t>();
		return ParamState::Present;
	}
	if (it->is_number_unsigned()) {
		value = static_cast<int64_t>(it->get<uint64_t>());
		return ParamState::Present;
	}
	if (it->is_number_float()) {
		const double number = it->get<double>();
		if (number != std::floor(number) || number < -9.0e15 || number > 9.0e15) return ParamState::Invalid;
		value = static_cast<int64_t>(number);
		return ParamState::Present;
	}
	if (it->is_string()) return ParseInt64(it->get<std::string>(), value) ? ParamState::Present : ParamState::Invalid;
	return ParamState::Invalid;
}

/**
 * Read an optional string parameter.
 * @param params The method parameters.
 * @param key The parameter name.
 * @param value Receives the text.
 * @return Whether the parameter was absent, invalid or usable.
 */
ParamState ReadString(const Json &params, std::string_view key, std::string &value)
{
	const auto it = params.find(std::string(key));
	if (it == params.end() || it->is_null()) return ParamState::Absent;
	if (!it->is_string()) return ParamState::Invalid;
	value = it->get<std::string>();
	return ParamState::Present;
}

/**
 * Read an optional boolean parameter.
 * @param params The method parameters.
 * @param key The parameter name.
 * @param value Receives the flag.
 * @return Whether the parameter was absent, invalid or usable.
 */
ParamState ReadBool(const Json &params, std::string_view key, bool &value)
{
	const auto it = params.find(std::string(key));
	if (it == params.end() || it->is_null()) return ParamState::Absent;
	if (it->is_boolean()) {
		value = it->get<bool>();
		return ParamState::Present;
	}
	if (it->is_number_integer()) {
		const int64_t number = it->get<int64_t>();
		if (number != 0 && number != 1) return ParamState::Invalid;
		value = number != 0;
		return ParamState::Present;
	}
	if (it->is_string()) {
		const std::optional<bool> parsed = BoolSettingDesc::ParseSingleValue(it->get<std::string>());
		if (!parsed.has_value()) return ParamState::Invalid;
		value = *parsed;
		return ParamState::Present;
	}
	return ParamState::Invalid;
}

/**
 * Read an optional `{x, y}` tile object.
 * @param params The method parameters.
 * @param key The parameter name.
 * @param tile Receives the tile.
 * @return Whether the parameter was absent, invalid or usable.
 */
ParamState ReadTile(const Json &params, std::string_view key, TileIndex &tile)
{
	const auto it = params.find(std::string(key));
	if (it == params.end() || it->is_null()) return ParamState::Absent;
	if (!it->is_object()) return ParamState::Invalid;
	int64_t x = 0;
	int64_t y = 0;
	if (ReadInt(*it, "x", x) != ParamState::Present || ReadInt(*it, "y", y) != ParamState::Present) return ParamState::Invalid;
	if (x < 0 || y < 0 || x >= static_cast<int64_t>(Map::SizeX()) || y >= static_cast<int64_t>(Map::SizeY())) return ParamState::Invalid;
	tile = TileXY(static_cast<uint>(x), static_cast<uint>(y));
	return ParamState::Present;
}

/**
 * Read the `limit` and `offset` of a bounded list query.
 *
 * Every list query of this module is bounded, so one call can never return the whole world.
 *
 * @param params The method parameters.
 * @param offset Receives the number of rows to skip.
 * @param limit Receives the maximum number of rows to return.
 * @return An error document, or nothing when the window is usable.
 */
std::optional<Json> ReadListWindow(const Json &params, size_t &offset, size_t &limit)
{
	int64_t requested = static_cast<int64_t>(DEFAULT_LIST_LIMIT);
	switch (ReadInt(params, "limit", requested)) {
		case ParamState::Invalid: return MakeError("invalid_arguments", "`limit` must be an integer");
		case ParamState::Present:
			if (requested < 1 || requested > static_cast<int64_t>(MAX_LIST_LIMIT)) {
				return MakeError("invalid_arguments", "`limit` is out of range", {{"min", 1}, {"max", MAX_LIST_LIMIT}});
			}
			break;
		case ParamState::Absent: break;
	}
	limit = static_cast<size_t>(requested);

	int64_t skipped = 0;
	switch (ReadInt(params, "offset", skipped)) {
		case ParamState::Invalid: return MakeError("invalid_arguments", "`offset` must be an integer");
		case ParamState::Present:
			if (skipped < 0) return MakeError("invalid_arguments", "`offset` must not be negative");
			break;
		case ParamState::Absent: break;
	}
	offset = static_cast<size_t>(skipped);
	return std::nullopt;
}

/**
 * Check that a company exists.
 * @param company The company to check.
 * @return An error document, or nothing when the company is usable.
 */
std::optional<Json> CheckCompany(CompanyID company)
{
	if (!Company::IsValidID(company)) {
		return MakeError("unknown_company", "there is no company with that id", {{"company_id", company.base()}});
	}
	return std::nullopt;
}

/**
 * Check that a town exists.
 * @param town The town to check.
 * @return An error document, or nothing when the town is usable.
 */
std::optional<Json> CheckTown(TownID town)
{
	if (!Town::IsValidID(town)) {
		return MakeError("unknown_town", "there is no town with that id", {{"town_id", town.base()}});
	}
	return std::nullopt;
}

/**
 * Read an optional company id parameter.
 * @param params The method parameters.
 * @param key The parameter name.
 * @param company Receives the company.
 * @return An error document, or nothing when the parameter is usable or absent.
 */
std::optional<Json> ReadCompanyId(const Json &params, std::string_view key, CompanyID &company)
{
	int64_t id = 0;
	switch (ReadInt(params, key, id)) {
		case ParamState::Absent:
			return std::nullopt;
		case ParamState::Invalid:
			return MakeError("invalid_arguments", "`" + std::string(key) + "` must be a company id");
		case ParamState::Present:
			break;
	}
	if (id < 0 || id >= MAX_COMPANIES) {
		return MakeError("unknown_company", "the company id is out of range", {{"company_id", id}, {"max", MAX_COMPANIES - 1}});
	}
	company = CompanyID(static_cast<uint8_t>(id));
	return std::nullopt;
}

/**
 * Read an optional town id parameter.
 * @param params The method parameters.
 * @param key The parameter name.
 * @param town Receives the town.
 * @return An error document, or nothing when the parameter is usable or absent.
 */
std::optional<Json> ReadTownId(const Json &params, std::string_view key, TownID &town)
{
	int64_t id = 0;
	switch (ReadInt(params, key, id)) {
		case ParamState::Absent:
			return std::nullopt;
		case ParamState::Invalid:
			return MakeError("invalid_arguments", "`" + std::string(key) + "` must be a town id");
		case ParamState::Present:
			break;
	}
	if (id < 0 || id >= static_cast<int64_t>(TownID::End().base())) {
		return MakeError("unknown_town", "the town id is out of range", {{"town_id", id}});
	}
	town = TownID(static_cast<uint16_t>(id));
	return std::nullopt;
}

/**
 * Post one command through the normal player command path and remember its result.
 * @param args The command arguments.
 * @return True when the engine executed the command and it succeeded.
 */
template <Commands Tcmd, typename... Targs>
bool RunCommand(Targs... args)
{
	_posted = PostedCommand{};
	Command<Tcmd>::Post(STR_NULL, &AgentAdminCommandCallback, std::forward<Targs>(args)...);
	return _posted.captured && _posted.cost.Succeeded();
}

/**
 * Post one command that has no tile argument of its own, with the tile the engine reports it on.
 *
 * Commands with CommandFlag::Location take no TileIndex themselves, so the engine needs the
 * location of the command for its error window; this is the Post overload for them.
 *
 * @param location The tile the command is reported on.
 * @param args The command arguments.
 * @return True when the engine executed the command and it succeeded.
 */
template <Commands Tcmd, typename... Targs>
bool RunCommandAt(TileIndex location, Targs... args)
{
	_posted = PostedCommand{};
	Command<Tcmd>::Post(STR_NULL, &AgentAdminCommandCallback, location, std::forward<Targs>(args)...);
	return _posted.captured && _posted.cost.Succeeded();
}

/**
 * Post Commands::FoundTown and remember the new town it returns.
 * @param args The command arguments.
 * @return True when the engine executed the command and it succeeded.
 */
template <Commands Tcmd, typename... Targs>
bool RunFoundTown(Targs... args)
{
	_posted_found_town = PostedFoundTown{};
	Command<Tcmd>::Post(STR_NULL, &AgentAdminFoundTownCallback, std::forward<Targs>(args)...);
	return _posted_found_town.captured && _posted_found_town.cost.Succeeded();
}

/**
 * Post Commands::PlaceSign and remember the new sign it returns.
 * @param args The command arguments.
 * @return True when the engine executed the command and it succeeded.
 */
template <Commands Tcmd, typename... Targs>
bool RunPlaceSign(Targs... args)
{
	_posted_place_sign = PostedPlaceSign{};
	Command<Tcmd>::Post(STR_NULL, &AgentAdminPlaceSignCallback, std::forward<Targs>(args)...);
	return _posted_place_sign.captured && _posted_place_sign.cost.Succeeded();
}

/**
 * Post Commands::BuildIndustryArea and remember how many industries it built.
 * @param args The command arguments.
 * @return True when the engine executed the command and it succeeded.
 */
template <Commands Tcmd, typename... Targs>
bool RunBuildIndustryArea(Targs... args)
{
	_posted_industry_area = PostedIndustryArea{};
	Command<Tcmd>::Post(STR_NULL, &AgentAdminBuildIndustryAreaCallback, std::forward<Targs>(args)...);
	return _posted_industry_area.captured && _posted_industry_area.cost.Succeeded();
}

/**
 * Add the cost and the engine error of one command result to a result document.
 * @param result The result document to complete.
 * @param cost The result the engine reported.
 * @return The completed document.
 */
Json AddCommandResult(Json result, const CommandCost &cost)
{
	result["cost"] = static_cast<int64_t>(cost.GetCost());
	if (cost.Failed()) {
		result["ok"] = false;
		result["error"] = "command_failed";
		const StringID message = cost.GetErrorMessage();
		if (message != INVALID_STRING_ID) {
			result["message_id"] = message.base();
			result["message"] = GetString(message);
		} else {
			result["message"] = "the engine rejected the command";
		}
	} else {
		result["ok"] = true;
	}
	return result;
}

/**
 * Add the cost and the engine error of the last command to a result document.
 * @param result The result document to complete.
 * @return The completed document.
 */
Json AddCommandResult(Json result)
{
	return AddCommandResult(std::move(result), _posted.cost);
}

/**
 * Report that the engine did not run the command at all.
 * @param result The result document to complete.
 * @return The completed document.
 */
Json AddNotExecuted(Json result)
{
	result["ok"] = false;
	result["error"] = "command_not_executed";
	result["message"] = "the engine did not execute the command; it was only estimated, the game is paused, or the command was rejected before dispatch";
	return result;
}

/**
 * Name of a setting scope for the protocol.
 * @param type The setting type.
 * @return The scope name.
 */
std::string_view ScopeName(SettingType type)
{
	switch (type) {
		case ST_GAME: return "game";
		case ST_COMPANY: return "company";
		case ST_CLIENT: return "client";
		default: return "unknown";
	}
}

/**
 * Name of a setting category for the protocol.
 * @param category The setting category.
 * @return The category name.
 */
std::string_view CategoryName(SettingCategory category)
{
	switch (category) {
		case SC_BASIC: return "basic";
		case SC_ADVANCED: return "advanced";
		case SC_EXPERT: return "expert";
		default: return "none";
	}
}

/**
 * The object a setting of the given scope lives in.
 * @param sd The setting.
 * @param company The company for per-company settings; must be valid then.
 * @return The object the setting is read from and written to.
 */
const void *SettingObject(const SettingDesc &sd, CompanyID company)
{
	switch (sd.GetType()) {
		case ST_GAME: return &GetGameSettings();
		case ST_COMPANY: return &Company::Get(company)->settings;
		case ST_CLIENT: return &_settings_client;
		default: NOT_REACHED();
	}
}

/**
 * Turn the allowed values of a one-of-many setting into a JSON array.
 * @param many The allowed values.
 * @return The array of names.
 */
Json ManyNames(const std::vector<std::string_view> &many)
{
	Json names = Json::array();
	for (const std::string_view name : many) names.push_back(std::string(name));
	return names;
}

/**
 * Describe one setting: its current value and the values it accepts.
 * @param sd The setting.
 * @param object The object the setting lives in.
 * @return The description.
 */
Json DescribeSetting(const SettingDesc &sd, const void *object)
{
	Json row = Json::object();
	row["name"] = sd.GetName();
	row["scope"] = ScopeName(sd.GetType());
	row["editable"] = sd.IsEditable(true);
	row["newgame_only"] = sd.flags.Test(SettingFlag::NewgameOnly);
	row["sandbox"] = sd.flags.Test(SettingFlag::Sandbox);
	row["formatted"] = sd.FormatValue(object);

	if (sd.IsIntSetting()) {
		const IntSettingDesc &isd = *sd.AsIntSetting();
		const auto [min_value, max_value] = isd.GetRange();
		row["value"] = isd.Read(object);
		row["min"] = min_value;
		row["max"] = max_value;
		row["default"] = isd.GetDefaultValue();
		row["category"] = CategoryName(isd.cat);
		if (isd.IsBoolSetting()) {
			row["type"] = "bool";
		} else if (const auto *many = dynamic_cast<const ManyOfManySettingDesc *>(&isd); many != nullptr) {
			row["type"] = "many_of_many";
			row["allowed"] = ManyNames(many->many);
		} else if (const auto *one = dynamic_cast<const OneOfManySettingDesc *>(&isd); one != nullptr) {
			row["type"] = "one_of_many";
			row["allowed"] = ManyNames(one->many);
		} else {
			row["type"] = "int";
		}
	} else if (sd.IsStringSetting()) {
		row["type"] = "string";
		row["value"] = sd.AsStringSetting()->Read(object);
	} else {
		row["type"] = "list";
	}
	return row;
}

/**
 * Every setting descriptor this module can see, in a stable order.
 * @return The descriptors.
 */
std::vector<const SettingDesc *> AllSettings()
{
	std::vector<const SettingDesc *> settings = GetFilteredSettingCollection([](const SettingDesc &sd) {
		return SlIsObjectCurrentlyValid(sd.save.version_from, sd.save.version_to);
	});
	for (const SettingVariant &desc : _company_settings) {
		const SettingDesc *sd = GetSettingDesc(desc);
		if (SlIsObjectCurrentlyValid(sd->save.version_from, sd->save.version_to)) settings.push_back(sd);
	}
	return settings;
}

/**
 * Turn a JSON value into the integer a setting accepts, checking the allowed range.
 * @param sd The setting.
 * @param value The value as sent by the caller.
 * @param parsed Receives the value to write.
 * @return An error document, or nothing when the value is usable.
 */
std::optional<Json> ParseSettingValue(const SettingDesc &sd, const Json &value, int32_t &parsed)
{
	const IntSettingDesc &isd = *sd.AsIntSetting();
	const auto [min_value, max_value] = isd.GetRange();
	const Json detail = {{"name", sd.GetName()}, {"min", min_value}, {"max", max_value}};

	if (isd.IsBoolSetting()) {
		std::optional<bool> parsed_bool;
		if (value.is_boolean()) {
			parsed_bool = value.get<bool>();
		} else if (value.is_number_integer()) {
			const int64_t number = value.get<int64_t>();
			if (number == 0 || number == 1) parsed_bool = number != 0;
		} else if (value.is_string()) {
			parsed_bool = BoolSettingDesc::ParseSingleValue(value.get<std::string>());
		}
		if (!parsed_bool.has_value()) return MakeError("invalid_value", "the value is not a boolean", detail);
		parsed = *parsed_bool ? 1 : 0;
		return std::nullopt;
	}

	if (dynamic_cast<const ManyOfManySettingDesc *>(&isd) != nullptr) {
		/* A combination of flags: only the bit mask itself is accepted. */
		int64_t number = 0;
		if (value.is_number_integer()) {
			number = value.get<int64_t>();
		} else if (value.is_string() && !ParseInt64(value.get<std::string>(), number)) {
			return MakeError("invalid_value", "a many-of-many setting takes the bit mask as an integer", detail);
		} else if (!value.is_string()) {
			return MakeError("invalid_value", "a many-of-many setting takes the bit mask as an integer", detail);
		}
		if (number < min_value || number > static_cast<int64_t>(max_value)) {
			return MakeError("value_out_of_range", "the value is outside the allowed range", detail);
		}
		parsed = static_cast<int32_t>(number);
		return std::nullopt;
	}

	if (const auto *one = dynamic_cast<const OneOfManySettingDesc *>(&isd); one != nullptr) {
		if (value.is_number_integer()) {
			const int64_t number = value.get<int64_t>();
			if (number < min_value || number > static_cast<int64_t>(max_value)) {
				return MakeError("value_out_of_range", "the value is outside the allowed range", detail);
			}
			parsed = static_cast<int32_t>(number);
			return std::nullopt;
		}
		if (value.is_string()) {
			const std::optional<uint32_t> index = OneOfManySettingDesc::ParseSingleValue(value.get<std::string>(), one->many);
			if (!index.has_value()) {
				Json allowed = detail;
				allowed["allowed"] = ManyNames(one->many);
				return MakeError("invalid_value", "the value is not one of the allowed values", std::move(allowed));
			}
			parsed = static_cast<int32_t>(*index);
			return std::nullopt;
		}
		return MakeError("invalid_value", "the value is not one of the allowed values", detail);
	}

	std::optional<int32_t> number;
	if (value.is_number_integer()) {
		const int64_t raw = value.get<int64_t>();
		if (raw < INT32_MIN || raw > INT32_MAX) return MakeError("value_out_of_range", "the value is outside the allowed range", detail);
		number = static_cast<int32_t>(raw);
	} else if (value.is_string()) {
		number = IntSettingDesc::ParseSingleValue(value.get<std::string>(), min_value, max_value);
	}
	if (!number.has_value()) return MakeError("invalid_value", "the value is not an integer", detail);
	if (*number < min_value || *number > static_cast<int64_t>(max_value)) {
		return MakeError("value_out_of_range", "the value is outside the allowed range", detail);
	}
	parsed = *number;
	return std::nullopt;
}

/**
 * Name of a town need for the protocol.
 * @param need The need.
 * @return The name.
 */
std::string_view NeedName(TownNeed need)
{
	switch (need) {
		case TownNeed::Passengers: return "passengers";
		case TownNeed::Mail: return "mail";
		case TownNeed::Goods: return "goods";
		case TownNeed::Electricity: return "electricity";
		case TownNeed::Valuables: return "valuables";
		default: return "unknown";
	}
}

/**
 * Read a town need by name.
 * @param name The name, or "all" for every need.
 * @param need Receives the need.
 * @return True when the name is known.
 */
bool ParseNeedName(std::string_view name, TownNeed &need)
{
	if (name == "all") {
		need = TownNeed::End;
		return true;
	}
	for (TownNeed candidate : EnumRange(TownNeed::End)) {
		if (NeedName(candidate) == name) {
			need = candidate;
			return true;
		}
	}
	return false;
}

/**
 * Name of a town growth status for the protocol.
 * @param status The status.
 * @return The name.
 */
std::string_view GrowthStatusName(TownGrowthStatus status)
{
	switch (status) {
		case TownGrowthStatus::Growing: return "growing";
		case TownGrowthStatus::Blocked: return "blocked";
		case TownGrowthStatus::NoSpace: return "no_space";
		case TownGrowthStatus::Disabled: return "disabled";
		case TownGrowthStatus::Custom: return "custom";
		default: return "unknown";
	}
}

/**
 * Name of a town acceptance effect for the protocol.
 * @param effect The effect.
 * @return The name.
 */
std::string_view AcceptanceEffectName(TownAcceptanceEffect effect)
{
	switch (effect) {
		case TownAcceptanceEffect::None: return "none";
		case TownAcceptanceEffect::Passengers: return "passengers";
		case TownAcceptanceEffect::Mail: return "mail";
		case TownAcceptanceEffect::Goods: return "goods";
		case TownAcceptanceEffect::Water: return "water";
		case TownAcceptanceEffect::Food: return "food";
		default: return "unknown";
	}
}

/**
 * Read a town acceptance effect by name.
 * @param name The name.
 * @param effect Receives the effect.
 * @return True when the name is known.
 */
bool ParseAcceptanceEffectName(std::string_view name, TownAcceptanceEffect &effect)
{
	for (TownAcceptanceEffect candidate : EnumRange(TownAcceptanceEffect::End)) {
		if (AcceptanceEffectName(candidate) == name) {
			effect = candidate;
			return true;
		}
	}
	return false;
}

/**
 * The name of a town, custom or generated.
 * @param t The town.
 * @return The name.
 */
std::string TownName(const Town *t)
{
	return t->name.empty() ? GetString(STR_TOWN_NAME, t->index) : t->name;
}

/**
 * The name of a company, custom or generated.
 * @param c The company.
 * @return The name.
 */
std::string CompanyName(const Company *c)
{
	return c->name.empty() ? GetString(c->name_1, c->name_2) : c->name;
}

/**
 * The name of the president of a company, custom or generated.
 * @param c The company.
 * @return The name.
 */
std::string PresidentName(const Company *c)
{
	return c->president_name.empty() ? GetString(c->president_name_1, c->president_name_2) : c->president_name;
}

/**
 * The finance and context of one company.
 * @param company The company; must be valid.
 * @return The description.
 */
Json CompanyInfo(CompanyID company)
{
	const Company *c = Company::Get(company);
	Json info = Json::object();
	info["company_id"] = company.base();
	info["name"] = CompanyName(c);
	info["president_name"] = PresidentName(c);
	info["colour"] = to_underlying(c->colour);
	info["is_ai"] = c->is_ai;
	info["is_local"] = company == _local_company;
	info["is_current"] = company == _current_company;
	info["money"] = static_cast<int64_t>(c->money);
	info["loan"] = static_cast<int64_t>(c->current_loan);
	info["max_loan"] = static_cast<int64_t>(c->GetMaxLoan());
	info["loan_interval"] = LOAN_INTERVAL;
	info["company_value"] = static_cast<int64_t>(c->cur_economy.company_value);
	info["income"] = static_cast<int64_t>(c->cur_economy.income);
	info["expenses"] = static_cast<int64_t>(c->cur_economy.expenses);
	info["performance_history"] = c->cur_economy.performance_history;
	info["months_of_bankruptcy"] = c->months_of_bankruptcy;
	info["inaugurated_year"] = c->inaugurated_year.base();
	return info;
}

/**
 * The growth state of one town: the needs, the investment and the power links.
 * @param town The town; must be valid.
 * @return The description.
 */
Json TownNeeds(TownID town_id)
{
	const Town *t = Town::Get(town_id);
	Json result = Json::object();
	result["town_id"] = town_id.base();
	result["name"] = TownName(t);
	result["population"] = t->cache.population;
	result["houses"] = t->cache.num_houses;
	result["growth_status"] = GrowthStatusName(t->growth.status);
	result["blocking_need"] = t->growth.blocking_need == TownNeed::End ? Json(nullptr) : Json(NeedName(t->growth.blocking_need));
	result["multiplier"] = t->growth.multiplier;
	result["yearly_growth"] = t->growth.yearly_growth;
	result["budget"] = t->growth.budget;
	result["investment"] = t->growth.investment;
	result["investment_bonus"] = GetTownInvestmentBonus(t);
	result["custom_growth"] = t->flags.Test(TownFlag::CustomGrowth);
	result["growth_rate"] = t->growth_rate;
	result["grow_counter"] = t->grow_counter;
	result["electricity_available"] = IsElectricityAvailable();
	result["power_received"] = GetTownPowerReceived(t);
	result["power_links_wanted"] = GetTownWantedPowerLinks(t);

	Json amounts = Json::array();
	for (const int64_t amount : TownGrowthBalance::INVESTMENT_AMOUNTS) amounts.push_back(amount);
	result["investment_amounts"] = std::move(amounts);

	Json needs = Json::array();
	for (const TownNeed need : EnumRange(TownNeed::End)) {
		const TownNeedState &state = t->growth.needs[need];
		const uint8_t effective = GetTownNeedEffectiveLevel(t, need);
		Json row = Json::object();
		row["need"] = NeedName(need);
		row["level"] = state.level;
		row["effective_level"] = effective;
		row["forced"] = state.forced == TOWN_NEED_NOT_FORCED ? Json(nullptr) : Json(state.forced);
		row["coverage"] = state.coverage;
		row["supplied"] = state.supplied;
		row["demand"] = state.demand;
		row["unlocked"] = IsTownNeedUnlocked(t, need);
		row["unlock_population"] = GetTownNeedUnlockPopulation(need);
		row["bonus"] = IsTownNeedBonus(need);
		row["multiplier"] = GetTownNeedMultiplier(need, effective);
		needs.push_back(std::move(row));
	}
	result["needs"] = std::move(needs);

	Json links = Json::array();
	for (const TownPowerLink &link : t->growth.power_links) {
		links.push_back(Json{{"plant", link.plant.base()}, {"delivered", link.delivered}});
	}
	result["power_links"] = std::move(links);
	return result;
}

/**
 * The state of one power plant.
 * @param i The industry; must be a power plant.
 * @return The description.
 */
Json PowerPlantInfo(const Industry *i)
{
	Json info = Json::object();
	info["industry_id"] = i->index.base();
	info["name"] = GetString(STR_INDUSTRY_NAME, i->index);
	info["fuel"] = GetPowerPlantFuelStock(i);
	info["output"] = i->power_output;
	info["demand"] = i->power_demand;
	Json towns = Json::array();
	for (const Town *t : Town::Iterate()) {
		for (const TownPowerLink &link : t->growth.power_links) {
			if (link.plant == i->index) towns.push_back(t->index.base());
		}
	}
	info["linked_towns"] = std::move(towns);
	return info;
}

/**
 * Describe one setting by name, for the dependency reports of the query operations.
 * @param name The setting name.
 * @param company The company for per-company settings.
 * @return The description, or an empty object when the setting does not exist.
 */
Json SettingRow(std::string_view name, CompanyID company)
{
	const SettingDesc *sd = GetSettingFromName(name);
	if (sd == nullptr || !SlIsObjectCurrentlyValid(sd->save.version_from, sd->save.version_to)) return Json::object();
	if (sd->GetType() == ST_COMPANY && !Company::IsValidID(company)) return Json::object();
	return DescribeSetting(*sd, SettingObject(*sd, company));
}

/**
 * Describe a list of settings by name, skipping the ones that do not exist.
 * @param names The setting names.
 * @param company The company for per-company settings.
 * @return The descriptions, in the order of the names.
 */
Json SettingRows(std::initializer_list<std::string_view> names, CompanyID company)
{
	Json rows = Json::array();
	for (const std::string_view name : names) {
		Json row = SettingRow(name, company);
		if (!row.empty()) rows.push_back(std::move(row));
	}
	return rows;
}

/**
 * The name of a cargo type.
 * @param cargo The cargo type.
 * @return The name, or "invalid" for an unused slot.
 */
std::string CargoName(CargoType cargo)
{
	if (!IsValidCargoType(cargo)) return "invalid";
	return GetString(CargoSpec::Get(cargo)->name);
}

/**
 * The name of a company colour for the protocol.
 * @param colour The colour.
 * @return The name.
 */
std::string_view ColourName(Colours colour)
{
	switch (colour) {
		case Colours::DarkBlue: return "dark_blue";
		case Colours::PaleGreen: return "pale_green";
		case Colours::Pink: return "pink";
		case Colours::Yellow: return "yellow";
		case Colours::Red: return "red";
		case Colours::LightBlue: return "light_blue";
		case Colours::Green: return "green";
		case Colours::DarkGreen: return "dark_green";
		case Colours::Blue: return "blue";
		case Colours::Cream: return "cream";
		case Colours::Mauve: return "mauve";
		case Colours::Purple: return "purple";
		case Colours::Orange: return "orange";
		case Colours::Brown: return "brown";
		case Colours::Grey: return "grey";
		case Colours::White: return "white";
		default: return "invalid";
	}
}

/**
 * Read a company colour by name or by palette index.
 * @param value The value as sent by the caller.
 * @param colour Receives the colour.
 * @return True when the value is a usable colour.
 */
bool ParseColour(const Json &value, Colours &colour)
{
	if (value.is_number_integer()) {
		const int64_t number = value.get<int64_t>();
		if (number < 0 || number >= static_cast<int64_t>(Colours::End)) return false;
		colour = static_cast<Colours>(number);
		return true;
	}
	if (!value.is_string()) return false;
	const std::string name = value.get<std::string>();
	for (Colours candidate : EnumRange(Colours::End)) {
		if (ColourName(candidate) == name) {
			colour = candidate;
			return true;
		}
	}
	return false;
}

/**
 * The name of a livery scheme for the protocol.
 * @param scheme The scheme.
 * @return The name.
 */
std::string_view LiverySchemeName(LiveryScheme scheme)
{
	switch (scheme) {
		case LiveryScheme::Default: return "default";
		case LiveryScheme::Steam: return "steam";
		case LiveryScheme::Diesel: return "diesel";
		case LiveryScheme::Electric: return "electric";
		case LiveryScheme::Monorail: return "monorail";
		case LiveryScheme::Maglev: return "maglev";
		case LiveryScheme::DMU: return "dmu";
		case LiveryScheme::EMU: return "emu";
		case LiveryScheme::PassengerWagonSteam: return "passenger_wagon_steam";
		case LiveryScheme::PassengerWagonDiesel: return "passenger_wagon_diesel";
		case LiveryScheme::PassengerWagonElectric: return "passenger_wagon_electric";
		case LiveryScheme::PassengerWagonMonorail: return "passenger_wagon_monorail";
		case LiveryScheme::PassengerWagonMaglev: return "passenger_wagon_maglev";
		case LiveryScheme::FreightWagon: return "freight_wagon";
		case LiveryScheme::Bus: return "bus";
		case LiveryScheme::Truck: return "truck";
		case LiveryScheme::PassengerShip: return "passenger_ship";
		case LiveryScheme::FreightShip: return "freight_ship";
		case LiveryScheme::Helicopter: return "helicopter";
		case LiveryScheme::SmallPlane: return "small_plane";
		case LiveryScheme::LargePlane: return "large_plane";
		case LiveryScheme::PassengerTram: return "passenger_tram";
		case LiveryScheme::FreightTram: return "freight_tram";
		default: return "unknown";
	}
}

/**
 * Read a livery scheme by name.
 * @param name The name.
 * @param scheme Receives the scheme.
 * @return True when the name is known.
 */
bool ParseLiverySchemeName(std::string_view name, LiveryScheme &scheme)
{
	for (LiveryScheme candidate : EnumRange(LiveryScheme::End)) {
		if (LiverySchemeName(candidate) == name) {
			scheme = candidate;
			return true;
		}
	}
	return false;
}

/**
 * The name of a town action for the protocol.
 * @param action The action.
 * @return The name.
 */
std::string_view TownActionName(TownAction action)
{
	switch (action) {
		case TownAction::AdvertiseSmall: return "advertise_small";
		case TownAction::AdvertiseMedium: return "advertise_medium";
		case TownAction::AdvertiseLarge: return "advertise_large";
		case TownAction::RoadRebuild: return "road_rebuild";
		case TownAction::BuildStatue: return "build_statue";
		case TownAction::FundBuildings: return "fund_buildings";
		case TownAction::BuyRights: return "buy_rights";
		case TownAction::Bribe: return "bribe";
		default: return "unknown";
	}
}

/**
 * Read a town action by name.
 * @param name The name.
 * @param action Receives the action.
 * @return True when the name is known.
 */
bool ParseTownActionName(std::string_view name, TownAction &action)
{
	for (TownAction candidate : EnumRange(TownAction::End)) {
		if (TownActionName(candidate) == name) {
			action = candidate;
			return true;
		}
	}
	return false;
}

/**
 * The name of a town size for the protocol.
 * @param size The size.
 * @return The name.
 */
std::string_view TownSizeName(TownSize size)
{
	switch (size) {
		case TownSize::Small: return "small";
		case TownSize::Medium: return "medium";
		case TownSize::Large: return "large";
		case TownSize::Random: return "random";
		default: return "unknown";
	}
}

/**
 * Read a town size by name.
 * @param name The name.
 * @param size Receives the size.
 * @return True when the name is known.
 */
bool ParseTownSizeName(std::string_view name, TownSize &size)
{
	for (TownSize candidate : EnumRange(TownSize::End)) {
		if (TownSizeName(candidate) == name) {
			size = candidate;
			return true;
		}
	}
	return false;
}

/**
 * The name of a town layout for the protocol.
 * @param layout The layout.
 * @return The name.
 */
std::string_view TownLayoutName(TownLayout layout)
{
	switch (layout) {
		case TownLayout::Original: return "original";
		case TownLayout::BetterRoads: return "better_roads";
		case TownLayout::Grid2x2: return "grid_2x2";
		case TownLayout::Grid3x3: return "grid_3x3";
		case TownLayout::Random: return "random";
		default: return "unknown";
	}
}

/**
 * Read a town layout by name.
 * @param name The name.
 * @param layout Receives the layout.
 * @return True when the name is known.
 */
bool ParseTownLayoutName(std::string_view name, TownLayout &layout)
{
	for (TownLayout candidate : EnumRange(TownLayout::End)) {
		if (TownLayoutName(candidate) == name) {
			layout = candidate;
			return true;
		}
	}
	return false;
}

/**
 * The name of an industry control flag for the protocol.
 * @param flag The flag.
 * @return The name.
 */
std::string_view IndustryControlFlagName(IndustryControlFlag flag)
{
	switch (flag) {
		case IndustryControlFlag::NoProductionDecrease: return "no_production_decrease";
		case IndustryControlFlag::NoProductionIncrease: return "no_production_increase";
		case IndustryControlFlag::NoClosure: return "no_closure";
		case IndustryControlFlag::ExternalProdLevel: return "external_prod_level";
		default: return "unknown";
	}
}

/**
 * Read an industry control flag by name.
 * @param name The name.
 * @param flag Receives the flag.
 * @return True when the name is known.
 */
bool ParseIndustryControlFlagName(std::string_view name, IndustryControlFlag &flag)
{
	for (IndustryControlFlag candidate : EnumRange(IndustryControlFlag::End)) {
		if (IndustryControlFlagName(candidate) == name) {
			flag = candidate;
			return true;
		}
	}
	return false;
}

/**
 * Read an optional industry id parameter.
 * @param params The method parameters.
 * @param key The parameter name.
 * @param industry Receives the industry.
 * @return An error document, or nothing when the parameter is usable or absent.
 */
std::optional<Json> ReadIndustryId(const Json &params, std::string_view key, IndustryID &industry)
{
	int64_t id = 0;
	switch (ReadInt(params, key, id)) {
		case ParamState::Absent:
			return std::nullopt;
		case ParamState::Invalid:
			return MakeError("invalid_arguments", "`" + std::string(key) + "` must be an industry id");
		case ParamState::Present:
			break;
	}
	if (id < 0 || id >= static_cast<int64_t>(IndustryID::End().base())) {
		return MakeError("unknown_industry", "the industry id is out of range", {{"industry_id", id}});
	}
	industry = IndustryID(static_cast<uint16_t>(id));
	return std::nullopt;
}

/**
 * Read an optional sign id parameter.
 * @param params The method parameters.
 * @param key The parameter name.
 * @param sign Receives the sign.
 * @return An error document, or nothing when the parameter is usable or absent.
 */
std::optional<Json> ReadSignId(const Json &params, std::string_view key, SignID &sign)
{
	int64_t id = 0;
	switch (ReadInt(params, key, id)) {
		case ParamState::Absent:
			return std::nullopt;
		case ParamState::Invalid:
			return MakeError("invalid_arguments", "`" + std::string(key) + "` must be a sign id");
		case ParamState::Present:
			break;
	}
	if (id < 0 || id >= static_cast<int64_t>(SignID::End().base())) {
		return MakeError("unknown_sign", "the sign id is out of range", {{"sign_id", id}});
	}
	sign = SignID(static_cast<uint16_t>(id));
	return std::nullopt;
}

/**
 * Read an optional station id parameter.
 * @param params The method parameters.
 * @param key The parameter name.
 * @param station Receives the station.
 * @return An error document, or nothing when the parameter is usable or absent.
 */
std::optional<Json> ReadStationId(const Json &params, std::string_view key, StationID &station)
{
	int64_t id = 0;
	switch (ReadInt(params, key, id)) {
		case ParamState::Absent:
			return std::nullopt;
		case ParamState::Invalid:
			return MakeError("invalid_arguments", "`" + std::string(key) + "` must be a station id");
		case ParamState::Present:
			break;
	}
	if (id < 0 || id >= static_cast<int64_t>(StationID::End().base())) {
		return MakeError("unknown_station", "the station id is out of range", {{"station_id", id}});
	}
	station = StationID(static_cast<uint16_t>(id));
	return std::nullopt;
}

/**
 * Check that an industry exists.
 * @param industry The industry to check.
 * @return An error document, or nothing when the industry is usable.
 */
std::optional<Json> CheckIndustry(IndustryID industry)
{
	if (!Industry::IsValidID(industry)) {
		return MakeError("unknown_industry", "there is no industry with that id", {{"industry_id", industry.base()}});
	}
	return std::nullopt;
}

/**
 * Check that a sign exists.
 * @param sign The sign to check.
 * @return An error document, or nothing when the sign is usable.
 */
std::optional<Json> CheckSign(SignID sign)
{
	if (!Sign::IsValidID(sign)) {
		return MakeError("unknown_sign", "there is no sign with that id", {{"sign_id", sign.base()}});
	}
	return std::nullopt;
}

/**
 * Check that a station exists.
 * @param station The station to check.
 * @return An error document, or nothing when the station is usable.
 */
std::optional<Json> CheckStation(StationID station)
{
	if (!Station::IsValidID(station)) {
		return MakeError("unknown_station", "there is no station with that id", {{"station_id", station.base()}});
	}
	return std::nullopt;
}

/**
 * The identity and the control state of one industry.
 * @param i The industry; must be valid.
 * @return The description.
 */
Json IndustryInfo(const Industry *i)
{
	Json info = Json::object();
	info["industry_id"] = i->index.base();
	info["name"] = GetString(STR_INDUSTRY_NAME, i->index);
	info["type"] = i->type;
	info["type_name"] = GetString(GetIndustrySpec(i->type)->name);
	info["town_id"] = i->town == nullptr ? Json(nullptr) : Json(i->town->index.base());
	info["town_name"] = i->town == nullptr ? Json(nullptr) : Json(TownName(i->town));
	info["location"] = Json{{"x", TileX(i->location.tile)}, {"y", TileY(i->location.tile)}, {"w", i->location.w}, {"h", i->location.h}};
	info["prod_level"] = i->prod_level;
	info["prod_level_min"] = PRODLEVEL_MINIMUM;
	info["prod_level_max"] = PRODLEVEL_MAXIMUM;
	info["founder"] = i->founder == INVALID_OWNER ? Json(nullptr) : Json(i->founder.base());
	info["construction_date"] = i->construction_date.base();
	info["last_prod_year"] = i->last_prod_year.base();
	info["exclusive_supplier"] = i->exclusive_supplier == INVALID_OWNER ? Json(nullptr) : Json(i->exclusive_supplier.base());
	info["exclusive_consumer"] = i->exclusive_consumer == INVALID_OWNER ? Json(nullptr) : Json(i->exclusive_consumer.base());
	info["is_raw_industry"] = GetIndustrySpec(i->type)->IsRawIndustry();
	info["is_power_plant"] = IsPowerPlant(i);

	Json flags = Json::array();
	for (const IndustryControlFlag flag : EnumRange(IndustryControlFlag::End)) {
		if (i->ctlflags.Test(flag)) flags.push_back(IndustryControlFlagName(flag));
	}
	info["control_flags"] = std::move(flags);
	return info;
}

/**
 * The cargo an industry accepts, with the waiting amount and the recent history.
 * @param i The industry; must be valid.
 * @return The description.
 */
Json IndustryNeeds(const Industry *i)
{
	Json result = Json::object();
	result["industry_id"] = i->index.base();
	result["name"] = GetString(STR_INDUSTRY_NAME, i->index);
	result["type_name"] = GetString(GetIndustrySpec(i->type)->name);

	Json accepted = Json::array();
	for (const Industry::AcceptedCargo &slot : i->accepted) {
		if (!IsValidCargoType(slot.cargo)) continue;
		Json row = Json::object();
		row["cargo"] = CargoName(slot.cargo);
		row["cargo_type"] = static_cast<uint8_t>(slot.cargo);
		row["waiting"] = slot.waiting;
		row["accumulated_waiting"] = slot.accumulated_waiting;
		row["last_accepted"] = slot.last_accepted.base();
		if (slot.history != nullptr) {
			Json history = Json::array();
			for (const Industry::AcceptedHistory &entry : *slot.history) {
				history.push_back(Json{{"accepted", entry.accepted}, {"waiting", entry.waiting}});
			}
			row["history"] = std::move(history);
		}
		accepted.push_back(std::move(row));
	}
	result["accepted"] = std::move(accepted);
	result["accepts_cargo"] = !accepted.empty();
	return result;
}

/**
 * The cargo an industry produces, with the rate and the recent history.
 * @param i The industry; must be valid.
 * @return The description.
 */
Json IndustryProduction(const Industry *i)
{
	Json result = Json::object();
	result["industry_id"] = i->index.base();
	result["name"] = GetString(STR_INDUSTRY_NAME, i->index);
	result["type_name"] = GetString(GetIndustrySpec(i->type)->name);
	result["prod_level"] = i->prod_level;
	result["prod_level_min"] = PRODLEVEL_MINIMUM;
	result["prod_level_max"] = PRODLEVEL_MAXIMUM;

	Json produced = Json::array();
	for (const Industry::ProducedCargo &slot : i->produced) {
		if (!IsValidCargoType(slot.cargo)) continue;
		Json row = Json::object();
		row["cargo"] = CargoName(slot.cargo);
		row["cargo_type"] = static_cast<uint8_t>(slot.cargo);
		row["waiting"] = slot.waiting;
		row["rate"] = slot.rate;
		Json history = Json::array();
		for (const Industry::ProducedHistory &entry : slot.history) {
			history.push_back(Json{{"production", entry.production}, {"transported", entry.transported}, {"transported_pct", entry.PctTransported()}});
		}
		row["history"] = std::move(history);
		produced.push_back(std::move(row));
	}
	result["produced"] = std::move(produced);
	result["produces_cargo"] = !produced.empty();
	return result;
}

/**
 * The cargo an industry accepts and produces in one document.
 * @param i The industry; must be valid.
 * @return The description.
 */
Json IndustryCargo(const Industry *i)
{
	Json result = IndustryProduction(i);
	Json needs = IndustryNeeds(i);
	result["accepted"] = std::move(needs["accepted"]);
	result["accepts_cargo"] = needs["accepts_cargo"];
	return result;
}

/**
 * The growth state of one town together with the settings that actually drive it.
 * @param t The town; must be valid.
 * @return The description.
 */
Json TownGrowth(const Town *t)
{
	Json result = TownNeeds(t->index);
	result["settings"] = SettingRows({
			"economy.town_growth_rate",
			"economy.larger_towns",
			"economy.town_growth_base",
			"economy.town_cargo_scale",
			"economy.town_need_power_residents",
			"economy.town_power_link_radius",
	}, _local_company);
	result["dependencies"] = Json::array({
			"the growth budget accumulates from the population and the town growth rate setting",
			"an unlocked need at level 0 blocks growth until it is supplied again",
			"the investment of the companies adds to the budget while it lasts",
			"electricity needs a power plant within the power link radius of the town",
	});
	return result;
}

/**
 * The own settings of one town and the global settings that drive them.
 * @param t The town; must be valid.
 * @return The description.
 */
Json TownSettings(const Town *t)
{
	Json result = Json::object();
	result["town_id"] = t->index.base();
	result["name"] = TownName(t);
	result["population"] = t->cache.population;
	result["houses"] = t->cache.num_houses;
	result["growth_rate"] = t->growth_rate;
	result["custom_growth"] = t->flags.Test(TownFlag::CustomGrowth);
	result["grow_counter"] = t->grow_counter;
	result["time_until_rebuild"] = t->time_until_rebuild;
	result["fund_buildings_months"] = t->fund_buildings_months;
	result["road_build_months"] = t->road_build_months;
	result["exclusive_counter"] = t->exclusive_counter;
	result["statues"] = t->statues.base();
	result["noise_reached"] = t->noise_reached;
	result["layout"] = TownLayoutName(t->layout);
	result["larger_town"] = t->larger_town;
	result["is_city"] = t->larger_town;
	result["growth_status"] = GrowthStatusName(t->growth.status);
	result["investment"] = t->growth.investment;
	result["investment_bonus"] = GetTownInvestmentBonus(t);
	result["districts_from_population"] = TOWN_DISTRICTS_MIN_POPULATION;

	Json ratings = Json::array();
	for (const Company *c : Company::Iterate()) {
		ratings.push_back(Json{{"company_id", c->index.base()}, {"rating", t->ratings[c->index]}});
	}
	result["ratings"] = std::move(ratings);

	result["settings"] = SettingRows({
			"economy.town_layout",
			"economy.found_town",
			"economy.town_growth_rate",
			"economy.larger_towns",
			"economy.town_growth_base",
			"economy.town_cargo_scale",
			"economy.town_need_power_residents",
			"economy.town_power_link_radius",
	}, _local_company);
	return result;
}

/**
 * The power links of one town and the plants behind them.
 * @param t The town; must be valid.
 * @return The description.
 */
Json TownPower(const Town *t)
{
	Json result = Json::object();
	result["town_id"] = t->index.base();
	result["name"] = TownName(t);
	result["population"] = t->cache.population;
	result["electricity_available"] = IsElectricityAvailable();
	result["power_received"] = GetTownPowerReceived(t);
	result["power_links_wanted"] = GetTownWantedPowerLinks(t);

	Json links = Json::array();
	for (const TownPowerLink &link : t->growth.power_links) {
		const Industry *plant = Industry::GetIfValid(link.plant);
		Json row = Json::object();
		row["plant"] = link.plant.base();
		row["delivered"] = link.delivered;
		if (plant != nullptr) {
			row["name"] = GetString(STR_INDUSTRY_NAME, plant->index);
			row["fuel"] = GetPowerPlantFuelStock(plant);
			row["output"] = plant->power_output;
			row["demand"] = plant->power_demand;
		}
		links.push_back(std::move(row));
	}
	result["power_links"] = std::move(links);

	result["settings"] = SettingRows({
			"economy.town_need_power_residents",
			"economy.town_power_link_radius",
	}, _local_company);
	return result;
}

/**
 * The identity and the owner of one sign.
 * @param si The sign; must be valid.
 * @return The description.
 */
Json SignInfo(const Sign *si)
{
	Json info = Json::object();
	info["sign_id"] = si->index.base();
	info["name"] = si->name;
	info["owner"] = si->owner == INVALID_OWNER ? Json(nullptr) : Json(si->owner.base());
	info["text_colour"] = ColourName(si->text_colour);
	info["x"] = si->x;
	info["y"] = si->y;
	info["z"] = si->z;
	info["tile"] = Json{{"x", si->x / TILE_SIZE}, {"y", si->y / TILE_SIZE}};
	return info;
}

/**
 * The catchment of one station and the settings that drive it.
 * @param st The station; must be valid.
 * @return The description.
 */
Json StationCatchment(const Station *st)
{
	Json result = Json::object();
	result["station_id"] = st->index.base();
	result["name"] = st->name;
	result["owner"] = st->owner == OWNER_NONE ? Json(nullptr) : Json(st->owner.base());
	result["catchment_level"] = st->catchment_level;
	result["max_catchment_level"] = MAX_STATION_CATCHMENT_LEVEL;
	result["next_upgrade_cost"] = st->catchment_level >= MAX_STATION_CATCHMENT_LEVEL
			? Json(nullptr)
			: Json(static_cast<int64_t>(GetStationCatchmentUpgradeCost(st->catchment_level + 1)));
	result["modified_catchment"] = _settings_game.station.modified_catchment;
	result["max_catchment"] = _settings_game.station.modified_catchment ? MAX_CATCHMENT : MAX_CATCHMENT_UNMODIFIED;
	result["settings"] = SettingRows({"station.modified_catchment"}, _local_company);
	return result;
}

/**
 * The object types that can be built in the current game.
 * @return The descriptions.
 */
Json ObjectTypeList()
{
	Json types = Json::array();
	for (ObjectType type = 0; type < ObjectSpec::Count(); type++) {
		const ObjectSpec *spec = ObjectSpec::Get(type);
		if (!spec->IsEnabled()) continue;
		Json row = Json::object();
		row["type"] = type;
		row["name"] = GetString(spec->name);
		row["views"] = spec->views;
		row["size_x"] = GB(spec->size, 0, 4);
		row["size_y"] = GB(spec->size, 4, 4);
		row["available"] = spec->IsAvailable();
		row["build_cost_multiplier"] = spec->build_cost_multiplier;
		types.push_back(std::move(row));
	}
	return types;
}

/**
 * The industry types that can be funded in the current game.
 * @return The descriptions.
 */
Json IndustryTypeList()
{
	Json types = Json::array();
	for (IndustryType type = 0; type < NUM_INDUSTRYTYPES; type++) {
		const IndustrySpec *spec = GetIndustrySpec(type);
		if (!spec->enabled || spec->layouts.empty()) continue;
		Json row = Json::object();
		row["type"] = type;
		row["name"] = GetString(spec->name);
		row["is_raw_industry"] = spec->IsRawIndustry();
		row["construction_cost"] = static_cast<int64_t>(spec->GetConstructionCost());
		row["prospecting_chance"] = spec->prospecting_chance;
		types.push_back(std::move(row));
	}
	return types;
}

/**
 * The livery of one company.
 * @param company The company; must be valid.
 * @return The description.
 */
Json CompanyLivery(CompanyID company)
{
	const Company *c = Company::Get(company);
	Json schemes = Json::array();
	for (const LiveryScheme scheme : EnumRange(LiveryScheme::End)) {
		const Livery &livery = c->livery[scheme];
		schemes.push_back(Json{
				{"scheme", LiverySchemeName(scheme)},
				{"in_use", livery.in_use.Any()},
				{"primary", livery.in_use.Test(Livery::Flag::Primary) ? Json(ColourName(livery.colour1)) : Json(nullptr)},
				{"secondary", livery.in_use.Test(Livery::Flag::Secondary) ? Json(ColourName(livery.colour2)) : Json(nullptr)},
		});
	}
	Json result = Json::object();
	result["company_id"] = company.base();
	result["colour"] = ColourName(c->colour);
	result["colour_index"] = to_underlying(c->colour);
	result["schemes"] = std::move(schemes);
	return result;
}

/* --- world.settings.get --- */

/**
 * Read settings with their descriptors.
 * @param params The method parameters.
 * @param company The company the call is made for.
 * @return The result document.
 */
Json SettingsGet(const Json &params, CompanyID company)
{
	/* The per-company settings are read from the company the call names, or from the caller's company. */
	CompanyID target = company;
	if (const auto error = ReadCompanyId(params, "company_id", target); error.has_value()) return *error;
	if (params.contains("company_id")) {
		if (const auto error = CheckCompany(target); error.has_value()) return *error;
	}

	std::string prefix;
	switch (ReadString(params, "prefix", prefix)) {
		case ParamState::Invalid: return MakeError("invalid_arguments", "`prefix` must be a string");
		case ParamState::Present: break;
		case ParamState::Absent: break;
	}

	bool writable_only = false;
	switch (ReadBool(params, "writable_only", writable_only)) {
		case ParamState::Invalid: return MakeError("invalid_arguments", "`writable_only` must be a boolean");
		case ParamState::Present: break;
		case ParamState::Absent: break;
	}

	int64_t limit = DEFAULT_SETTING_LIMIT;
	switch (ReadInt(params, "limit", limit)) {
		case ParamState::Invalid: return MakeError("invalid_arguments", "`limit` must be an integer");
		case ParamState::Present:
			if (limit < 1 || limit > static_cast<int64_t>(MAX_SETTING_LIMIT)) {
				return MakeError("invalid_arguments", "`limit` is out of range", {{"min", 1}, {"max", MAX_SETTING_LIMIT}});
			}
			break;
		case ParamState::Absent: break;
	}

	int64_t offset = 0;
	switch (ReadInt(params, "offset", offset)) {
		case ParamState::Invalid: return MakeError("invalid_arguments", "`offset` must be an integer");
		case ParamState::Present:
			if (offset < 0) return MakeError("invalid_arguments", "`offset` must not be negative");
			break;
		case ParamState::Absent: break;
	}

	std::vector<std::string> names;
	const auto names_it = params.find("names");
	if (names_it != params.end() && !names_it->is_null()) {
		if (!names_it->is_array()) return MakeError("invalid_arguments", "`names` must be an array of setting names");
		if (names_it->size() > MAX_SETTING_NAMES) {
			return MakeError("invalid_arguments", "too many setting names in one call", {{"max", MAX_SETTING_NAMES}});
		}
		for (const Json &item : *names_it) {
			if (!item.is_string()) return MakeError("invalid_arguments", "`names` must be an array of setting names");
			names.push_back(item.get<std::string>());
		}
	}

	Json rows = Json::array();
	std::vector<std::string> unknown;
	size_t total = 0;

	if (!names.empty()) {
		for (const std::string &name : names) {
			const SettingDesc *sd = GetSettingFromName(name);
			if (sd == nullptr || !SlIsObjectCurrentlyValid(sd->save.version_from, sd->save.version_to)) {
				unknown.push_back(name);
				continue;
			}
			if (sd->GetType() == ST_COMPANY && !Company::IsValidID(target)) {
				unknown.push_back(name);
				continue;
			}
			rows.push_back(DescribeSetting(*sd, SettingObject(*sd, target)));
		}
		total = rows.size();
	} else {
		for (const SettingDesc *sd : AllSettings()) {
			if (!prefix.empty() && !sd->GetName().starts_with(prefix)) continue;
			if (writable_only && !(sd->IsIntSetting() && sd->IsEditable(true))) continue;
			if (sd->GetType() == ST_COMPANY && !Company::IsValidID(target)) continue;
			if (total >= static_cast<size_t>(offset) && rows.size() < static_cast<size_t>(limit)) {
				rows.push_back(DescribeSetting(*sd, SettingObject(*sd, target)));
			}
			total++;
		}
	}

	Json result = Json::object();
	result["ok"] = true;
	result["settings"] = std::move(rows);
	result["total"] = total;
	result["offset"] = offset;
	result["limit"] = limit;
	if (!unknown.empty()) result["unknown"] = std::move(unknown);
	return result;
}

/* --- world.settings.set --- */

/**
 * Change one setting through the normal command path.
 * @param params The method parameters.
 * @param company The company the call is made for.
 * @return The result document.
 */
Json SettingsSet(const Json &params, CompanyID company)
{
	std::string name;
	switch (ReadString(params, "name", name)) {
		case ParamState::Invalid: return MakeError("invalid_arguments", "`name` must be a string");
		case ParamState::Absent: return MakeError("invalid_arguments", "`name` is required");
		case ParamState::Present: break;
	}
	if (name.empty()) return MakeError("invalid_arguments", "`name` is required");

	const auto value_it = params.find("value");
	if (value_it == params.end()) return MakeError("invalid_arguments", "`value` is required");

	const SettingDesc *sd = GetSettingFromName(name);
	if (sd == nullptr || !SlIsObjectCurrentlyValid(sd->save.version_from, sd->save.version_to)) {
		return MakeError("unknown_setting", "there is no setting with that name", {{"name", name}});
	}

	const SettingType scope = sd->GetType();
	CompanyID target = company;
	if (scope == ST_COMPANY) {
		if (const auto error = ReadCompanyId(params, "company_id", target); error.has_value()) return *error;
		if (const auto error = CheckCompany(target); error.has_value()) return *error;
	} else if (params.contains("company_id")) {
		return MakeError("invalid_arguments", "`company_id` only applies to per-company settings", {{"name", name}});
	}

	if (scope == ST_CLIENT) {
		return MakeGap("world.settings.set", "client_scope");
	}
	if (!sd->IsIntSetting()) {
		return MakeGap("world.settings.set", "string_setting");
	}
	if (!sd->IsEditable(true)) {
		return MakeError("setting_not_editable", "the setting cannot be changed now", {{"name", name}, {"newgame_only", sd->flags.Test(SettingFlag::NewgameOnly)}});
	}

	int32_t parsed = 0;
	if (const auto error = ParseSettingValue(*sd, *value_it, parsed); error.has_value()) return *error;

	const IntSettingDesc &isd = *sd->AsIntSetting();
	const void *object = SettingObject(*sd, target);
	const int32_t old_value = isd.Read(object);

	Json result = Json::object();
	result["name"] = sd->GetName();
	result["scope"] = ScopeName(scope);
	if (scope == ST_COMPANY) result["company_id"] = target.base();
	result["old_value"] = old_value;
	result["old_formatted"] = sd->FormatValue(object);

	bool ok;
	if (scope == ST_COMPANY) {
		const Backup<CompanyID> context(_current_company, target);
		ok = RunCommand<Commands::ChangeCompanySetting>(sd->GetName(), parsed);
	} else {
		ok = RunCommand<Commands::ChangeSetting>(sd->GetName(), parsed);
	}
	if (!_posted.captured) return AddNotExecuted(std::move(result));
	result = AddCommandResult(std::move(result));
	if (!ok) return result;

	/* Report what the engine actually applied, not what was asked for. */
	const void *after = SettingObject(*sd, target);
	result["value"] = isd.Read(after);
	result["formatted"] = sd->FormatValue(after);
	result["changed"] = isd.Read(after) != old_value;
	return result;
}

/* --- world.company --- */

/**
 * Finance, loan and company creation operations.
 * @param params The method parameters.
 * @param company The company the call is made for.
 * @return The result document.
 */
Json CompanyOp(const Json &params, CompanyID company)
{
	std::string op;
	switch (ReadString(params, "op", op)) {
		case ParamState::Invalid: return MakeError("invalid_arguments", "`op` must be a string");
		case ParamState::Absent: return MakeError("invalid_arguments", "`op` is required");
		case ParamState::Present: break;
	}

	if (op == "info") {
		CompanyID target = company;
		if (const auto error = ReadCompanyId(params, "company_id", target); error.has_value()) return *error;
		if (const auto error = CheckCompany(target); error.has_value()) return *error;
		Json result = Json::object();
		result["ok"] = true;
		result["company"] = CompanyInfo(target);
		return result;
	}

	if (op == "loan_increase" || op == "loan_decrease") {
		/* The argument shape is checked before the world is touched. */
		bool all = false;
		switch (ReadBool(params, "all", all)) {
			case ParamState::Invalid: return MakeError("invalid_arguments", "`all` must be a boolean");
			case ParamState::Present: break;
			case ParamState::Absent: break;
		}

		int64_t amount = LOAN_INTERVAL;
		switch (ReadInt(params, "amount", amount)) {
			case ParamState::Invalid: return MakeError("invalid_arguments", "`amount` must be an integer");
			case ParamState::Present: break;
			case ParamState::Absent: break;
		}
		if (!all && (amount < LOAN_INTERVAL || amount % LOAN_INTERVAL != 0)) {
			return MakeError("invalid_amount", "the amount must be a positive multiple of the loan interval",
					{{"amount", amount}, {"loan_interval", LOAN_INTERVAL}});
		}

		CompanyID target = company;
		if (const auto error = ReadCompanyId(params, "company_id", target); error.has_value()) return *error;
		if (const auto error = CheckCompany(target); error.has_value()) return *error;

		Json result = Json::object();
		result["company_id"] = target.base();
		result["before"] = CompanyInfo(target);

		const Backup<CompanyID> context(_current_company, target);
		const bool ok = op == "loan_increase"
				? (all ? RunCommand<Commands::IncreaseLoan>(LoanCommand::Max, Money(0)) : RunCommand<Commands::IncreaseLoan>(LoanCommand::Amount, Money(amount)))
				: (all ? RunCommand<Commands::DecreaseLoan>(LoanCommand::Max, Money(0)) : RunCommand<Commands::DecreaseLoan>(LoanCommand::Amount, Money(amount)));
		if (!_posted.captured) return AddNotExecuted(std::move(result));
		result = AddCommandResult(std::move(result));
		if (ok) result["after"] = CompanyInfo(target);
		return result;
	}

	if (op == "set_max_loan") {
		/* The argument shape is checked before the world is touched. */
		bool reset = false;
		switch (ReadBool(params, "reset", reset)) {
			case ParamState::Invalid: return MakeError("invalid_arguments", "`reset` must be a boolean");
			case ParamState::Present: break;
			case ParamState::Absent: break;
		}

		int64_t amount = 0;
		switch (ReadInt(params, "amount", amount)) {
			case ParamState::Invalid: return MakeError("invalid_arguments", "`amount` must be an integer");
			case ParamState::Present: break;
			case ParamState::Absent:
				if (!reset) return MakeError("invalid_arguments", "`amount` is required unless `reset` is set");
				break;
		}
		if (!reset && (amount < 0 || amount > MAX_LOAN_LIMIT)) {
			return MakeError("invalid_amount", "the maximum loan is out of range", {{"amount", amount}, {"max", MAX_LOAN_LIMIT}});
		}

		CompanyID target = company;
		if (const auto error = ReadCompanyId(params, "company_id", target); error.has_value()) return *error;
		if (const auto error = CheckCompany(target); error.has_value()) return *error;

		Json result = Json::object();
		result["company_id"] = target.base();
		result["before"] = CompanyInfo(target);

		const Backup<CompanyID> context(_current_company, OWNER_DEITY);
		const bool ok = RunCommand<Commands::SetCompanyMaxLoan>(target, reset ? Money(COMPANY_MAX_LOAN_DEFAULT) : Money(amount));
		if (!_posted.captured) return AddNotExecuted(std::move(result));
		result = AddCommandResult(std::move(result));
		if (ok) result["after"] = CompanyInfo(target);
		return result;
	}

	if (op == "create") {
		std::array<bool, MAX_COMPANIES> before{};
		for (const Company *c : Company::Iterate()) before[c->index.base()] = true;

		Json result = Json::object();
		const bool ok = RunCommand<Commands::CompanyControl>(CompanyCtrlAction::NewAI, CompanyID::Invalid(), CompanyRemoveReason::None, ClientID::Invalid);
		if (!_posted.captured) return AddNotExecuted(std::move(result));
		result = AddCommandResult(std::move(result));
		if (!ok) return result;

		Json created = Json::array();
		for (const Company *c : Company::Iterate()) {
			if (!before[c->index.base()]) created.push_back(c->index.base());
		}
		result["created"] = std::move(created);
		if (created.empty()) {
			/* The command succeeded but the pool was full, so no company exists. */
			result["ok"] = false;
			result["error"] = "no_company_created";
			result["message"] = "the engine accepted the command but no company was created; the company pool is full";
		}
		return result;
	}

	if (op == "delete") {
		CompanyID target = CompanyID::Invalid();
		if (const auto error = ReadCompanyId(params, "company_id", target); error.has_value()) return *error;
		if (target == CompanyID::Invalid()) return MakeError("invalid_arguments", "`company_id` is required");
		if (const auto error = CheckCompany(target); error.has_value()) return *error;
		if (Company::GetNumItems() <= 1) {
			return MakeError("last_company", "the last company of a single player game cannot be deleted", {{"company_id", target.base()}});
		}

		Json result = Json::object();
		result["company_id"] = target.base();
		result["before"] = CompanyInfo(target);
		const bool ok = RunCommand<Commands::CompanyControl>(CompanyCtrlAction::Delete, target, CompanyRemoveReason::Manual, ClientID::Invalid);
		if (!_posted.captured) return AddNotExecuted(std::move(result));
		result = AddCommandResult(std::move(result));
		if (ok) {
			result["deleted"] = !Company::IsValidID(target);
			result["local_company"] = _local_company.base();
		}
		return result;
	}

	if (op == "give_money") {
		/* The argument shape is checked before the world is touched. */
		int64_t amount = 0;
		switch (ReadInt(params, "amount", amount)) {
			case ParamState::Invalid: return MakeError("invalid_arguments", "`amount` must be an integer");
			case ParamState::Absent: return MakeError("invalid_arguments", "`amount` is required");
			case ParamState::Present: break;
		}
		if (amount <= 0) return MakeError("invalid_amount", "the amount must be positive", {{"amount", amount}});

		CompanyID to = CompanyID::Invalid();
		if (const auto error = ReadCompanyId(params, "to_company_id", to); error.has_value()) return *error;
		if (to == CompanyID::Invalid()) return MakeError("invalid_arguments", "`to_company_id` is required");

		CompanyID from = company;
		if (const auto error = ReadCompanyId(params, "company_id", from); error.has_value()) return *error;
		if (const auto error = CheckCompany(from); error.has_value()) return *error;
		if (const auto error = CheckCompany(to); error.has_value()) return *error;
		if (to == from) return MakeError("invalid_arguments", "a company cannot give money to itself", {{"company_id", from.base()}});

		Json result = Json::object();
		result["company_id"] = from.base();
		result["to_company_id"] = to.base();
		result["amount"] = amount;
		const Backup<CompanyID> context(_current_company, from);
		const bool ok = RunCommand<Commands::GiveMoney>(Money(amount), to);
		if (!_posted.captured) return AddNotExecuted(std::move(result));
		result = AddCommandResult(std::move(result));
		if (ok) {
			result["from_money"] = static_cast<int64_t>(Company::Get(from)->money);
			result["to_money"] = static_cast<int64_t>(Company::Get(to)->money);
		}
		return result;
	}

	if (op == "info") {
		CompanyID target = company;
		if (const auto error = ReadCompanyId(params, "company_id", target); error.has_value()) return *error;
		if (const auto error = CheckCompany(target); error.has_value()) return *error;
		Json result = Json::object();
		result["ok"] = true;
		result["company"] = CompanyInfo(target);
		return result;
	}

	if (op == "livery") {
		CompanyID target = company;
		if (const auto error = ReadCompanyId(params, "company_id", target); error.has_value()) return *error;
		if (const auto error = CheckCompany(target); error.has_value()) return *error;
		Json result = Json::object();
		result["ok"] = true;
		result["livery"] = CompanyLivery(target);
		return result;
	}

	if (op == "colour") {
		CompanyID target = company;
		if (const auto error = ReadCompanyId(params, "company_id", target); error.has_value()) return *error;
		if (const auto error = CheckCompany(target); error.has_value()) return *error;

		const auto colour_it = params.find("colour");
		if (colour_it == params.end() || colour_it->is_null()) return MakeError("invalid_arguments", "`colour` is required");
		Colours colour = Colours::Begin;
		if (!ParseColour(*colour_it, colour)) {
			Json allowed = Json::array();
			for (const Colours candidate : EnumRange(Colours::End)) allowed.push_back(ColourName(candidate));
			return MakeError("invalid_arguments", "`colour` must be a colour name or a palette index", {{"colour", *colour_it}, {"allowed", std::move(allowed)}});
		}

		Json result = Json::object();
		result["company_id"] = target.base();
		result["colour"] = ColourName(colour);
		result["colour_before"] = ColourName(Company::Get(target)->colour);
		const Backup<CompanyID> context(_current_company, target);
		const bool ok = RunCommand<Commands::SetCompanyColour>(LiveryScheme::Default, true, colour);
		if (!_posted.captured) return AddNotExecuted(std::move(result));
		result = AddCommandResult(std::move(result));
		if (ok) result["colour_after"] = ColourName(Company::Get(target)->colour);
		return result;
	}

	if (op == "livery_set") {
		CompanyID target = company;
		if (const auto error = ReadCompanyId(params, "company_id", target); error.has_value()) return *error;
		if (const auto error = CheckCompany(target); error.has_value()) return *error;

		std::string scheme_name;
		switch (ReadString(params, "scheme", scheme_name)) {
			case ParamState::Invalid: return MakeError("invalid_arguments", "`scheme` must be a string");
			case ParamState::Absent: return MakeError("invalid_arguments", "`scheme` is required");
			case ParamState::Present: break;
		}
		LiveryScheme scheme = LiveryScheme::Default;
		if (!ParseLiverySchemeName(scheme_name, scheme)) {
			Json allowed = Json::array();
			for (const LiveryScheme candidate : EnumRange(LiveryScheme::End)) allowed.push_back(LiverySchemeName(candidate));
			return MakeError("invalid_arguments", "unknown livery scheme", {{"scheme", scheme_name}, {"allowed", std::move(allowed)}});
		}

		bool primary = true;
		switch (ReadBool(params, "primary", primary)) {
			case ParamState::Invalid: return MakeError("invalid_arguments", "`primary` must be a boolean");
			case ParamState::Present: break;
			case ParamState::Absent: break;
		}

		const auto colour_it = params.find("colour");
		if (colour_it == params.end() || colour_it->is_null()) return MakeError("invalid_arguments", "`colour` is required");
		Colours colour = Colours::Begin;
		if (!ParseColour(*colour_it, colour)) {
			return MakeError("invalid_arguments", "`colour` must be a colour name or a palette index", {{"colour", *colour_it}});
		}

		Json result = Json::object();
		result["company_id"] = target.base();
		result["scheme"] = LiverySchemeName(scheme);
		result["primary"] = primary;
		result["colour"] = ColourName(colour);
		const Backup<CompanyID> context(_current_company, target);
		const bool ok = RunCommand<Commands::SetCompanyColour>(scheme, primary, colour);
		if (!_posted.captured) return AddNotExecuted(std::move(result));
		result = AddCommandResult(std::move(result));
		if (ok) result["livery"] = CompanyLivery(target);
		return result;
	}

	if (op == "manager_face") {
		CompanyID target = company;
		if (const auto error = ReadCompanyId(params, "company_id", target); error.has_value()) return *error;
		if (const auto error = CheckCompany(target); error.has_value()) return *error;

		int64_t style = 0;
		switch (ReadInt(params, "style", style)) {
			case ParamState::Invalid: return MakeError("invalid_arguments", "`style` must be an integer");
			case ParamState::Absent: return MakeError("invalid_arguments", "`style` is required");
			case ParamState::Present: break;
		}
		if (style < 0 || style >= static_cast<int64_t>(GetNumCompanyManagerFaceStyles())) {
			return MakeError("invalid_arguments", "the manager face style is out of range",
					{{"style", style}, {"max", GetNumCompanyManagerFaceStyles() - 1}});
		}

		int64_t bits = 0;
		switch (ReadInt(params, "bits", bits)) {
			case ParamState::Invalid: return MakeError("invalid_arguments", "`bits` must be an integer");
			case ParamState::Absent: return MakeError("invalid_arguments", "`bits` is required");
			case ParamState::Present: break;
		}
		if (bits < 0 || bits > static_cast<int64_t>(UINT32_MAX)) {
			return MakeError("invalid_arguments", "the manager face bits are out of range", {{"bits", bits}});
		}

		Json result = Json::object();
		result["company_id"] = target.base();
		result["style"] = style;
		result["bits"] = bits;
		const Backup<CompanyID> context(_current_company, target);
		const bool ok = RunCommand<Commands::SetCompanyManagerFace>(static_cast<uint>(style), static_cast<uint32_t>(bits));
		if (!_posted.captured) return AddNotExecuted(std::move(result));
		result = AddCommandResult(std::move(result));
		if (ok) {
			const Company *c = Company::Get(target);
			result["face_after"] = Json{{"style", c->face.style}, {"bits", c->face.bits}};
		}
		return result;
	}

	if (op == "rename") {
		CompanyID target = company;
		if (const auto error = ReadCompanyId(params, "company_id", target); error.has_value()) return *error;
		if (const auto error = CheckCompany(target); error.has_value()) return *error;

		std::string text;
		switch (ReadString(params, "text", text)) {
			case ParamState::Invalid: return MakeError("invalid_arguments", "`text` must be a string");
			case ParamState::Absent: return MakeError("invalid_arguments", "`text` is required");
			case ParamState::Present: break;
		}

		Json result = Json::object();
		result["company_id"] = target.base();
		result["name_before"] = CompanyName(Company::Get(target));
		const Backup<CompanyID> context(_current_company, target);
		const bool ok = RunCommand<Commands::RenameCompany>(text);
		if (!_posted.captured) return AddNotExecuted(std::move(result));
		result = AddCommandResult(std::move(result));
		if (ok) result["name_after"] = CompanyName(Company::Get(target));
		return result;
	}

	if (op == "rename_president") {
		CompanyID target = company;
		if (const auto error = ReadCompanyId(params, "company_id", target); error.has_value()) return *error;
		if (const auto error = CheckCompany(target); error.has_value()) return *error;

		std::string text;
		switch (ReadString(params, "text", text)) {
			case ParamState::Invalid: return MakeError("invalid_arguments", "`text` must be a string");
			case ParamState::Absent: return MakeError("invalid_arguments", "`text` is required");
			case ParamState::Present: break;
		}

		Json result = Json::object();
		result["company_id"] = target.base();
		result["president_name_before"] = PresidentName(Company::Get(target));
		const Backup<CompanyID> context(_current_company, target);
		const bool ok = RunCommand<Commands::RenamePresident>(text);
		if (!_posted.captured) return AddNotExecuted(std::move(result));
		result = AddCommandResult(std::move(result));
		if (ok) result["president_name_after"] = PresidentName(Company::Get(target));
		return result;
	}

	if (op == "buy") {
		CompanyID target = CompanyID::Invalid();
		if (const auto error = ReadCompanyId(params, "company_id", target); error.has_value()) return *error;
		if (target == CompanyID::Invalid()) return MakeError("invalid_arguments", "`company_id` is required");
		if (const auto error = CheckCompany(target); error.has_value()) return *error;

		bool hostile = false;
		switch (ReadBool(params, "hostile", hostile)) {
			case ParamState::Invalid: return MakeError("invalid_arguments", "`hostile` must be a boolean");
			case ParamState::Present: break;
			case ParamState::Absent: break;
		}

		CompanyID buyer = company;
		if (const auto error = ReadCompanyId(params, "buyer_company_id", buyer); error.has_value()) return *error;
		if (const auto error = CheckCompany(buyer); error.has_value()) return *error;
		if (buyer == target) return MakeError("invalid_arguments", "a company cannot buy itself", {{"company_id", target.base()}});

		Json result = Json::object();
		result["company_id"] = target.base();
		result["buyer_company_id"] = buyer.base();
		result["hostile"] = hostile;
		result["before"] = CompanyInfo(target);
		const Backup<CompanyID> context(_current_company, buyer);
		const bool ok = RunCommand<Commands::BuyCompany>(target, hostile);
		if (!_posted.captured) return AddNotExecuted(std::move(result));
		result = AddCommandResult(std::move(result));
		if (ok) {
			result["bought"] = !Company::IsValidID(target);
			result["buyer"] = CompanyInfo(buyer);
		}
		return result;
	}

	return MakeGap("world.company", op);
}

/* --- world.towns --- */

/**
 * Town needs, investment and growth operations.
 * @param params The method parameters.
 * @param company The company the call is made for.
 * @return The result document.
 */
Json TownsOp(const Json &params, CompanyID company)
{
	std::string op;
	switch (ReadString(params, "op", op)) {
		case ParamState::Invalid: return MakeError("invalid_arguments", "`op` must be a string");
		case ParamState::Absent: return MakeError("invalid_arguments", "`op` is required");
		case ParamState::Present: break;
	}

	if (op == "needs") {
		TownID town = TownID::Invalid();
		if (const auto error = ReadTownId(params, "town_id", town); error.has_value()) return *error;
		if (town == TownID::Invalid()) return MakeError("invalid_arguments", "`town_id` is required");
		if (const auto error = CheckTown(town); error.has_value()) return *error;
		Json result = Json::object();
		result["ok"] = true;
		result["town"] = TownNeeds(town);
		return result;
	}

	if (op == "force_need") {
		TownID town = TownID::Invalid();
		if (const auto error = ReadTownId(params, "town_id", town); error.has_value()) return *error;
		if (town != TownID::Invalid()) {
			if (const auto error = CheckTown(town); error.has_value()) return *error;
		}

		std::string need_name = "all";
		switch (ReadString(params, "need", need_name)) {
			case ParamState::Invalid: return MakeError("invalid_arguments", "`need` must be a string");
			case ParamState::Present: break;
			case ParamState::Absent: break;
		}
		TownNeed need = TownNeed::End;
		if (!ParseNeedName(need_name, need)) {
			return MakeError("invalid_arguments", "unknown town need", {{"need", need_name}, {"allowed", Json::array({"passengers", "mail", "goods", "electricity", "valuables", "all"})}});
		}

		/* `level` is a number, or "auto" to let the engine compute it from the supply again. */
		uint8_t level = TOWN_NEED_NOT_FORCED;
		const auto level_it = params.find("level");
		if (level_it != params.end() && !level_it->is_null()) {
			if (level_it->is_string() && level_it->get<std::string>() == "auto") {
				level = TOWN_NEED_NOT_FORCED;
			} else {
				int64_t number = 0;
				if (ReadInt(params, "level", number) != ParamState::Present) return MakeError("invalid_arguments", "`level` must be an integer or \"auto\"");
				if (number < 0 || number > TOWN_NEED_LEVEL_MAX) {
					return MakeError("invalid_arguments", "the need level is out of range", {{"level", number}, {"min", 0}, {"max", TOWN_NEED_LEVEL_MAX}});
				}
				level = static_cast<uint8_t>(number);
			}
		}

		Json result = Json::object();
		result["town_id"] = town == TownID::Invalid() ? Json(nullptr) : Json(town.base());
		result["need"] = need_name;
		result["level"] = level == TOWN_NEED_NOT_FORCED ? Json("auto") : Json(level);
		const Backup<CompanyID> context(_current_company, OWNER_DEITY);
		const bool ok = RunCommand<Commands::TownForceNeedLevel>(town, need, level);
		if (!_posted.captured) return AddNotExecuted(std::move(result));
		result = AddCommandResult(std::move(result));
		if (ok && town != TownID::Invalid()) result["town"] = TownNeeds(town);
		return result;
	}

	if (op == "invest") {
		/* The argument shape is checked before the world is touched. */
		int64_t index = 0;
		switch (ReadInt(params, "amount_index", index)) {
			case ParamState::Invalid: return MakeError("invalid_arguments", "`amount_index` must be an integer");
			case ParamState::Absent: return MakeError("invalid_arguments", "`amount_index` is required");
			case ParamState::Present: break;
		}
		if (index < 0 || index >= static_cast<int64_t>(TownGrowthBalance::INVESTMENT_AMOUNTS.size())) {
			return MakeError("invalid_arguments", "the investment index is out of range",
					{{"amount_index", index}, {"max", TownGrowthBalance::INVESTMENT_AMOUNTS.size() - 1}});
		}

		TownID town = TownID::Invalid();
		if (const auto error = ReadTownId(params, "town_id", town); error.has_value()) return *error;
		if (town == TownID::Invalid()) return MakeError("invalid_arguments", "`town_id` is required");
		if (const auto error = CheckTown(town); error.has_value()) return *error;

		CompanyID investor = company;
		if (const auto error = ReadCompanyId(params, "company_id", investor); error.has_value()) return *error;
		if (const auto error = CheckCompany(investor); error.has_value()) return *error;

		Json result = Json::object();
		result["town_id"] = town.base();
		result["company_id"] = investor.base();
		result["amount_index"] = index;
		result["amount"] = TownGrowthBalance::INVESTMENT_AMOUNTS[index];
		result["investment_before"] = Town::Get(town)->growth.investment;
		const Backup<CompanyID> context(_current_company, investor);
		const bool ok = RunCommand<Commands::InvestInTown>(town, static_cast<uint8_t>(index));
		if (!_posted.captured) return AddNotExecuted(std::move(result));
		result = AddCommandResult(std::move(result));
		if (ok) result["investment_after"] = Town::Get(town)->growth.investment;
		return result;
	}

	if (op == "set_investment") {
		TownID town = TownID::Invalid();
		if (const auto error = ReadTownId(params, "town_id", town); error.has_value()) return *error;
		if (town != TownID::Invalid()) {
			if (const auto error = CheckTown(town); error.has_value()) return *error;
		}

		int64_t amount = 0;
		switch (ReadInt(params, "amount", amount)) {
			case ParamState::Invalid: return MakeError("invalid_arguments", "`amount` must be an integer");
			case ParamState::Absent: return MakeError("invalid_arguments", "`amount` is required");
			case ParamState::Present: break;
		}
		if (amount < 0) return MakeError("invalid_amount", "the investment must not be negative", {{"amount", amount}});

		bool add = false;
		switch (ReadBool(params, "add", add)) {
			case ParamState::Invalid: return MakeError("invalid_arguments", "`add` must be a boolean");
			case ParamState::Present: break;
			case ParamState::Absent: break;
		}

		Json result = Json::object();
		result["town_id"] = town == TownID::Invalid() ? Json(nullptr) : Json(town.base());
		result["amount"] = amount;
		result["add"] = add;
		const Backup<CompanyID> context(_current_company, OWNER_DEITY);
		const bool ok = RunCommand<Commands::SetTownInvestment>(town, Money(amount), add);
		if (!_posted.captured) return AddNotExecuted(std::move(result));
		result = AddCommandResult(std::move(result));
		if (ok && town != TownID::Invalid()) result["investment_after"] = Town::Get(town)->growth.investment;
		return result;
	}

	if (op == "growth_rate") {
		TownID town = TownID::Invalid();
		if (const auto error = ReadTownId(params, "town_id", town); error.has_value()) return *error;
		if (town == TownID::Invalid()) return MakeError("invalid_arguments", "`town_id` is required");
		if (const auto error = CheckTown(town); error.has_value()) return *error;

		int64_t rate = 0;
		switch (ReadInt(params, "growth_rate", rate)) {
			case ParamState::Invalid: return MakeError("invalid_arguments", "`growth_rate` must be an integer");
			case ParamState::Absent: return MakeError("invalid_arguments", "`growth_rate` is required");
			case ParamState::Present: break;
		}
		if (rate < 0 || rate > UINT16_MAX) {
			return MakeError("invalid_arguments", "the growth rate is out of range", {{"growth_rate", rate}, {"max", UINT16_MAX}});
		}

		Json result = Json::object();
		result["town_id"] = town.base();
		result["growth_rate"] = rate;
		result["growth_rate_before"] = Town::Get(town)->growth_rate;
		const Backup<CompanyID> context(_current_company, OWNER_DEITY);
		const bool ok = RunCommand<Commands::TownGrowthRate>(town, static_cast<uint16_t>(rate));
		if (!_posted.captured) return AddNotExecuted(std::move(result));
		result = AddCommandResult(std::move(result));
		if (ok) {
			result["growth_rate_after"] = Town::Get(town)->growth_rate;
			result["custom_growth"] = Town::Get(town)->flags.Test(TownFlag::CustomGrowth);
		}
		return result;
	}

	if (op == "expand") {
		/* The argument shape is checked before the world is touched. */
		int64_t grow_amount = 0;
		switch (ReadInt(params, "grow_amount", grow_amount)) {
			case ParamState::Invalid: return MakeError("invalid_arguments", "`grow_amount` must be an integer");
			case ParamState::Present: break;
			case ParamState::Absent: break;
		}
		if (grow_amount < 0 || grow_amount > UINT32_MAX) {
			return MakeError("invalid_arguments", "the grow amount is out of range", {{"grow_amount", grow_amount}});
		}

		TownExpandModes modes;
		const auto modes_it = params.find("modes");
		if (modes_it != params.end() && !modes_it->is_null()) {
			if (!modes_it->is_array()) return MakeError("invalid_arguments", "`modes` must be an array of \"buildings\" and \"roads\"");
			for (const Json &item : *modes_it) {
				if (!item.is_string()) return MakeError("invalid_arguments", "`modes` must be an array of \"buildings\" and \"roads\"");
				const std::string mode = item.get<std::string>();
				if (mode == "buildings") {
					modes.Set(TownExpandMode::Buildings);
				} else if (mode == "roads") {
					modes.Set(TownExpandMode::Roads);
				} else {
					return MakeError("invalid_arguments", "unknown expand mode", {{"mode", mode}, {"allowed", Json::array({"buildings", "roads"})}});
				}
			}
		} else {
			modes.Set(TownExpandMode::Buildings);
			modes.Set(TownExpandMode::Roads);
		}
		if (modes.None()) return MakeError("invalid_arguments", "`modes` must not be empty");

		TownID town = TownID::Invalid();
		if (const auto error = ReadTownId(params, "town_id", town); error.has_value()) return *error;
		if (town == TownID::Invalid()) return MakeError("invalid_arguments", "`town_id` is required");
		if (const auto error = CheckTown(town); error.has_value()) return *error;

		Json result = Json::object();
		result["town_id"] = town.base();
		result["grow_amount"] = grow_amount;
		result["population_before"] = Town::Get(town)->cache.population;
		const Backup<CompanyID> context(_current_company, OWNER_DEITY);
		const bool ok = RunCommand<Commands::ExpandTown>(town, static_cast<uint32_t>(grow_amount), modes);
		if (!_posted.captured) return AddNotExecuted(std::move(result));
		result = AddCommandResult(std::move(result));
		if (ok) result["population_after"] = Town::Get(town)->cache.population;
		return result;
	}

	if (op == "cargo_goal") {
		TownID town = TownID::Invalid();
		if (const auto error = ReadTownId(params, "town_id", town); error.has_value()) return *error;
		if (town == TownID::Invalid()) return MakeError("invalid_arguments", "`town_id` is required");
		if (const auto error = CheckTown(town); error.has_value()) return *error;

		std::string effect_name;
		switch (ReadString(params, "effect", effect_name)) {
			case ParamState::Invalid: return MakeError("invalid_arguments", "`effect` must be a string");
			case ParamState::Absent: return MakeError("invalid_arguments", "`effect` is required");
			case ParamState::Present: break;
		}
		TownAcceptanceEffect effect = TownAcceptanceEffect::None;
		if (!ParseAcceptanceEffectName(effect_name, effect)) {
			return MakeError("invalid_arguments", "unknown town acceptance effect", {{"effect", effect_name}});
		}

		int64_t goal = 0;
		switch (ReadInt(params, "goal", goal)) {
			case ParamState::Invalid: return MakeError("invalid_arguments", "`goal` must be an integer");
			case ParamState::Absent: return MakeError("invalid_arguments", "`goal` is required");
			case ParamState::Present: break;
		}
		if (goal < 0 || goal > UINT32_MAX) return MakeError("invalid_arguments", "the goal is out of range", {{"goal", goal}});

		Json result = Json::object();
		result["town_id"] = town.base();
		result["effect"] = effect_name;
		result["goal"] = goal;
		const Backup<CompanyID> context(_current_company, OWNER_DEITY);
		RunCommand<Commands::TownCargoGoal>(town, effect, static_cast<uint32_t>(goal));
		if (!_posted.captured) return AddNotExecuted(std::move(result));
		return AddCommandResult(std::move(result));
	}

	if (op == "rating") {
		TownID town = TownID::Invalid();
		if (const auto error = ReadTownId(params, "town_id", town); error.has_value()) return *error;
		if (town == TownID::Invalid()) return MakeError("invalid_arguments", "`town_id` is required");
		if (const auto error = CheckTown(town); error.has_value()) return *error;

		CompanyID rated = company;
		if (const auto error = ReadCompanyId(params, "company_id", rated); error.has_value()) return *error;
		if (const auto error = CheckCompany(rated); error.has_value()) return *error;

		int64_t rating = 0;
		switch (ReadInt(params, "rating", rating)) {
			case ParamState::Invalid: return MakeError("invalid_arguments", "`rating` must be an integer");
			case ParamState::Absent: return MakeError("invalid_arguments", "`rating` is required");
			case ParamState::Present: break;
		}
		if (rating < RATING_MINIMUM || rating > RATING_MAXIMUM) {
			return MakeError("invalid_arguments", "the rating is out of range", {{"rating", rating}, {"min", RATING_MINIMUM}, {"max", RATING_MAXIMUM}});
		}

		Json result = Json::object();
		result["town_id"] = town.base();
		result["company_id"] = rated.base();
		result["rating"] = rating;
		const Backup<CompanyID> context(_current_company, OWNER_DEITY);
		const bool ok = RunCommand<Commands::TownRating>(town, rated, static_cast<int16_t>(rating));
		if (!_posted.captured) return AddNotExecuted(std::move(result));
		result = AddCommandResult(std::move(result));
		if (ok) result["rating_after"] = Town::Get(town)->ratings[rated];
		return result;
	}

	if (op == "destroy") {
		TownID town = TownID::Invalid();
		if (const auto error = ReadTownId(params, "town_id", town); error.has_value()) return *error;
		if (town == TownID::Invalid()) return MakeError("invalid_arguments", "`town_id` is required");
		if (const auto error = CheckTown(town); error.has_value()) return *error;

		CompanyID payer = OWNER_DEITY;
		if (const auto error = ReadCompanyId(params, "company_id", payer); error.has_value()) return *error;
		if (payer != OWNER_DEITY) {
			if (const auto error = CheckCompany(payer); error.has_value()) return *error;
		}

		const auto [houses, roads] = GetTownDestructionCounts(Town::Get(town));
		Json result = Json::object();
		result["town_id"] = town.base();
		result["houses"] = houses;
		result["road_tiles"] = roads;
		const Backup<CompanyID> context(_current_company, payer);
		const bool ok = RunCommand<Commands::DestroyTown>(town);
		if (!_posted.captured) return AddNotExecuted(std::move(result));
		result = AddCommandResult(std::move(result));
		if (ok) result["destroyed"] = !Town::IsValidID(town);
		return result;
	}

	if (op == "list") {
		size_t offset = 0;
		size_t limit = 0;
		if (const auto error = ReadListWindow(params, offset, limit)) return *error;

		Json towns = Json::array();
		size_t total = 0;
		for (const Town *t : Town::Iterate()) {
			if (total >= offset && towns.size() < limit) {
				towns.push_back(Json{
						{"town_id", t->index.base()},
						{"name", TownName(t)},
						{"population", t->cache.population},
						{"houses", t->cache.num_houses},
						{"is_city", t->larger_town},
						{"growth_status", GrowthStatusName(t->growth.status)},
						{"location", Json{{"x", TileX(t->xy)}, {"y", TileY(t->xy)}}},
				});
			}
			total++;
		}
		Json result = Json::object();
		result["ok"] = true;
		result["towns"] = std::move(towns);
		result["total"] = total;
		result["offset"] = offset;
		result["limit"] = limit;
		return result;
	}

	if (op == "growth") {
		TownID town = TownID::Invalid();
		if (const auto error = ReadTownId(params, "town_id", town); error.has_value()) return *error;
		if (town == TownID::Invalid()) return MakeError("invalid_arguments", "`town_id` is required");
		if (const auto error = CheckTown(town); error.has_value()) return *error;
		Json result = Json::object();
		result["ok"] = true;
		result["town"] = TownGrowth(Town::Get(town));
		return result;
	}

	if (op == "settings") {
		TownID town = TownID::Invalid();
		if (const auto error = ReadTownId(params, "town_id", town); error.has_value()) return *error;
		if (town == TownID::Invalid()) return MakeError("invalid_arguments", "`town_id` is required");
		if (const auto error = CheckTown(town); error.has_value()) return *error;
		Json result = Json::object();
		result["ok"] = true;
		result["town"] = TownSettings(Town::Get(town));
		return result;
	}

	if (op == "power") {
		TownID town = TownID::Invalid();
		if (const auto error = ReadTownId(params, "town_id", town); error.has_value()) return *error;
		if (town == TownID::Invalid()) return MakeError("invalid_arguments", "`town_id` is required");
		if (const auto error = CheckTown(town); error.has_value()) return *error;
		Json result = Json::object();
		result["ok"] = true;
		result["town"] = TownPower(Town::Get(town));
		return result;
	}

	if (op == "found") {
		/* The cheap arguments are checked before the tile, so a malformed call never reaches the map. */
		TownSize size = TownSize::Small;
		std::string size_name;
		switch (ReadString(params, "size", size_name)) {
			case ParamState::Invalid: return MakeError("invalid_arguments", "`size` must be a string");
			case ParamState::Present:
				if (!ParseTownSizeName(size_name, size)) {
					Json allowed = Json::array();
					for (const TownSize candidate : EnumRange(TownSize::End)) allowed.push_back(TownSizeName(candidate));
					return MakeError("invalid_arguments", "unknown town size", {{"size", size_name}, {"allowed", std::move(allowed)}});
				}
				break;
			case ParamState::Absent: break;
		}

		bool city = false;
		switch (ReadBool(params, "city", city)) {
			case ParamState::Invalid: return MakeError("invalid_arguments", "`city` must be a boolean");
			case ParamState::Present: break;
			case ParamState::Absent: break;
		}

		TownLayout layout = _settings_game.economy.town_layout;
		std::string layout_name;
		switch (ReadString(params, "layout", layout_name)) {
			case ParamState::Invalid: return MakeError("invalid_arguments", "`layout` must be a string");
			case ParamState::Present:
				if (!ParseTownLayoutName(layout_name, layout)) {
					Json allowed = Json::array();
					for (const TownLayout candidate : EnumRange(TownLayout::End)) allowed.push_back(TownLayoutName(candidate));
					return MakeError("invalid_arguments", "unknown town layout", {{"layout", layout_name}, {"allowed", std::move(allowed)}});
				}
				break;
			case ParamState::Absent: break;
		}

		std::string name;
		switch (ReadString(params, "name", name)) {
			case ParamState::Invalid: return MakeError("invalid_arguments", "`name` must be a string");
			case ParamState::Present: break;
			case ParamState::Absent: break;
		}
		if (Utf8StringLength(name) >= MAX_LENGTH_TOWN_NAME_CHARS) {
			return MakeError("invalid_arguments", "the town name is too long",
					{{"length", Utf8StringLength(name)}, {"max", MAX_LENGTH_TOWN_NAME_CHARS - 1}});
		}

		TileIndex tile = INVALID_TILE;
		switch (ReadTile(params, "tile", tile)) {
			case ParamState::Invalid: return MakeError("invalid_arguments", "`tile` must be an object with `x` and `y` inside the map");
			case ParamState::Absent: return MakeError("invalid_arguments", "`tile` is required");
			case ParamState::Present: break;
		}

		Json result = Json::object();
		result["tile"] = Json{{"x", TileX(tile)}, {"y", TileY(tile)}};
		result["size"] = TownSizeName(size);
		result["city"] = city;
		result["layout"] = TownLayoutName(layout);
		result["name"] = name;

		/*
		 * The engine generates the name from the town name parts and rejects a duplicate, so an
		 * automatic name is retried with a fresh seed a few times before the error is reported.
		 */
		const Backup<CompanyID> context(_current_company, OWNER_DEITY);
		bool ok = false;
		for (int attempt = 0; attempt < 8; attempt++) {
			const uint32_t parts = Random();
			ok = RunFoundTown<Commands::FoundTown>(tile, size, city, layout, false, parts, name);
			if (ok) break;
			if (!_posted_found_town.captured) break;
			if (!name.empty()) break;
			if (_posted_found_town.cost.GetErrorMessage() != STR_ERROR_NAME_MUST_BE_UNIQUE) break;
		}
		if (!_posted_found_town.captured) return AddNotExecuted(std::move(result));
		result = AddCommandResult(std::move(result), _posted_found_town.cost);
		if (ok) {
			result["town_cost"] = static_cast<int64_t>(_posted_found_town.town_cost);
			if (Town::IsValidID(_posted_found_town.town)) {
				const Town *t = Town::Get(_posted_found_town.town);
				result["town_id"] = t->index.base();
				result["town_name"] = TownName(t);
				result["population"] = t->cache.population;
			}
		}
		return result;
	}

	if (op == "rename") {
		TownID town = TownID::Invalid();
		if (const auto error = ReadTownId(params, "town_id", town); error.has_value()) return *error;
		if (town == TownID::Invalid()) return MakeError("invalid_arguments", "`town_id` is required");
		if (const auto error = CheckTown(town); error.has_value()) return *error;

		std::string text;
		switch (ReadString(params, "text", text)) {
			case ParamState::Invalid: return MakeError("invalid_arguments", "`text` must be a string");
			case ParamState::Absent: return MakeError("invalid_arguments", "`text` is required");
			case ParamState::Present: break;
		}
		if (Utf8StringLength(text) >= MAX_LENGTH_TOWN_NAME_CHARS) {
			return MakeError("invalid_arguments", "the town name is too long",
					{{"length", Utf8StringLength(text)}, {"max", MAX_LENGTH_TOWN_NAME_CHARS - 1}});
		}

		Json result = Json::object();
		result["town_id"] = town.base();
		result["name_before"] = TownName(Town::Get(town));
		const Backup<CompanyID> context(_current_company, OWNER_DEITY);
		const bool ok = RunCommand<Commands::RenameTown>(town, text);
		if (!_posted.captured) return AddNotExecuted(std::move(result));
		result = AddCommandResult(std::move(result));
		if (ok) result["name_after"] = TownName(Town::Get(town));
		return result;
	}

	if (op == "town_action") {
		TownID town = TownID::Invalid();
		if (const auto error = ReadTownId(params, "town_id", town); error.has_value()) return *error;
		if (town == TownID::Invalid()) return MakeError("invalid_arguments", "`town_id` is required");
		if (const auto error = CheckTown(town); error.has_value()) return *error;

		std::string action_name;
		switch (ReadString(params, "action", action_name)) {
			case ParamState::Invalid: return MakeError("invalid_arguments", "`action` must be a string");
			case ParamState::Absent: return MakeError("invalid_arguments", "`action` is required");
			case ParamState::Present: break;
		}
		TownAction action = TownAction::AdvertiseSmall;
		if (!ParseTownActionName(action_name, action)) {
			Json allowed = Json::array();
			for (const TownAction candidate : EnumRange(TownAction::End)) allowed.push_back(TownActionName(candidate));
			return MakeError("invalid_arguments", "unknown town action", {{"action", action_name}, {"allowed", std::move(allowed)}});
		}

		CompanyID actor = company;
		if (const auto error = ReadCompanyId(params, "company_id", actor); error.has_value()) return *error;
		if (const auto error = CheckCompany(actor); error.has_value()) return *error;

		const Town *t = Town::Get(town);
		const TownActions available = GetMaskOfTownActions(actor, t);
		if (!available.Test(action)) {
			Json allowed = Json::array();
			for (const TownAction candidate : EnumRange(TownAction::End)) {
				if (available.Test(candidate)) allowed.push_back(TownActionName(candidate));
			}
			return MakeError("action_not_available", "the town does not offer that action to the company",
					{{"town_id", town.base()}, {"action", action_name}, {"allowed", std::move(allowed)}});
		}

		Json result = Json::object();
		result["town_id"] = town.base();
		result["company_id"] = actor.base();
		result["action"] = action_name;
		result["expected_cost"] = GetTownActionCost(action);
		result["rating_before"] = t->ratings[actor];
		const Backup<CompanyID> context(_current_company, actor);
		/* Commands::TownAction has CommandFlag::Location, so it is posted with the town tile. */
		const bool ok = RunCommandAt<Commands::TownAction>(t->xy, town, action);
		if (!_posted.captured) return AddNotExecuted(std::move(result));
		result = AddCommandResult(std::move(result));
		if (ok) result["rating_after"] = Town::Get(town)->ratings[actor];
		return result;
	}

	return MakeGap("world.towns", op);
}

/* --- world.project --- */

/**
 * Project specific operations: the power grid, the station town and the town districts.
 * @param params The method parameters.
 * @param company The company the call is made for.
 * @return The result document.
 */
Json ProjectOp(const Json &params, CompanyID company)
{
	std::string op;
	switch (ReadString(params, "op", op)) {
		case ParamState::Invalid: return MakeError("invalid_arguments", "`op` must be a string");
		case ParamState::Absent: return MakeError("invalid_arguments", "`op` is required");
		case ParamState::Present: break;
	}

	if (op == "power") {
		Json result = Json::object();
		result["ok"] = true;
		result["electricity_available"] = IsElectricityAvailable();

		TownID town = TownID::Invalid();
		if (const auto error = ReadTownId(params, "town_id", town); error.has_value()) return *error;
		if (town != TownID::Invalid()) {
			if (const auto error = CheckTown(town); error.has_value()) return *error;
			result["town"] = TownNeeds(town);
			return result;
		}

		int64_t industry_id = 0;
		switch (ReadInt(params, "industry_id", industry_id)) {
			case ParamState::Invalid: return MakeError("invalid_arguments", "`industry_id` must be an integer");
			case ParamState::Present: {
				if (industry_id < 0 || industry_id >= static_cast<int64_t>(IndustryID::End().base())) {
					return MakeError("unknown_industry", "the industry id is out of range", {{"industry_id", industry_id}});
				}
				const Industry *i = Industry::GetIfValid(IndustryID(static_cast<uint16_t>(industry_id)));
				if (i == nullptr) return MakeError("unknown_industry", "there is no industry with that id", {{"industry_id", industry_id}});
				if (!IsPowerPlant(i)) return MakeError("not_a_power_plant", "the industry is not a power plant", {{"industry_id", industry_id}});
				result["plant"] = PowerPlantInfo(i);
				return result;
			}
			case ParamState::Absent:
				break;
		}

		/* No id given: list every power plant. */
		Json plants = Json::array();
		for (const Industry *i : Industry::Iterate()) {
			if (IsPowerPlant(i)) plants.push_back(PowerPlantInfo(i));
		}
		result["plants"] = std::move(plants);
		return result;
	}

	if (op == "power_fuel") {
		IndustryID plant = IndustryID::Invalid();
		int64_t industry_id = 0;
		switch (ReadInt(params, "industry_id", industry_id)) {
			case ParamState::Invalid: return MakeError("invalid_arguments", "`industry_id` must be an integer");
			case ParamState::Present: {
				if (industry_id < 0 || industry_id >= static_cast<int64_t>(IndustryID::End().base())) {
					return MakeError("unknown_industry", "the industry id is out of range", {{"industry_id", industry_id}});
				}
				plant = IndustryID(static_cast<uint16_t>(industry_id));
				const Industry *i = Industry::GetIfValid(plant);
				if (i == nullptr) return MakeError("unknown_industry", "there is no industry with that id", {{"industry_id", industry_id}});
				if (!IsPowerPlant(i)) return MakeError("not_a_power_plant", "the industry is not a power plant", {{"industry_id", industry_id}});
				break;
			}
			case ParamState::Absent:
				break;
		}

		int64_t amount = 0;
		switch (ReadInt(params, "amount", amount)) {
			case ParamState::Invalid: return MakeError("invalid_arguments", "`amount` must be an integer");
			case ParamState::Absent: return MakeError("invalid_arguments", "`amount` is required");
			case ParamState::Present: break;
		}
		if (amount <= 0 || amount > UINT16_MAX) {
			return MakeError("invalid_amount", "the amount is out of range", {{"amount", amount}, {"min", 1}, {"max", UINT16_MAX}});
		}

		Json result = Json::object();
		result["industry_id"] = plant == IndustryID::Invalid() ? Json(nullptr) : Json(plant.base());
		result["amount"] = amount;
		const Backup<CompanyID> context(_current_company, OWNER_DEITY);
		const bool ok = RunCommand<Commands::PowerPlantAddFuel>(plant, static_cast<uint16_t>(amount));
		if (!_posted.captured) return AddNotExecuted(std::move(result));
		result = AddCommandResult(std::move(result));
		if (ok && plant != IndustryID::Invalid()) result["fuel_after"] = GetPowerPlantFuelStock(Industry::Get(plant));
		return result;
	}

	if (op == "station_town") {
		int64_t station_id = 0;
		switch (ReadInt(params, "station_id", station_id)) {
			case ParamState::Invalid: return MakeError("invalid_arguments", "`station_id` must be an integer");
			case ParamState::Absent: return MakeError("invalid_arguments", "`station_id` is required");
			case ParamState::Present: break;
		}
		if (station_id < 0 || station_id >= static_cast<int64_t>(StationID::End().base())) {
			return MakeError("unknown_station", "the station id is out of range", {{"station_id", station_id}});
		}
		const Station *st = Station::GetIfValid(StationID(static_cast<uint16_t>(station_id)));
		if (st == nullptr) return MakeError("unknown_station", "there is no station with that id", {{"station_id", station_id}});

		TownID town = TownID::Invalid();
		if (const auto error = ReadTownId(params, "town_id", town); error.has_value()) return *error;
		if (town == TownID::Invalid()) return MakeError("invalid_arguments", "`town_id` is required");
		if (const auto error = CheckTown(town); error.has_value()) return *error;

		/* The engine only accepts a town that is a candidate of the station; report the candidates. */
		Json candidates = Json::array();
		bool is_candidate = false;
		for (const Town *candidate : GetStationTownCandidates(st)) {
			candidates.push_back(candidate->index.base());
			if (candidate->index == town) is_candidate = true;
		}
		if (!is_candidate) {
			return MakeError("town_not_a_candidate", "the town is not one of the towns nearest to the station",
					{{"station_id", station_id}, {"town_id", town.base()}, {"candidates", std::move(candidates)}});
		}

		Json result = Json::object();
		result["station_id"] = station_id;
		result["town_id"] = town.base();
		result["town_before"] = st->town == nullptr ? Json(nullptr) : Json(st->town->index.base());
		const Backup<CompanyID> context(_current_company, OWNER_DEITY);
		const bool ok = RunCommand<Commands::SetStationTown>(st->index, town);
		if (!_posted.captured) return AddNotExecuted(std::move(result));
		result = AddCommandResult(std::move(result));
		if (ok) {
			const Station *after = Station::GetIfValid(st->index);
			result["town_after"] = (after == nullptr || after->town == nullptr) ? Json(nullptr) : Json(after->town->index.base());
		}
		return result;
	}

	if (op == "district") {
		/* The argument shape is checked before the world is touched. */
		TileIndex tile = INVALID_TILE;
		switch (ReadTile(params, "tile", tile)) {
			case ParamState::Invalid: return MakeError("invalid_arguments", "`tile` must be a `{x, y}` object inside the map");
			case ParamState::Absent: return MakeError("invalid_arguments", "`tile` is required");
			case ParamState::Present: break;
		}

		TownID town = TownID::Invalid();
		if (const auto error = ReadTownId(params, "town_id", town); error.has_value()) return *error;
		if (town == TownID::Invalid()) return MakeError("invalid_arguments", "`town_id` is required");
		if (const auto error = CheckTown(town); error.has_value()) return *error;

		const Town *t = Town::Get(town);
		const TownDistrict district = GetTownDistrict(t, tile);
		Json result = Json::object();
		result["ok"] = true;
		result["town_id"] = town.base();
		result["tile"] = Json{{"x", TileX(tile)}, {"y", TileY(tile)}};
		result["district"] = GetTownDistrictName(district);
		result["districts_from_population"] = TOWN_DISTRICTS_MIN_POPULATION;
		result["population"] = t->cache.population;
		return result;
	}

	if (op == "catchment") {
		StationID station = StationID::Invalid();
		if (const auto error = ReadStationId(params, "station_id", station); error.has_value()) return *error;
		if (station != StationID::Invalid()) {
			if (const auto error = CheckStation(station); error.has_value()) return *error;
			Json result = Json::object();
			result["ok"] = true;
			result["station"] = StationCatchment(Station::Get(station));
			return result;
		}

		/* No id given: list the stations that have an owner and a facility, bounded like the other lists. */
		size_t offset = 0;
		size_t limit = 0;
		if (const auto error = ReadListWindow(params, offset, limit)) return *error;

		Json stations = Json::array();
		size_t total = 0;
		for (const Station *st : Station::Iterate()) {
			if (st->owner == OWNER_NONE || st->facilities.None()) continue;
			if (total >= offset && stations.size() < limit) stations.push_back(StationCatchment(st));
			total++;
		}
		Json result = Json::object();
		result["ok"] = true;
		result["stations"] = std::move(stations);
		result["total"] = total;
		result["offset"] = offset;
		result["limit"] = limit;
		return result;
	}

	if (op == "catchment_upgrade") {
		StationID station = StationID::Invalid();
		if (const auto error = ReadStationId(params, "station_id", station); error.has_value()) return *error;
		if (station == StationID::Invalid()) return MakeError("invalid_arguments", "`station_id` is required");
		if (const auto error = CheckStation(station); error.has_value()) return *error;

		const Station *st = Station::Get(station);
		if (st->catchment_level >= MAX_STATION_CATCHMENT_LEVEL) {
			return MakeError("catchment_max_level", "the station already has the maximum catchment level",
					{{"station_id", station.base()}, {"max", MAX_STATION_CATCHMENT_LEVEL}});
		}

		Json result = Json::object();
		result["station_id"] = station.base();
		result["catchment_level_before"] = st->catchment_level;
		result["expected_cost"] = static_cast<int64_t>(GetStationCatchmentUpgradeCost(st->catchment_level + 1));
		/* The command checks the ownership itself, so it runs as the station owner. */
		const Backup<CompanyID> context(_current_company, st->owner);
		const bool ok = RunCommand<Commands::UpgradeStationCatchment>(station);
		if (!_posted.captured) return AddNotExecuted(std::move(result));
		result = AddCommandResult(std::move(result));
		if (ok) result["catchment_level_after"] = Station::Get(station)->catchment_level;
		return result;
	}

	if (op == "settings") {
		Json result = Json::object();
		result["ok"] = true;
		result["settings"] = SettingRows({
				"station.modified_catchment",
				"economy.town_growth_rate",
				"economy.town_growth_base",
				"economy.town_cargo_scale",
				"economy.town_need_power_residents",
				"economy.town_power_link_radius",
				"economy.town_layout",
				"economy.found_town",
		}, company);
		result["notes"] = Json::array({
				"station.modified_catchment decides whether the catchment radius follows the station type or is a fixed square",
				"the town power settings decide which towns need electricity and how far a plant reaches",
				"the town growth settings decide the growth budget and the cargo scale of every town",
		});
		return result;
	}

	return MakeGap("world.project", op);
}

/**
 * Build one capability entry.
 * @param method The method name.
 * @param authority "read" or "write".
 * @param implemented The operations that work.
 * @param gaps The operations that do not work yet.
 * @return The entry.
 */
Json Capability(std::string_view method, std::string_view authority, std::initializer_list<std::string_view> implemented, std::initializer_list<std::string_view> gaps)
{
	Json entry = Json::object();
	entry["method"] = std::string(method);
	entry["authority"] = std::string(authority);
	entry["implemented"] = Json::array();
	for (const std::string_view op : implemented) entry["implemented"].push_back(std::string(op));
	entry["gaps"] = Json::array();
	for (const std::string_view op : gaps) entry["gaps"].push_back(std::string(op));
	return entry;
}

/* --- world.industry --- */

/**
 * Industry queries and the industry control commands.
 * @param params The method parameters.
 * @param company The company the call is made for.
 * @return The result document.
 */
Json IndustryOp(const Json &params, CompanyID company)
{
	std::string op;
	switch (ReadString(params, "op", op)) {
		case ParamState::Invalid: return MakeError("invalid_arguments", "`op` must be a string");
		case ParamState::Absent: return MakeError("invalid_arguments", "`op` is required");
		case ParamState::Present: break;
	}

	if (op == "list") {
		size_t offset = 0;
		size_t limit = 0;
		if (const auto error = ReadListWindow(params, offset, limit)) return *error;

		Json industries = Json::array();
		size_t total = 0;
		for (const Industry *i : Industry::Iterate()) {
			if (total >= offset && industries.size() < limit) industries.push_back(IndustryInfo(i));
			total++;
		}
		Json result = Json::object();
		result["ok"] = true;
		result["industries"] = std::move(industries);
		result["types"] = IndustryTypeList();
		result["total"] = total;
		result["offset"] = offset;
		result["limit"] = limit;
		return result;
	}

	if (op == "types") {
		Json result = Json::object();
		result["ok"] = true;
		result["types"] = IndustryTypeList();
		return result;
	}

	if (op == "info" || op == "needs" || op == "production" || op == "cargo") {
		IndustryID industry = IndustryID::Invalid();
		if (const auto error = ReadIndustryId(params, "industry_id", industry); error.has_value()) return *error;
		if (industry == IndustryID::Invalid()) return MakeError("invalid_arguments", "`industry_id` is required");
		if (const auto error = CheckIndustry(industry); error.has_value()) return *error;

		const Industry *i = Industry::Get(industry);
		Json result = Json::object();
		result["ok"] = true;
		if (op == "info") {
			result["industry"] = IndustryInfo(i);
		} else if (op == "needs") {
			result["industry"] = IndustryNeeds(i);
		} else if (op == "production") {
			result["industry"] = IndustryProduction(i);
		} else {
			result["industry"] = IndustryCargo(i);
		}
		return result;
	}

	if (op == "fund" || op == "prospect") {
		/* The argument shape is checked before the world is touched. */
		int64_t type = 0;
		switch (ReadInt(params, "industry_type", type)) {
			case ParamState::Invalid: return MakeError("invalid_arguments", "`industry_type` must be an integer");
			case ParamState::Absent: return MakeError("invalid_arguments", "`industry_type` is required");
			case ParamState::Present: break;
		}
		if (type < 0 || type >= static_cast<int64_t>(NUM_INDUSTRYTYPES)) {
			return MakeError("unknown_industry_type", "the industry type is out of range", {{"industry_type", type}, {"max", NUM_INDUSTRYTYPES - 1}});
		}
		const IndustrySpec *spec = GetIndustrySpec(static_cast<IndustryType>(type));
		if (!spec->enabled || spec->layouts.empty()) {
			return MakeError("unknown_industry_type", "the industry type is not available in this game", {{"industry_type", type}});
		}

		TileIndex tile = INVALID_TILE;
		switch (ReadTile(params, "tile", tile)) {
			case ParamState::Invalid: return MakeError("invalid_arguments", "`tile` must be an object with `x` and `y` inside the map");
			case ParamState::Absent:
				/* Prospecting picks the place itself; funding needs one. */
				if (op == "fund") return MakeError("invalid_arguments", "`tile` is required");
				tile = TileXY(0, 0);
				break;
			case ParamState::Present: break;
		}

		int64_t layout = 0;
		switch (ReadInt(params, "layout", layout)) {
			case ParamState::Invalid: return MakeError("invalid_arguments", "`layout` must be an integer");
			case ParamState::Present:
				if (layout < 0 || layout >= static_cast<int64_t>(spec->layouts.size())) {
					return MakeError("invalid_arguments", "the layout index is out of range",
							{{"layout", layout}, {"max", spec->layouts.size() - 1}});
				}
				break;
			case ParamState::Absent: break;
		}

		int64_t seed = 0;
		switch (ReadInt(params, "seed", seed)) {
			case ParamState::Invalid: return MakeError("invalid_arguments", "`seed` must be an integer");
			case ParamState::Present:
				if (seed < 0 || seed > UINT32_MAX) return MakeError("invalid_arguments", "the seed is out of range", {{"seed", seed}});
				break;
			case ParamState::Absent: seed = static_cast<int64_t>(Random()); break;
		}

		/* The new industry is read back from the pool, because the command only reports a cost. */
		std::vector<IndustryID> before;
		for (const Industry *i : Industry::Iterate()) before.push_back(i->index);

		Json result = Json::object();
		result["industry_type"] = type;
		result["type_name"] = GetString(spec->name);
		result["fund"] = op == "fund";
		result["seed"] = seed;
		if (op == "fund") {
			result["tile"] = Json{{"x", TileX(tile)}, {"y", TileY(tile)}};
			result["layout"] = layout;
		}
		const Backup<CompanyID> context(_current_company, OWNER_DEITY);
		const bool ok = RunCommand<Commands::BuildIndustry>(tile, static_cast<IndustryType>(type),
				static_cast<uint32_t>(layout), op == "fund", static_cast<uint32_t>(seed));
		if (!_posted.captured) return AddNotExecuted(std::move(result));
		result = AddCommandResult(std::move(result));
		if (ok) {
			Json created = Json::array();
			for (const Industry *i : Industry::Iterate()) {
				if (std::find(before.begin(), before.end(), i->index) == before.end()) created.push_back(IndustryInfo(i));
			}
			result["created"] = std::move(created);
			result["created_count"] = created.size();
		}
		return result;
	}

	if (op == "fund_area") {
		int64_t type = 0;
		switch (ReadInt(params, "industry_type", type)) {
			case ParamState::Invalid: return MakeError("invalid_arguments", "`industry_type` must be an integer");
			case ParamState::Absent: return MakeError("invalid_arguments", "`industry_type` is required");
			case ParamState::Present: break;
		}
		if (type < 0 || type >= static_cast<int64_t>(NUM_INDUSTRYTYPES)) {
			return MakeError("unknown_industry_type", "the industry type is out of range", {{"industry_type", type}, {"max", NUM_INDUSTRYTYPES - 1}});
		}
		const IndustrySpec *spec = GetIndustrySpec(static_cast<IndustryType>(type));
		if (!spec->enabled || spec->layouts.empty()) {
			return MakeError("unknown_industry_type", "the industry type is not available in this game", {{"industry_type", type}});
		}

		TileIndex tile = INVALID_TILE;
		switch (ReadTile(params, "tile", tile)) {
			case ParamState::Invalid: return MakeError("invalid_arguments", "`tile` must be an object with `x` and `y` inside the map");
			case ParamState::Absent: return MakeError("invalid_arguments", "`tile` is required");
			case ParamState::Present: break;
		}
		TileIndex start_tile = tile;
		switch (ReadTile(params, "start_tile", start_tile)) {
			case ParamState::Invalid: return MakeError("invalid_arguments", "`start_tile` must be an object with `x` and `y` inside the map");
			case ParamState::Present: break;
			case ParamState::Absent: break;
		}

		int64_t seed = 0;
		switch (ReadInt(params, "seed", seed)) {
			case ParamState::Invalid: return MakeError("invalid_arguments", "`seed` must be an integer");
			case ParamState::Present:
				if (seed < 0 || seed > UINT32_MAX) return MakeError("invalid_arguments", "the seed is out of range", {{"seed", seed}});
				break;
			case ParamState::Absent: seed = static_cast<int64_t>(Random()); break;
		}

		std::vector<IndustryID> before;
		for (const Industry *i : Industry::Iterate()) before.push_back(i->index);

		Json result = Json::object();
		result["industry_type"] = type;
		result["type_name"] = GetString(spec->name);
		result["tile"] = Json{{"x", TileX(tile)}, {"y", TileY(tile)}};
		result["start_tile"] = Json{{"x", TileX(start_tile)}, {"y", TileY(start_tile)}};
		result["seed"] = seed;
		const Backup<CompanyID> context(_current_company, OWNER_DEITY);
		const bool ok = RunBuildIndustryArea<Commands::BuildIndustryArea>(tile, start_tile, static_cast<IndustryType>(type), static_cast<uint32_t>(seed));
		if (!_posted_industry_area.captured) return AddNotExecuted(std::move(result));
		result = AddCommandResult(std::move(result), _posted_industry_area.cost);
		result["built_count"] = _posted_industry_area.count;
		if (ok) {
			Json created = Json::array();
			for (const Industry *i : Industry::Iterate()) {
				if (std::find(before.begin(), before.end(), i->index) == before.end()) created.push_back(IndustryInfo(i));
			}
			result["created"] = std::move(created);
		}
		return result;
	}

	if (op == "set_production") {
		IndustryID industry = IndustryID::Invalid();
		if (const auto error = ReadIndustryId(params, "industry_id", industry); error.has_value()) return *error;
		if (industry == IndustryID::Invalid()) return MakeError("invalid_arguments", "`industry_id` is required");
		if (const auto error = CheckIndustry(industry); error.has_value()) return *error;

		int64_t level = 0;
		switch (ReadInt(params, "prod_level", level)) {
			case ParamState::Invalid: return MakeError("invalid_arguments", "`prod_level` must be an integer");
			case ParamState::Absent: return MakeError("invalid_arguments", "`prod_level` is required");
			case ParamState::Present: break;
		}
		if (level < PRODLEVEL_MINIMUM || level > PRODLEVEL_MAXIMUM) {
			return MakeError("invalid_arguments", "the production level is out of range",
					{{"prod_level", level}, {"min", PRODLEVEL_MINIMUM}, {"max", PRODLEVEL_MAXIMUM}});
		}

		bool show_news = false;
		switch (ReadBool(params, "show_news", show_news)) {
			case ParamState::Invalid: return MakeError("invalid_arguments", "`show_news` must be a boolean");
			case ParamState::Present: break;
			case ParamState::Absent: break;
		}

		Json result = Json::object();
		result["industry_id"] = industry.base();
		result["prod_level"] = level;
		result["prod_level_before"] = Industry::Get(industry)->prod_level;
		const Backup<CompanyID> context(_current_company, OWNER_DEITY);
		const bool ok = RunCommand<Commands::IndustrySetProduction>(industry, static_cast<uint8_t>(level), show_news, EncodedString{});
		if (!_posted.captured) return AddNotExecuted(std::move(result));
		result = AddCommandResult(std::move(result));
		if (ok) {
			const Industry *i = Industry::Get(industry);
			result["prod_level_after"] = i->prod_level;
			result["external_prod_level"] = i->ctlflags.Test(IndustryControlFlag::ExternalProdLevel);
		}
		return result;
	}

	if (op == "set_flags") {
		IndustryID industry = IndustryID::Invalid();
		if (const auto error = ReadIndustryId(params, "industry_id", industry); error.has_value()) return *error;
		if (industry == IndustryID::Invalid()) return MakeError("invalid_arguments", "`industry_id` is required");
		if (const auto error = CheckIndustry(industry); error.has_value()) return *error;

		IndustryControlFlags flags;
		const auto flags_it = params.find("flags");
		if (flags_it != params.end() && !flags_it->is_null()) {
			if (!flags_it->is_array()) return MakeError("invalid_arguments", "`flags` must be an array of control flag names");
			for (const Json &item : *flags_it) {
				if (!item.is_string()) return MakeError("invalid_arguments", "`flags` must be an array of control flag names");
				const std::string name = item.get<std::string>();
				IndustryControlFlag flag = IndustryControlFlag::NoClosure;
				if (!ParseIndustryControlFlagName(name, flag)) {
					Json allowed = Json::array();
					for (const IndustryControlFlag candidate : EnumRange(IndustryControlFlag::End)) allowed.push_back(IndustryControlFlagName(candidate));
					return MakeError("invalid_arguments", "unknown industry control flag", {{"flag", name}, {"allowed", std::move(allowed)}});
				}
				flags.Set(flag);
			}
		}

		Json result = Json::object();
		result["industry_id"] = industry.base();
		Json requested = Json::array();
		for (const IndustryControlFlag flag : EnumRange(IndustryControlFlag::End)) {
			if (flags.Test(flag)) requested.push_back(IndustryControlFlagName(flag));
		}
		result["flags"] = std::move(requested);
		const Backup<CompanyID> context(_current_company, OWNER_DEITY);
		const bool ok = RunCommand<Commands::IndustrySetFlags>(industry, flags);
		if (!_posted.captured) return AddNotExecuted(std::move(result));
		result = AddCommandResult(std::move(result));
		if (ok) {
			Json after = Json::array();
			for (const IndustryControlFlag flag : EnumRange(IndustryControlFlag::End)) {
				if (Industry::Get(industry)->ctlflags.Test(flag)) after.push_back(IndustryControlFlagName(flag));
			}
			result["flags_after"] = std::move(after);
		}
		return result;
	}

	if (op == "set_exclusivity") {
		IndustryID industry = IndustryID::Invalid();
		if (const auto error = ReadIndustryId(params, "industry_id", industry); error.has_value()) return *error;
		if (industry == IndustryID::Invalid()) return MakeError("invalid_arguments", "`industry_id` is required");
		if (const auto error = CheckIndustry(industry); error.has_value()) return *error;

		bool consumer = false;
		switch (ReadBool(params, "consumer", consumer)) {
			case ParamState::Invalid: return MakeError("invalid_arguments", "`consumer` must be a boolean");
			case ParamState::Present: break;
			case ParamState::Absent: break;
		}

		/* A null company clears the exclusive right; otherwise the company must exist. */
		Owner exclusive = OWNER_NONE;
		const auto company_it = params.find("company_id");
		if (company_it != params.end() && !company_it->is_null()) {
			CompanyID holder = company;
			if (const auto error = ReadCompanyId(params, "company_id", holder); error.has_value()) return *error;
			if (const auto error = CheckCompany(holder); error.has_value()) return *error;
			exclusive = holder;
		}

		Json result = Json::object();
		result["industry_id"] = industry.base();
		result["consumer"] = consumer;
		result["company_id"] = exclusive == OWNER_NONE ? Json(nullptr) : Json(exclusive.base());
		const Backup<CompanyID> context(_current_company, OWNER_DEITY);
		const bool ok = RunCommand<Commands::IndustrySetExclusivity>(industry, exclusive, consumer);
		if (!_posted.captured) return AddNotExecuted(std::move(result));
		result = AddCommandResult(std::move(result));
		if (ok) {
			const Industry *i = Industry::Get(industry);
			const Owner after = consumer ? i->exclusive_consumer : i->exclusive_supplier;
			result["exclusive_after"] = after == INVALID_OWNER ? Json(nullptr) : Json(after.base());
		}
		return result;
	}

	return MakeGap("world.industry", op);
}

/* --- world.signs --- */

/**
 * Sign queries and the sign commands.
 * @param params The method parameters.
 * @param company The company the call is made for.
 * @return The result document.
 */
Json SignsOp(const Json &params, CompanyID company)
{
	std::string op;
	switch (ReadString(params, "op", op)) {
		case ParamState::Invalid: return MakeError("invalid_arguments", "`op` must be a string");
		case ParamState::Absent: return MakeError("invalid_arguments", "`op` is required");
		case ParamState::Present: break;
	}

	if (op == "list") {
		size_t offset = 0;
		size_t limit = 0;
		if (const auto error = ReadListWindow(params, offset, limit)) return *error;

		Json signs = Json::array();
		size_t total = 0;
		for (const Sign *si : Sign::Iterate()) {
			if (total >= offset && signs.size() < limit) signs.push_back(SignInfo(si));
			total++;
		}
		Json result = Json::object();
		result["ok"] = true;
		result["signs"] = std::move(signs);
		result["total"] = total;
		result["offset"] = offset;
		result["limit"] = limit;
		return result;
	}

	if (op == "info") {
		SignID sign = SignID::Invalid();
		if (const auto error = ReadSignId(params, "sign_id", sign); error.has_value()) return *error;
		if (sign == SignID::Invalid()) return MakeError("invalid_arguments", "`sign_id` is required");
		if (const auto error = CheckSign(sign); error.has_value()) return *error;
		Json result = Json::object();
		result["ok"] = true;
		result["sign"] = SignInfo(Sign::Get(sign));
		return result;
	}

	if (op == "place") {
		/* The text is checked before the tile, so a malformed call never reaches the map. */
		std::string text;
		switch (ReadString(params, "text", text)) {
			case ParamState::Invalid: return MakeError("invalid_arguments", "`text` must be a string");
			case ParamState::Absent: break;
			case ParamState::Present: break;
		}
		if (Utf8StringLength(text) >= MAX_LENGTH_SIGN_NAME_CHARS) {
			return MakeError("invalid_arguments", "the sign text is too long",
					{{"length", Utf8StringLength(text)}, {"max", MAX_LENGTH_SIGN_NAME_CHARS - 1}});
		}

		TileIndex tile = INVALID_TILE;
		switch (ReadTile(params, "tile", tile)) {
			case ParamState::Invalid: return MakeError("invalid_arguments", "`tile` must be an object with `x` and `y` inside the map");
			case ParamState::Absent: return MakeError("invalid_arguments", "`tile` is required");
			case ParamState::Present: break;
		}

		Json result = Json::object();
		result["tile"] = Json{{"x", TileX(tile)}, {"y", TileY(tile)}};
		result["text"] = text;
		const Backup<CompanyID> context(_current_company, company);
		const bool ok = RunPlaceSign<Commands::PlaceSign>(tile, text);
		if (!_posted_place_sign.captured) return AddNotExecuted(std::move(result));
		result = AddCommandResult(std::move(result), _posted_place_sign.cost);
		if (ok && Sign::IsValidID(_posted_place_sign.sign)) {
			result["sign_id"] = _posted_place_sign.sign.base();
			result["sign"] = SignInfo(Sign::Get(_posted_place_sign.sign));
		}
		return result;
	}

	if (op == "rename" || op == "delete") {
		SignID sign = SignID::Invalid();
		if (const auto error = ReadSignId(params, "sign_id", sign); error.has_value()) return *error;
		if (sign == SignID::Invalid()) return MakeError("invalid_arguments", "`sign_id` is required");
		if (const auto error = CheckSign(sign); error.has_value()) return *error;

		std::string text;
		if (op == "rename") {
			switch (ReadString(params, "text", text)) {
				case ParamState::Invalid: return MakeError("invalid_arguments", "`text` must be a string");
				case ParamState::Absent: return MakeError("invalid_arguments", "`text` is required");
				case ParamState::Present: break;
			}
			if (text.empty()) return MakeError("invalid_arguments", "`text` must not be empty; use op=delete to remove the sign");
			if (Utf8StringLength(text) >= MAX_LENGTH_SIGN_NAME_CHARS) {
				return MakeError("invalid_arguments", "the sign text is too long",
						{{"length", Utf8StringLength(text)}, {"max", MAX_LENGTH_SIGN_NAME_CHARS - 1}});
			}
		}

		/* The text colour is only meaningful for deity signs; an absent colour keeps the current one. */
		Colours colour = Colours::Invalid;
		const auto colour_it = params.find("text_colour");
		if (colour_it != params.end() && !colour_it->is_null()) {
			if (!ParseColour(*colour_it, colour)) {
				return MakeError("invalid_arguments", "`text_colour` must be a colour name or a palette index", {{"text_colour", *colour_it}});
			}
		}

		Json result = Json::object();
		result["sign_id"] = sign.base();
		result["text"] = text;
		result["text_colour"] = colour == Colours::Invalid ? Json(nullptr) : Json(ColourName(colour));
		const Backup<CompanyID> context(_current_company, OWNER_DEITY);
		const bool ok = RunCommand<Commands::RenameSign>(sign, text, colour);
		if (!_posted.captured) return AddNotExecuted(std::move(result));
		result = AddCommandResult(std::move(result));
		if (ok) {
			if (op == "delete") {
				result["deleted"] = !Sign::IsValidID(sign);
			} else {
				result["sign"] = SignInfo(Sign::Get(sign));
			}
		}
		return result;
	}

	if (op == "move") {
		SignID sign = SignID::Invalid();
		if (const auto error = ReadSignId(params, "sign_id", sign); error.has_value()) return *error;
		if (sign == SignID::Invalid()) return MakeError("invalid_arguments", "`sign_id` is required");
		if (const auto error = CheckSign(sign); error.has_value()) return *error;

		TileIndex tile = INVALID_TILE;
		switch (ReadTile(params, "tile", tile)) {
			case ParamState::Invalid: return MakeError("invalid_arguments", "`tile` must be an object with `x` and `y` inside the map");
			case ParamState::Absent: return MakeError("invalid_arguments", "`tile` is required");
			case ParamState::Present: break;
		}

		Json result = Json::object();
		result["sign_id"] = sign.base();
		result["tile"] = Json{{"x", TileX(tile)}, {"y", TileY(tile)}};
		const Backup<CompanyID> context(_current_company, OWNER_DEITY);
		const bool ok = RunCommand<Commands::MoveSign>(sign, tile);
		if (!_posted.captured) return AddNotExecuted(std::move(result));
		result = AddCommandResult(std::move(result));
		if (ok) result["sign"] = SignInfo(Sign::Get(sign));
		return result;
	}

	return MakeGap("world.signs", op);
}

/* --- world.landscape --- */

/**
 * Object and tree planting, the landscape commands this module owns.
 * @param params The method parameters.
 * @param company The company the call is made for.
 * @return The result document.
 */
Json LandscapeOp(const Json &params, CompanyID company)
{
	std::string op;
	switch (ReadString(params, "op", op)) {
		case ParamState::Invalid: return MakeError("invalid_arguments", "`op` must be a string");
		case ParamState::Absent: return MakeError("invalid_arguments", "`op` is required");
		case ParamState::Present: break;
	}

	if (op == "object_types") {
		size_t offset = 0;
		size_t limit = 0;
		if (const auto error = ReadListWindow(params, offset, limit)) return *error;

		Json types = Json::array();
		size_t total = 0;
		for (const Json &row : ObjectTypeList()) {
			if (total >= offset && types.size() < limit) types.push_back(row);
			total++;
		}
		Json result = Json::object();
		result["ok"] = true;
		result["types"] = std::move(types);
		result["total"] = total;
		result["offset"] = offset;
		result["limit"] = limit;
		return result;
	}

	if (op == "object" || op == "object_area") {
		int64_t type = 0;
		switch (ReadInt(params, "object_type", type)) {
			case ParamState::Invalid: return MakeError("invalid_arguments", "`object_type` must be an integer");
			case ParamState::Absent: return MakeError("invalid_arguments", "`object_type` is required");
			case ParamState::Present: break;
		}
		if (type < 0 || type >= static_cast<int64_t>(ObjectSpec::Count())) {
			return MakeError("unknown_object_type", "the object type is out of range", {{"object_type", type}, {"max", ObjectSpec::Count() - 1}});
		}
		const ObjectSpec *spec = ObjectSpec::Get(static_cast<ObjectType>(type));
		if (!spec->IsEnabled()) {
			return MakeError("unknown_object_type", "the object type is not available in this game", {{"object_type", type}});
		}

		int64_t view = 0;
		switch (ReadInt(params, "view", view)) {
			case ParamState::Invalid: return MakeError("invalid_arguments", "`view` must be an integer");
			case ParamState::Present:
				if (view < 0 || view >= static_cast<int64_t>(spec->views)) {
					return MakeError("invalid_arguments", "the view is out of range", {{"view", view}, {"max", spec->views - 1}});
				}
				break;
			case ParamState::Absent: break;
		}

		TileIndex tile = INVALID_TILE;
		switch (ReadTile(params, "tile", tile)) {
			case ParamState::Invalid: return MakeError("invalid_arguments", "`tile` must be an object with `x` and `y` inside the map");
			case ParamState::Absent: return MakeError("invalid_arguments", "`tile` is required");
			case ParamState::Present: break;
		}

		Json result = Json::object();
		result["object_type"] = type;
		result["name"] = GetString(spec->name);
		result["view"] = view;
		result["tile"] = Json{{"x", TileX(tile)}, {"y", TileY(tile)}};
		const Backup<CompanyID> context(_current_company, company);
		bool ok;
		if (op == "object") {
			ok = RunCommand<Commands::BuildObject>(tile, static_cast<ObjectType>(type), static_cast<uint8_t>(view));
		} else {
			TileIndex start_tile = tile;
			switch (ReadTile(params, "start_tile", start_tile)) {
				case ParamState::Invalid: return MakeError("invalid_arguments", "`start_tile` must be an object with `x` and `y` inside the map");
				case ParamState::Present: break;
				case ParamState::Absent: break;
			}
			bool diagonal = false;
			switch (ReadBool(params, "diagonal", diagonal)) {
				case ParamState::Invalid: return MakeError("invalid_arguments", "`diagonal` must be a boolean");
				case ParamState::Present: break;
				case ParamState::Absent: break;
			}
			result["start_tile"] = Json{{"x", TileX(start_tile)}, {"y", TileY(start_tile)}};
			result["diagonal"] = diagonal;
			ok = RunCommand<Commands::BuildObjectArea>(tile, start_tile, static_cast<ObjectType>(type), static_cast<uint8_t>(view), diagonal);
		}
		if (!_posted.captured) return AddNotExecuted(std::move(result));
		result = AddCommandResult(std::move(result));
		if (ok) result["built"] = true;
		return result;
	}

	if (op == "tree" || op == "tree_area") {
		/* TREE_INVALID lets the engine pick a tree type that fits the climate. */
		int64_t tree = TREE_INVALID;
		switch (ReadInt(params, "tree_type", tree)) {
			case ParamState::Invalid: return MakeError("invalid_arguments", "`tree_type` must be an integer");
			case ParamState::Present:
				if (tree < 0 || tree > UINT8_MAX) return MakeError("invalid_arguments", "the tree type is out of range", {{"tree_type", tree}, {"max", UINT8_MAX}});
				break;
			case ParamState::Absent: break;
		}

		TileIndex tile = INVALID_TILE;
		switch (ReadTile(params, "tile", tile)) {
			case ParamState::Invalid: return MakeError("invalid_arguments", "`tile` must be an object with `x` and `y` inside the map");
			case ParamState::Absent: return MakeError("invalid_arguments", "`tile` is required");
			case ParamState::Present: break;
		}

		TileIndex start_tile = tile;
		switch (ReadTile(params, "start_tile", start_tile)) {
			case ParamState::Invalid: return MakeError("invalid_arguments", "`start_tile` must be an object with `x` and `y` inside the map");
			case ParamState::Present: break;
			case ParamState::Absent: break;
		}

		bool diagonal = false;
		switch (ReadBool(params, "diagonal", diagonal)) {
			case ParamState::Invalid: return MakeError("invalid_arguments", "`diagonal` must be a boolean");
			case ParamState::Present: break;
			case ParamState::Absent: break;
		}

		Json result = Json::object();
		result["tile"] = Json{{"x", TileX(tile)}, {"y", TileY(tile)}};
		result["start_tile"] = Json{{"x", TileX(start_tile)}, {"y", TileY(start_tile)}};
		result["tree_type"] = tree == TREE_INVALID ? Json(nullptr) : Json(tree);
		result["diagonal"] = diagonal;
		const Backup<CompanyID> context(_current_company, company);
		const bool ok = RunCommand<Commands::PlantTree>(tile, start_tile, static_cast<uint8_t>(tree), diagonal);
		if (!_posted.captured) return AddNotExecuted(std::move(result));
		result = AddCommandResult(std::move(result));
		if (ok) result["planted"] = true;
		return result;
	}

	return MakeGap("world.landscape", op);
}

/* --- world.cheat --- */

/**
 * The money cheats, which only run in a single player game.
 * @param params The method parameters.
 * @param company The company the call is made for.
 * @return The result document.
 */
Json CheatOp(const Json &params, CompanyID company)
{
	std::string op;
	switch (ReadString(params, "op", op)) {
		case ParamState::Invalid: return MakeError("invalid_arguments", "`op` must be a string");
		case ParamState::Absent: return MakeError("invalid_arguments", "`op` is required");
		case ParamState::Present: break;
	}

	if (op == "money") {
		int64_t amount = 0;
		switch (ReadInt(params, "amount", amount)) {
			case ParamState::Invalid: return MakeError("invalid_arguments", "`amount` must be an integer");
			case ParamState::Absent: return MakeError("invalid_arguments", "`amount` is required");
			case ParamState::Present: break;
		}
		if (amount <= 0) return MakeError("invalid_amount", "the amount must be positive", {{"amount", amount}});

		Json result = Json::object();
		result["amount"] = amount;
		result["company_id"] = company == CompanyID::Invalid() ? Json(nullptr) : Json(company.base());
		const Backup<CompanyID> context(_current_company, company);
		const bool ok = RunCommand<Commands::MoneyCheat>(Money(amount));
		if (!_posted.captured) return AddNotExecuted(std::move(result));
		result = AddCommandResult(std::move(result));
		if (ok && Company::IsValidID(company)) result["money_after"] = static_cast<int64_t>(Company::Get(company)->money);
		return result;
	}

	if (op == "bank_balance") {
		CompanyID target = company;
		if (const auto error = ReadCompanyId(params, "company_id", target); error.has_value()) return *error;
		if (const auto error = CheckCompany(target); error.has_value()) return *error;

		int64_t delta = 0;
		switch (ReadInt(params, "delta", delta)) {
			case ParamState::Invalid: return MakeError("invalid_arguments", "`delta` must be an integer");
			case ParamState::Absent: return MakeError("invalid_arguments", "`delta` is required");
			case ParamState::Present: break;
		}
		if (delta == 0) return MakeError("invalid_amount", "the delta must not be zero", {{"delta", delta}});

		Json result = Json::object();
		result["company_id"] = target.base();
		result["delta"] = delta;
		result["money_before"] = static_cast<int64_t>(Company::Get(target)->money);
		const Backup<CompanyID> context(_current_company, OWNER_DEITY);
		const bool ok = RunCommand<Commands::ChangeBankBalance>(INVALID_TILE, Money(delta), target, ExpensesType::Other);
		if (!_posted.captured) return AddNotExecuted(std::move(result));
		result = AddCommandResult(std::move(result));
		if (ok) result["money_after"] = static_cast<int64_t>(Company::Get(target)->money);
		return result;
	}

	return MakeGap("world.cheat", op);
}

} // anonymous namespace

void AgentAdminCommandCallback([[maybe_unused]] Commands cmd, const CommandCost &result, [[maybe_unused]] TileIndex tile)
{
	_posted.captured = true;
	_posted.cost = result;
}

void AgentAdminFoundTownCallback([[maybe_unused]] Commands cmd, const CommandCost &result, Money cost, TownID town_id)
{
	_posted_found_town.captured = true;
	_posted_found_town.cost = result;
	_posted_found_town.town_cost = cost;
	_posted_found_town.town = town_id;
}

void AgentAdminPlaceSignCallback([[maybe_unused]] Commands cmd, const CommandCost &result, SignID sign_id)
{
	_posted_place_sign.captured = true;
	_posted_place_sign.cost = result;
	_posted_place_sign.sign = sign_id;
}

void AgentAdminBuildIndustryAreaCallback([[maybe_unused]] Commands cmd, const CommandCost &result, uint32_t count)
{
	_posted_industry_area.captured = true;
	_posted_industry_area.cost = result;
	_posted_industry_area.count = count;
}

Json AdminToolCapabilities()
{
	Json methods = Json::array();
	methods.push_back(Capability("world.settings.get", "read",
			{"game", "company", "client", "names", "prefix"},
			{"write"}));
	methods.push_back(Capability("world.settings.set", "write",
			{"game", "company", "bool", "int", "one_of_many", "many_of_many"},
			{"client_scope", "string_setting"}));
	methods.push_back(Capability("world.company", "write",
			{"info", "livery", "loan_increase", "loan_decrease", "set_max_loan", "create", "delete", "give_money",
					"colour", "livery_set", "manager_face", "rename", "rename_president", "buy"},
			{"hostile_takeover_of_a_human_company", "company_password"}));
	methods.push_back(Capability("world.towns", "write",
			{"list", "needs", "growth", "settings", "power", "force_need", "invest", "set_investment", "growth_rate",
					"expand", "cargo_goal", "rating", "destroy", "found", "rename", "town_action"},
			{"town_district_authority", "town_road_layout_change"}));
	methods.push_back(Capability("world.industry", "write",
			{"list", "types", "info", "needs", "production", "cargo", "fund", "fund_area", "prospect",
					"set_production", "set_flags", "set_exclusivity"},
			{"industry_text"}));
	methods.push_back(Capability("world.signs", "write",
			{"list", "info", "place", "rename", "delete", "move"},
			{"sign_owner_transfer"}));
	methods.push_back(Capability("world.landscape", "write",
			{"object_types", "object", "object_area", "tree", "tree_area"},
			{"clear_tile", "terraform", "build_canal", "build_lock", "build_bridge", "build_tunnel"}));
	methods.push_back(Capability("world.cheat", "write",
			{"money", "bank_balance"},
			{"magic_bulldozer", "ignore_vehicle_limits", "change_date"}));
	methods.push_back(Capability("world.project", "write",
			{"power", "power_fuel", "station_town", "district", "catchment", "catchment_upgrade", "settings"},
			{"save", "load"}));

	Json result = Json::object();
	result["module"] = "agent_admin_tools";
	result["methods"] = std::move(methods);
	result["unimplemented_methods"] = Json::array({"world.pause", "world.rename", "world.objects", "world.info", "world.capture", "task.barrier"});
	result["query_ops"] = Json::array({
			"world.settings.get",
			"world.company op=info",
			"world.company op=livery",
			"world.towns op=list",
			"world.towns op=needs",
			"world.towns op=growth",
			"world.towns op=settings",
			"world.towns op=power",
			"world.industry op=list",
			"world.industry op=types",
			"world.industry op=info",
			"world.industry op=needs",
			"world.industry op=production",
			"world.industry op=cargo",
			"world.signs op=list",
			"world.signs op=info",
			"world.landscape op=object_types",
			"world.project op=power",
			"world.project op=district",
			"world.project op=catchment",
			"world.project op=settings",
	});
	result["notes"] = Json::array({
		"Actual mutations go through Command<Commands::...>::Post, the normal player command path, so the engine's own eligibility, pause and finance checks apply; results carry the actual cost and the engine error.",
		"world.settings.get returns the setting descriptors with their current value, type, scope, allowed values or range, default and whether they can be changed now.",
		"world.settings.set validates the name, the scope, the type and the range before it posts Commands::ChangeSetting or Commands::ChangeCompanySetting, and reports the value the engine actually applied.",
		"world.company op=create uses Commands::CompanyControl with CompanyCtrlAction::NewAI, the only company creation the single player engine has; the created id is read back from the company pool.",
		"world.company op=colour and op=livery_set post Commands::SetCompanyColour as the company itself; op=manager_face posts Commands::SetCompanyManagerFace; op=rename and op=rename_president post the matching rename commands; op=buy posts Commands::BuyCompany and needs the company to be bankrupt-asked or an AI for a hostile takeover.",
		"world.towns op=invest is paid by the company and reports the real cost; op=set_investment, op=force_need, op=growth_rate, op=expand, op=cargo_goal, op=rating and op=destroy are the free administrative commands and run as the deity.",
		"world.towns op=found posts Commands::FoundTown as the deity, so the random location form is not available; the new town id comes from the command callback, and an automatic name is retried a few times when the engine reports a duplicate.",
		"world.towns op=rename posts Commands::RenameTown, which the engine runs as the server; op=town_action posts Commands::TownAction as the company and reports the rating before and after.",
		"world.towns op=growth, op=settings and op=power are queries: they report the town state together with the settings that actually drive it, so the caller can see the dependency instead of guessing.",
		"world.industry op=fund and op=prospect post Commands::BuildIndustry as the deity; op=fund_area posts Commands::BuildIndustryArea and reports the count from the callback; the created industries are read back from the pool because the command only reports a cost.",
		"world.industry op=set_production, op=set_flags and op=set_exclusivity post the matching deity commands and report the state after the command.",
		"world.signs op=place posts Commands::PlaceSign as the company and reports the new sign id from the callback; op=rename, op=delete and op=move post the deity sign commands.",
		"world.landscape op=object and op=object_area post Commands::BuildObject and Commands::BuildObjectArea as the company; op=tree and op=tree_area post Commands::PlantTree and let the engine pick a tree type when `tree_type` is absent.",
		"world.cheat op=money posts Commands::MoneyCheat, which is a single player only command and reports the income as a negative cost; op=bank_balance posts Commands::ChangeBankBalance as the deity.",
		"world.project op=station_town only accepts a town that is a candidate of the station and reports the candidates otherwise; op=district reports the district of one tile of a town.",
		"world.project op=catchment is a query; op=catchment_upgrade posts Commands::UpgradeStationCatchment as the station owner and reports the level before and after.",
		"Every list query (world.industry op=list, world.signs op=list, world.towns op=list, world.landscape op=object_types, world.project op=catchment) is bounded: `limit` defaults to 64, `limit` is at most 512 and `offset` skips rows, and the result always reports the `total` number of rows.",
		"world.industry op=set_flags covers the closure control (the no_closure flag); op=set_production reports the production level before and after, and the engine marks the industry as externally controlled.",
		"world.project op=save/load is not implemented: the durable save lifecycle is left to the bridge.",
	});

	return result;
}

bool IsAdminMutation(std::string_view method, const Json &params)
{
	if (method == "world.settings.set") return true;
	if (method != "world.company" && method != "world.towns" && method != "world.project" &&
			method != "world.industry" && method != "world.signs" && method != "world.landscape" &&
			method != "world.cheat") {
		return false;
	}

	std::string op;
	if (ReadString(params, "op", op) != ParamState::Present) return true;
	if (method == "world.company") return op != "info" && op != "livery";
	if (method == "world.towns") return op != "list" && op != "needs" && op != "growth" && op != "settings" && op != "power";
	if (method == "world.industry") return op != "list" && op != "types" && op != "info" && op != "needs" && op != "production" && op != "cargo";
	if (method == "world.signs") return op != "list" && op != "info";
	if (method == "world.landscape") return op != "object_types";
	return op != "power" && op != "district" && op != "catchment" && op != "settings";
}

std::optional<Json> DispatchAdminTool(std::string_view method, const Json &params, CompanyID company)
{
	if (method != "world.settings.get" && method != "world.settings.set" &&
			method != "world.company" && method != "world.towns" && method != "world.project" &&
			method != "world.industry" && method != "world.signs" && method != "world.landscape" &&
			method != "world.cheat") {
		return std::nullopt;
	}
	if (!params.is_object()) return MakeError("invalid_arguments", "params must be an object");

	if (method == "world.settings.get") return SettingsGet(params, company);
	if (method == "world.settings.set") return SettingsSet(params, company);
	if (method == "world.company") return CompanyOp(params, company);
	if (method == "world.towns") return TownsOp(params, company);
	if (method == "world.industry") return IndustryOp(params, company);
	if (method == "world.signs") return SignsOp(params, company);
	if (method == "world.landscape") return LandscapeOp(params, company);
	if (method == "world.cheat") return CheatOp(params, company);
	return ProjectOp(params, company);
}

} // namespace InGameAgent
