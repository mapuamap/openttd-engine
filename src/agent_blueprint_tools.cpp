/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file agent_blueprint_tools.cpp Rail blueprint and trunk line tool methods of the in-game agent.
 *
 * The module is a thin, bounded shell around the engine's own tools: CaptureBlueprint, LoadBlueprint,
 * ListBlueprints, PlanBlueprintPaste and PlanRailRoute do the work, and the commands that build the
 * result are the normal Commands::PasteBlueprint and Commands::PrepareRailRoute, one chunk per call.
 *
 * There is no timer and no background job here on purpose: the player's own blueprint paste job
 * (StartBlueprintPaste) keeps running on its own, and a second autonomous job would keep building
 * after the bridge stopped calling and would compete with the player's job for the same chunk queue.
 * Instead the bridge executes one chunk per call and gets the engine's real result for that chunk.
 */

#include "stdafx.h"
#include "agent_blueprint_tools.h"

#include "blueprint.h"
#include "blueprint_cmd.h"
#include "command_func.h"
#include "company_base.h"
#include "company_func.h"
#include "core/backup_type.hpp"
#include "map_func.h"
#include "rail.h"
#include "rail_route.h"
#include "rail_route_cmd.h"
#include "rail_type.h"
#include "openttd.h"
#include "string_func.h"
#include "strings_func.h"
#include "window_func.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <deque>
#include <optional>
#include <string>
#include <vector>

#include "table/strings.h"

#include "safeguards.h"

namespace InGameAgent {

namespace {

/** Maximum number of bytes of a blueprint file this module loads. */
constexpr size_t MAX_BLUEPRINT_FILE_BYTES = 4 * 1024 * 1024;
/** Maximum number of tiles one capture may cover. */
constexpr size_t MAX_CAPTURE_TILES = 8192;
/** Maximum number of plans the store keeps. */
constexpr size_t MAX_PLANS = 8;
/** Maximum number of chunks of one plan. */
constexpr size_t MAX_PLAN_CHUNKS = 1024;
/** Maximum number of chunk bytes of one plan. */
constexpr size_t MAX_PLAN_BYTES = 512 * 1024;
/** Maximum number of chunk bytes of the whole plan store. */
constexpr size_t MAX_PLAN_STORE_BYTES = 2 * 1024 * 1024;
/** Maximum number of chunk commands that may wait for their result. */
constexpr size_t MAX_IN_FLIGHT = 16;
/** Default number of rows returned by one bounded list query. */
constexpr size_t DEFAULT_LIST_LIMIT = 64;
/** Maximum number of rows returned by one bounded list query. */
constexpr size_t MAX_LIST_LIMIT = 256;
/** Default number of blueprint items returned by one inspect or capture call. */
constexpr size_t DEFAULT_ITEM_LIMIT = 64;
/** Maximum number of blueprint items returned by one inspect or capture call. */
constexpr size_t MAX_ITEM_LIMIT = 512;
/** Maximum number of chunk sizes reported in a plan document. */
constexpr size_t MAX_CHUNK_SIZES = 64;
/** Maximum length of a blueprint name, in bytes. */
constexpr size_t MAX_NAME_BYTES = 48;
/** Maximum length of a task id or world id, in bytes. */
constexpr size_t MAX_BINDING_BYTES = 128;

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
	return ParamState::Invalid;
}

/**
 * Read an optional `{x, y}` tile object inside the map.
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
 * @param params The method parameters.
 * @param default_limit The number of rows used when `limit` is not given.
 * @param max_limit The largest number of rows a call may ask for.
 * @param offset Receives the number of rows to skip.
 * @param limit Receives the maximum number of rows to return.
 * @return An error document, or nothing when the window is usable.
 */
std::optional<Json> ReadWindow(const Json &params, size_t default_limit, size_t max_limit, size_t &offset, size_t &limit)
{
	int64_t requested = static_cast<int64_t>(default_limit);
	switch (ReadInt(params, "limit", requested)) {
		case ParamState::Invalid: return MakeError("invalid_arguments", "`limit` must be an integer");
		case ParamState::Present:
			if (requested < 1 || requested > static_cast<int64_t>(max_limit)) {
				return MakeError("invalid_arguments", "`limit` is out of range", {{"min", 1}, {"max", max_limit}});
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
 * Is this a simple blueprint name, i.e. exactly one file name inside the blueprint directory?
 *
 * The name is used as the file name of the blueprint, so a name that the engine would have to change
 * is refused: that keeps the name and the file one to one, and it keeps the file inside the native
 * blueprint directory without any path of the caller.
 *
 * @param name The name.
 * @return True when the name is usable as a blueprint name.
 */
bool IsSimpleBlueprintName(std::string_view name)
{
	if (name.empty() || name.size() > MAX_NAME_BYTES) return false;
	if (name == "." || name == "..") return false;
	if (name.front() == ' ' || name.back() == ' ') return false;
	for (const char c : name) {
		if (c < 0x20 || c > 0x7E) return false;
		if (std::string_view("/\\:*?\"<>|").find(c) != std::string_view::npos) return false;
	}
	return true;
}

/**
 * Read a required blueprint name parameter.
 * @param params The method parameters.
 * @param name Receives the name.
 * @return An error document, or nothing when the name is usable.
 */
std::optional<Json> ReadBlueprintName(const Json &params, std::string &name)
{
	switch (ReadString(params, "name", name)) {
		case ParamState::Invalid: return MakeError("invalid_arguments", "`name` must be a string");
		case ParamState::Absent: return MakeError("invalid_arguments", "`name` is required");
		case ParamState::Present: break;
	}
	if (!IsSimpleBlueprintName(name)) {
		return MakeError("invalid_arguments", "`name` must be a simple file name inside the blueprint directory",
				{{"name", name}, {"max_bytes", MAX_NAME_BYTES}});
	}
	return std::nullopt;
}

/**
 * Read an optional four character rail type label.
 * @param params The method parameters.
 * @param key The parameter name.
 * @param railtype Receives the rail type, or INVALID_RAILTYPE when the parameter is absent.
 * @return An error document, or nothing when the label is usable.
 */
std::optional<Json> ReadRailTypeLabelParam(const Json &params, std::string_view key, RailType &railtype)
{
	std::string text;
	switch (ReadString(params, key, text)) {
		case ParamState::Invalid: return MakeError("invalid_arguments", "`" + std::string(key) + "` must be a four character rail type label");
		case ParamState::Absent: return std::nullopt;
		case ParamState::Present: break;
	}
	if (text.size() != 4) {
		return MakeError("invalid_arguments", "`" + std::string(key) + "` must be a four character rail type label", {{"value", text}});
	}
	RailTypeLabel label{};
	std::copy(text.begin(), text.end(), label.begin());
	railtype = GetRailTypeByLabel(label);
	if (railtype == INVALID_RAILTYPE) {
		return MakeError("unknown_railtype", "the game does not know that rail type", {{"railtype", text}});
	}
	return std::nullopt;
}

/**
 * Read the task, world and epoch binding of a call.
 *
 * The binding is what makes a plan belong to one task on one world and one epoch: the bridge passes the
 * `task_id`, `world_id` and `epoch` of the request it is executing, and this module refuses a plan of
 * another binding. `epoch` is the bridge's UUID string of the current load of the world, not a number.
 * A task is required, so two chats can never share a plan by accident.
 *
 * @param params The method parameters, with the binding of the request injected by the bridge.
 * @param task_id Receives the task id of the call.
 * @param world_id Receives the world id of the call.
 * @param epoch Receives the world epoch of the call.
 * @return An error document, or nothing when the binding is usable.
 */
std::optional<Json> ReadBinding(const Json &params, std::string &task_id, std::string &world_id, std::string &epoch)
{
	switch (ReadString(params, "task_id", task_id)) {
		case ParamState::Invalid: return MakeError("invalid_arguments", "`task_id` must be a string");
		case ParamState::Absent: return MakeError("invalid_arguments", "`task_id` is required: a plan belongs to one task");
		case ParamState::Present: break;
	}
	if (task_id.empty() || task_id.size() > MAX_BINDING_BYTES) {
		return MakeError("invalid_arguments", "`task_id` is out of range", {{"min_bytes", 1}, {"max_bytes", MAX_BINDING_BYTES}});
	}

	switch (ReadString(params, "world_id", world_id)) {
		case ParamState::Invalid: return MakeError("invalid_arguments", "`world_id` must be a string");
		case ParamState::Absent: return MakeError("invalid_arguments", "`world_id` is required");
		case ParamState::Present: break;
	}
	if (world_id.empty() || world_id.size() > MAX_BINDING_BYTES) {
		return MakeError("invalid_arguments", "`world_id` is out of range", {{"min_bytes", 1}, {"max_bytes", MAX_BINDING_BYTES}});
	}

	switch (ReadString(params, "epoch", epoch)) {
		case ParamState::Invalid: return MakeError("invalid_arguments", "`epoch` must be a string");
		case ParamState::Absent: return MakeError("invalid_arguments", "`epoch` is required");
		case ParamState::Present: break;
	}
	if (epoch.empty() || epoch.size() > MAX_BINDING_BYTES) {
		return MakeError("invalid_arguments", "`epoch` is out of range", {{"min_bytes", 1}, {"max_bytes", MAX_BINDING_BYTES}});
	}
	return std::nullopt;
}

/**
 * Read the company of a call, defaulting to the company the call was made for.
 * @param params The method parameters.
 * @param default_company The company of the call.
 * @param company Receives the company.
 * @return An error document, or nothing when the company exists.
 */
std::optional<Json> ReadCompany(const Json &params, CompanyID default_company, CompanyID &company)
{
	company = default_company;
	int64_t id = 0;
	switch (ReadInt(params, "company_id", id)) {
		case ParamState::Invalid: return MakeError("invalid_arguments", "`company_id` must be an integer");
		case ParamState::Absent: break;
		case ParamState::Present:
			if (id < 0 || id > UINT8_MAX) return MakeError("invalid_arguments", "`company_id` is out of range");
			company = static_cast<CompanyID>(id);
			break;
	}
	if (!Company::IsValidID(company)) return MakeError("unknown_company", "that company does not exist", {{"company_id", company.base()}});
	return std::nullopt;
}

/**
 * The lifetime generation of a company.
 *
 * The generation is the engine's own identity of one lifetime of a pooled object: it is assigned when
 * the company is created and never repeats within the process, so a company that was deleted and whose
 * id was reused by a new company has another generation. It is runtime only, like the world epoch.
 *
 * @param company The company.
 * @return The generation of the company at that index, or 0 when no company exists there.
 */
uint64_t CompanyLifetime(CompanyID company)
{
	if (!Company::IsValidID(company)) return 0;
	return Company::Get(company)->generation;
}

/**
 * Check that a call does not name another company than the one the request was made for.
 *
 * The bridge pins the actor of a request and passes it here; a `company_id` in the parameters is only
 * a copy of it, never an override, so a plan is always made for the company the request was made for.
 *
 * @param params The method parameters.
 * @param actor The company of the request, or CompanyID::Invalid() when the request names none.
 * @return An error document, or nothing when the parameters agree with the request.
 */
std::optional<Json> CheckRequestCompany(const Json &params, CompanyID actor)
{
	int64_t id = 0;
	switch (ReadInt(params, "company_id", id)) {
		case ParamState::Invalid: return MakeError("invalid_arguments", "`company_id` must be an integer");
		case ParamState::Absent: return std::nullopt;
		case ParamState::Present: break;
	}
	if (id < 0 || id > UINT8_MAX) return MakeError("invalid_arguments", "`company_id` is out of range");
	if (actor != CompanyID::Invalid() && static_cast<CompanyID>(id) != actor) {
		return MakeError("invalid_arguments", "`company_id` must be the company of the request",
				{{"actor_id", actor.base()}, {"company_id", id}});
	}
	return std::nullopt;
}

/**
 * Document of one tile.
 * @param tile The tile.
 * @return The `{x, y}` document.
 */
Json TileDocument(TileIndex tile)
{
	return Json{{"x", TileX(tile)}, {"y", TileY(tile)}};
}

/**
 * Explain why the plan store refused a plan.
 * @return The error document: the ids are used up, or the store is full.
 */
Json PlanStoreRefusal()
{
	if (BlueprintToolPlanIdCounter() > UINT32_MAX) {
		return MakeError("plan_id_exhausted", "the 32 bit plan id space is used up; plan ids are never reused");
	}
	return MakeError("plan_store_full", "the plan store is full; stop or drop a plan first");
}

/**
 * Document of the last error of a plan.
 * @param error The error string, or INVALID_STRING_ID.
 * @return The error text, or null.
 */
Json ErrorDocument(StringID error)
{
	if (error == INVALID_STRING_ID || error == STR_NULL) return Json(nullptr);
	return Json(GetString(error));
}

/** A chunk command whose result has not come back yet. */
struct InFlightChunk {
	uint32_t plan_id; ///< Plan the chunk belongs to.
	size_t chunk_index; ///< Index of the chunk in the plan.
};

/** Plans of this module, oldest first. */
std::vector<BlueprintToolPlan> _plans;
/** Id the store hands out next; engine-owned, starts at 1 and is never turned back. */
uint64_t _next_plan_id = 1;
/** Number of chunk bytes the store holds. */
size_t _stored_bytes = 0;
/** Chunk commands waiting for their result. */
std::deque<InFlightChunk> _in_flight;
/** The callback of the last posted chunk ran. */
bool _callback_seen = false;

/**
 * Number of chunk bytes of a plan.
 * @param plan The plan.
 * @return The number of bytes.
 */
size_t PlanBytes(const BlueprintToolPlan &plan)
{
	size_t bytes = 0;
	for (const BlueprintChunkData &chunk : plan.chunks) bytes += chunk.data.size();
	return bytes;
}

/**
 * Find a stored plan.
 * @param plan_id The plan id.
 * @return The plan, or nullptr when there is none.
 */
BlueprintToolPlan *FindPlan(uint32_t plan_id)
{
	for (BlueprintToolPlan &plan : _plans) {
		if (plan.plan_id == plan_id) return &plan;
	}
	return nullptr;
}

/**
 * Is a plan finished, i.e. will it never post another chunk by itself?
 * @param plan The plan.
 * @return True when the plan has no more chunks to execute.
 */
bool IsPlanFinished(const BlueprintToolPlan &plan)
{
	return plan.stopped || (plan.next_chunk >= plan.chunks.size() && !plan.pending);
}

/**
 * Document of the binding of a plan.
 * @param plan The plan.
 * @return The binding document.
 */
Json BindingDocument(const BlueprintToolPlan &plan)
{
	return Json{
			{"task_id", plan.task_id},
			{"world_id", plan.world_id},
			{"epoch", plan.epoch},
			{"company_id", plan.company.base()},
			{"company_generation", plan.company_generation},
	};
}

/**
 * Document of a plan and its progress.
 * @param plan The plan.
 * @return The plan document.
 */
Json PlanDocument(const BlueprintToolPlan &plan)
{
	Json chunk_sizes = Json::array();
	for (size_t i = 0; i < plan.chunks.size() && i < MAX_CHUNK_SIZES; i++) chunk_sizes.push_back(plan.chunks[i].data.size());

	Json result = Json::object();
	result["ok"] = true;
	result["plan_id"] = plan.plan_id;
	result["kind"] = plan.route ? "rail_route" : "blueprint";
	result["name"] = plan.name;
	result["origin"] = TileDocument(plan.origin);
	result["company_id"] = plan.company.base();
	result["company_generation"] = plan.company_generation;
	/* Generation 0 is "there was no company at that index", so it is never a current company. */
	result["company_current"] = plan.company_generation != 0 && CompanyLifetime(plan.company) == plan.company_generation;
	result["binding"] = BindingDocument(plan);
	result["chunks"] = Json{
			{"total", plan.chunks.size()},
			{"prepare", plan.prepare_count},
			{"build", plan.chunks.size() - plan.prepare_count},
	};
	result["chunk_sizes"] = std::move(chunk_sizes);
	result["chunk_sizes_truncated"] = plan.chunks.size() > MAX_CHUNK_SIZES;
	result["plan_bytes"] = PlanBytes(plan);
	result["next_chunk_index"] = plan.next_chunk;
	result["chunks_done"] = plan.next_chunk;
	result["done"] = IsPlanFinished(plan);
	result["stopped"] = plan.stopped;
	result["pending"] = plan.pending;
	result["cost"] = static_cast<int64_t>(plan.cost);
	result["built"] = plan.built;
	result["failed"] = plan.failed;
	result["partial"] = plan.failed != 0;
	result["out_of_money"] = plan.out_of_money;
	result["last_error"] = ErrorDocument(plan.last_error);
	return result;
}

/**
 * Account the result of one chunk command.
 * @param plan The plan.
 * @param result The result the engine reported.
 * @param extra_money Money that was missing to build the next item.
 * @param built Items built.
 * @param failed Items that failed.
 * @param last_error Error of the last failed item.
 */
void AccountChunk(BlueprintToolPlan &plan, const CommandCost &result, Money extra_money, uint32_t built, uint32_t failed, StringID last_error)
{
	plan.built += built;
	plan.failed += failed;
	if (result.Succeeded()) plan.cost += result.GetCost();
	if (last_error != INVALID_STRING_ID) plan.last_error = last_error;
	if (result.Failed() && result.GetErrorMessage() != INVALID_STRING_ID) plan.last_error = result.GetErrorMessage();

	/* A chunk that stopped for money did not build its remaining items, so it is not advanced. */
	if (extra_money != 0) {
		plan.out_of_money = true;
		return;
	}

	/* A command that reports nothing at all did not do anything: do not count it as done. */
	if (built == 0 && failed == 0 && result.Failed()) return;
	plan.next_chunk++;
}

/* --- Blueprint documents --- */

/**
 * Document of one blueprint, without its items.
 * @param bp The blueprint.
 * @return The document.
 */
Json BlueprintDocument(const Blueprint &bp)
{
	Json railtypes = Json::array();
	for (const RailTypeLabel &label : bp.railtypes) railtypes.push_back(label.AsString());

	Json specs = Json::array();
	for (const BlueprintStationSpec &spec : bp.specs) {
		specs.push_back(Json{{"grfid", spec.grfid.AsString()}, {"local_id", spec.local_id}});
	}

	size_t waypoints = 0;
	for (const BlueprintStationTile &st : bp.stations) {
		if (st.waypoint) waypoints++;
	}

	Json result = Json::object();
	result["name"] = bp.name;
	result["width"] = bp.width;
	result["height"] = bp.height;
	result["empty"] = bp.IsEmpty();
	/* The items themselves are reported by #BlueprintItems as a bounded list; their number is `items_total`. */
	result["counts"] = Json{
			{"tracks", bp.tracks.size()},
			{"signals", bp.signals.size()},
			{"depots", bp.depots.size()},
			{"tunnelbridges", bp.tunnelbridges.size()},
			{"stations", bp.stations.size() - waypoints},
			{"waypoints", waypoints},
			{"railtypes", bp.railtypes.size()},
			{"station_specs", bp.specs.size()},
	};
	result["railtypes"] = std::move(railtypes);
	result["station_specs"] = std::move(specs);
	return result;
}

/**
 * Document of a bounded part of the items of a blueprint.
 *
 * The items are reported kind by kind in a fixed order, so the listing never has to hold the whole
 * blueprint in memory and one call can never return an unbounded answer.
 *
 * @param bp The blueprint.
 * @param offset Number of items to skip.
 * @param limit Maximum number of items to report.
 * @param total Receives the number of items of the blueprint.
 * @return The items document.
 */
Json BlueprintItems(const Blueprint &bp, size_t offset, size_t limit, size_t &total)
{
	total = bp.CountItems();
	Json items = Json::array();
	size_t index = 0;
	auto emit = [&](Json item) {
		if (index >= offset && items.size() < limit) items.push_back(std::move(item));
		index++;
	};

	for (const BlueprintTrack &t : bp.tracks) {
		emit(Json{{"kind", "track"}, {"x", t.x}, {"y", t.y}, {"railtype", t.railtype < bp.railtypes.size() ? bp.railtypes[t.railtype].AsString() : std::string{}},
				{"tracks", fmt::format("{:02x}", t.tracks.base())}});
	}
	for (const BlueprintSignal &s : bp.signals) {
		emit(Json{{"kind", "signal"}, {"x", s.x}, {"y", s.y}, {"track", to_underlying(s.track)},
				{"signal_type", to_underlying(s.type)}, {"variant", to_underlying(s.variant)}, {"directions", s.trackdirs}});
	}
	for (const BlueprintDepot &d : bp.depots) {
		emit(Json{{"kind", "depot"}, {"x", d.x}, {"y", d.y}, {"railtype", d.railtype < bp.railtypes.size() ? bp.railtypes[d.railtype].AsString() : std::string{}},
				{"direction", to_underlying(d.dir)}});
	}
	for (const BlueprintTunnelBridge &tb : bp.tunnelbridges) {
		emit(Json{{"kind", tb.bridge ? "bridge" : "tunnel"}, {"x", tb.x}, {"y", tb.y}, {"end_x", tb.end_x}, {"end_y", tb.end_y},
				{"railtype", tb.railtype < bp.railtypes.size() ? bp.railtypes[tb.railtype].AsString() : std::string{}},
				{"bridge_type", tb.bridge ? Json(tb.bridge_type) : Json(nullptr)}});
	}
	for (const BlueprintStationTile &st : bp.stations) {
		emit(Json{{"kind", st.waypoint ? "waypoint" : "station"}, {"x", st.x}, {"y", st.y}, {"group", st.group},
				{"axis", to_underlying(st.axis)}, {"railtype", st.railtype < bp.railtypes.size() ? bp.railtypes[st.railtype].AsString() : std::string{}},
				{"spec", st.spec == UINT16_MAX ? Json(nullptr) : Json(st.spec)}});
	}

	Json result = Json::object();
	result["items"] = std::move(items);
	result["items_total"] = total;
	result["offset"] = offset;
	result["limit"] = limit;
	return result;
}

/**
 * Number of bytes of the file of a blueprint, when it is a blueprint this module may load.
 * @param name The blueprint name.
 * @param bytes Receives the file size.
 * @return An error document, or nothing when the file may be loaded.
 */
std::optional<Json> CheckBlueprintFile(std::string_view name, size_t &bytes)
{
	std::error_code error_code;
	const auto size = std::filesystem::file_size(OTTD2FS(GetBlueprintFileName(name)), error_code);
	if (error_code) return MakeError("unknown_blueprint", "there is no blueprint with that name", {{"name", std::string(name)}});
	if (size > MAX_BLUEPRINT_FILE_BYTES) {
		return MakeError("blueprint_too_large", "the blueprint file is too large to load", {{"bytes", size}, {"max_bytes", MAX_BLUEPRINT_FILE_BYTES}});
	}
	bytes = static_cast<size_t>(size);
	return std::nullopt;
}

/**
 * Load a blueprint, bounded by the size of its file.
 * @param name The blueprint name.
 * @param bp Receives the blueprint.
 * @return An error document, or nothing when the blueprint was loaded.
 */
std::optional<Json> LoadBlueprintBounded(std::string_view name, Blueprint &bp)
{
	size_t bytes = 0;
	if (const auto error = CheckBlueprintFile(name, bytes)) return error;
	std::optional<Blueprint> loaded = LoadBlueprint(name);
	if (!loaded.has_value()) return MakeError("broken_blueprint", "the blueprint file cannot be read", {{"name", std::string(name)}});
	bp = std::move(*loaded);
	return std::nullopt;
}

/**
 * Look up a stored plan of a call and check its binding.
 * @param params The method parameters.
 * @param plan Receives the plan.
 * @param task_id Receives the task id of the call.
 * @param world_id Receives the world id of the call.
 * @param epoch Receives the world epoch of the call.
 * @return An error document, or nothing when the plan was found and belongs to the call.
 */
std::optional<Json> FindBoundPlan(const Json &params, BlueprintToolPlan *&plan, std::string &task_id, std::string &world_id,
		std::string &epoch)
{
	int64_t id = 0;
	switch (ReadInt(params, "plan_id", id)) {
		case ParamState::Invalid: return MakeError("invalid_arguments", "`plan_id` must be an integer");
		case ParamState::Absent: return MakeError("invalid_arguments", "`plan_id` is required");
		case ParamState::Present: break;
	}
	if (id <= 0 || id > UINT32_MAX) return MakeError("invalid_arguments", "`plan_id` is out of range");

	if (const auto error = ReadBinding(params, task_id, world_id, epoch)) return error;

	plan = FindPlan(static_cast<uint32_t>(id));
	if (plan == nullptr) return MakeError("unknown_plan", "there is no plan with that id", {{"plan_id", id}});

	if (plan->task_id != task_id || plan->world_id != world_id || plan->epoch != epoch) {
		return MakeError("plan_binding_mismatch", "the plan belongs to another task, world or epoch", BindingDocument(*plan));
	}
	return std::nullopt;
}

/**
 * Check that a plan may still be executed for the company of the call.
 *
 * A plan belongs to the company it was made for and to that one lifetime of the company: another
 * company of the same task cannot build it, and neither can the id of a company that was deleted and
 * reused by a new company. The check reads the engine's own company pool, so it is the real lifetime
 * and not a copy of the id.
 *
 * @param plan The plan.
 * @param actor The company of the call, or CompanyID::Invalid() when the call names none.
 * @return An error document, or nothing when the plan may be executed.
 */
std::optional<Json> CheckPlanCompany(const BlueprintToolPlan &plan, CompanyID actor)
{
	if (actor != CompanyID::Invalid() && actor != plan.company) {
		return MakeError("plan_actor_mismatch", "the plan was made for another company than the request actor",
				{{"plan_id", plan.plan_id}, {"company_id", plan.company.base()}, {"actor_id", actor.base()}});
	}

	const uint64_t generation = CompanyLifetime(plan.company);
	/* A plan stored for a company that did not exist has generation 0 and is never executable. */
	if (plan.company_generation == 0 || generation != plan.company_generation) {
		return MakeError("company_lifetime_mismatch", "the company of the plan was deleted or its id was reused",
				{{"plan_id", plan.plan_id}, {"company_id", plan.company.base()},
				 {"company_generation", plan.company_generation}, {"generation_now", generation}});
	}
	return std::nullopt;
}

/* --- Operations of world.blueprint --- */

/**
 * List the blueprints in the native blueprint directory.
 * @param params The method parameters.
 * @return The result document.
 */
Json BlueprintListOp(const Json &params)
{
	size_t offset = 0;
	size_t limit = 0;
	if (const auto error = ReadWindow(params, DEFAULT_LIST_LIMIT, MAX_LIST_LIMIT, offset, limit)) return *error;

	const std::vector<std::string> names = ListBlueprints();
	Json blueprints = Json::array();
	size_t total = 0;
	for (const std::string &name : names) {
		if (total >= offset && blueprints.size() < limit) {
			std::error_code error_code;
			const auto bytes = std::filesystem::file_size(OTTD2FS(GetBlueprintFileName(name)), error_code);
			blueprints.push_back(Json{{"name", name}, {"bytes", error_code ? 0 : bytes}});
		}
		total++;
	}

	Json result = Json::object();
	result["ok"] = true;
	result["blueprints"] = std::move(blueprints);
	result["total"] = total;
	result["offset"] = offset;
	result["limit"] = limit;
	return result;
}

/**
 * Inspect a blueprint file.
 * @param params The method parameters.
 * @return The result document.
 */
Json BlueprintInspectOp(const Json &params)
{
	std::string name;
	if (const auto error = ReadBlueprintName(params, name)) return *error;
	size_t offset = 0;
	size_t limit = 0;
	if (const auto error = ReadWindow(params, DEFAULT_ITEM_LIMIT, MAX_ITEM_LIMIT, offset, limit)) return *error;

	Blueprint bp;
	if (const auto error = LoadBlueprintBounded(name, bp)) return *error;

	Json result = BlueprintDocument(bp);
	result["ok"] = true;
	size_t total = 0;
	Json items = BlueprintItems(bp, offset, limit, total);
	for (auto &[key, value] : items.items()) result[key] = std::move(value);
	return result;
}

/**
 * Capture the rail infrastructure of an area.
 * @param params The method parameters.
 * @param company The company of the call.
 * @return The result document.
 */
Json BlueprintCaptureOp(const Json &params, CompanyID company)
{
	TileIndex corner1 = INVALID_TILE;
	TileIndex corner2 = INVALID_TILE;
	switch (ReadTile(params, "corner1", corner1)) {
		case ParamState::Invalid: return MakeError("invalid_arguments", "`corner1` must be an object with `x` and `y` inside the map");
		case ParamState::Absent: return MakeError("invalid_arguments", "`corner1` is required");
		case ParamState::Present: break;
	}
	switch (ReadTile(params, "corner2", corner2)) {
		case ParamState::Invalid: return MakeError("invalid_arguments", "`corner2` must be an object with `x` and `y` inside the map");
		case ParamState::Absent: return MakeError("invalid_arguments", "`corner2` is required");
		case ParamState::Present: break;
	}

	const uint x0 = std::min(TileX(corner1), TileX(corner2));
	const uint x1 = std::max(TileX(corner1), TileX(corner2));
	const uint y0 = std::min(TileY(corner1), TileY(corner2));
	const uint y1 = std::max(TileY(corner1), TileY(corner2));
	const size_t tiles = static_cast<size_t>(x1 - x0 + 1) * static_cast<size_t>(y1 - y0 + 1);
	if (tiles > MAX_CAPTURE_TILES) {
		return MakeError("area_too_large", "the area is larger than one capture may cover",
				{{"tiles", tiles}, {"max_tiles", MAX_CAPTURE_TILES}});
	}

	std::string name;
	switch (ReadString(params, "name", name)) {
		case ParamState::Invalid: return MakeError("invalid_arguments", "`name` must be a string");
		case ParamState::Absent: break;
		case ParamState::Present:
			if (!IsSimpleBlueprintName(name)) {
				return MakeError("invalid_arguments", "`name` must be a simple file name inside the blueprint directory",
						{{"name", name}, {"max_bytes", MAX_NAME_BYTES}});
			}
			break;
	}
	bool save = false;
	switch (ReadBool(params, "save", save)) {
		case ParamState::Invalid: return MakeError("invalid_arguments", "`save` must be a boolean");
		case ParamState::Present: break;
		case ParamState::Absent: break;
	}
	if (save && name.empty()) return MakeError("invalid_arguments", "`name` is required when `save` is true");

	size_t offset = 0;
	size_t limit = 0;
	if (const auto error = ReadWindow(params, DEFAULT_ITEM_LIMIT, MAX_ITEM_LIMIT, offset, limit)) return *error;

	const auto start = std::chrono::steady_clock::now();
	BlueprintCaptureStats stats;
	Blueprint bp = CaptureBlueprint(corner1, corner2, name, &stats);
	const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();

	bool saved = false;
	if (save) {
		if (!SaveBlueprint(bp)) return MakeError("save_failed", "the blueprint file could not be written", {{"name", name}});
		saved = true;
		InvalidateWindowClassesData(WindowClass::Blueprints, 1);
	}

	Json result = BlueprintDocument(bp);
	result["ok"] = true;
	result["company_id"] = company.base();
	result["saved"] = saved;
	result["partial_tunnelbridges"] = stats.partial_tunnelbridges;
	result["capture_ms"] = ms;
	size_t total = 0;
	Json items = BlueprintItems(bp, offset, limit, total);
	for (auto &[key, value] : items.items()) result[key] = std::move(value);
	return result;
}

/**
 * Plan a blueprint paste.
 * @param params The method parameters.
 * @param company The company of the call.
 * @return The result document.
 */
Json BlueprintPlanOp(const Json &params, CompanyID company)
{
	std::string name;
	if (const auto error = ReadBlueprintName(params, name)) return *error;

	/* The cheap arguments are checked before the tile, so a malformed call never reaches the map. */
	BlueprintPasteOptions options;
	int64_t rotation = 0;
	switch (ReadInt(params, "rotation", rotation)) {
		case ParamState::Invalid: return MakeError("invalid_arguments", "`rotation` must be an integer");
		case ParamState::Present:
			if (rotation < 0 || rotation > 3) return MakeError("invalid_arguments", "`rotation` is out of range", {{"min", 0}, {"max", 3}});
			options.transform.rotation = static_cast<uint8_t>(rotation);
			break;
		case ParamState::Absent: break;
	}
	bool mirror = false;
	switch (ReadBool(params, "mirror", mirror)) {
		case ParamState::Invalid: return MakeError("invalid_arguments", "`mirror` must be a boolean");
		case ParamState::Present: options.transform.mirror = mirror; break;
		case ParamState::Absent: break;
	}
	if (const auto error = ReadRailTypeLabelParam(params, "railtype", options.railtype)) return *error;

	std::string task_id;
	std::string world_id;
	std::string epoch;
	if (const auto error = ReadBinding(params, task_id, world_id, epoch)) return *error;

	TileIndex origin = INVALID_TILE;
	switch (ReadTile(params, "origin", origin)) {
		case ParamState::Invalid: return MakeError("invalid_arguments", "`origin` must be an object with `x` and `y` inside the map");
		case ParamState::Absent: return MakeError("invalid_arguments", "`origin` is required");
		case ParamState::Present: break;
	}

	/* The company is checked after the geometry: a plan is only posted later, and the tiles decide whether it can exist at all. */
	CompanyID actor = company;
	if (const auto error = ReadCompany(params, company, actor)) return *error;

	Blueprint bp;
	if (const auto error = LoadBlueprintBounded(name, bp)) return *error;

	const BlueprintPastePlan paste = PlanBlueprintPaste(bp, origin, options);
	if (paste.chunks.empty()) {
		return MakeError("plan_failed", paste.error.empty() ? "the blueprint cannot be pasted here" : paste.error,
				{{"name", name}, {"origin", TileDocument(origin)}});
	}
	size_t paste_bytes = 0;
	for (const BlueprintChunkData &chunk : paste.chunks) paste_bytes += chunk.data.size();
	if (paste.chunks.size() > MAX_PLAN_CHUNKS || paste_bytes > MAX_PLAN_BYTES) {
		return MakeError("plan_too_large", "the paste needs more chunks than one plan may hold",
				{{"chunks", paste.chunks.size()}, {"max_chunks", MAX_PLAN_CHUNKS}, {"bytes", paste_bytes}, {"max_bytes", MAX_PLAN_BYTES}});
	}

	BlueprintToolPlan plan;
	plan.route = false;
	plan.name = name;
	plan.origin = paste.origin;
	plan.company = actor;
	plan.task_id = std::move(task_id);
	plan.world_id = std::move(world_id);
	plan.epoch = std::move(epoch);
	plan.prepare_count = 0;
	plan.chunks = std::move(paste.chunks);

	const std::optional<uint32_t> plan_id = StoreBlueprintToolPlan(std::move(plan));
	if (!plan_id.has_value()) return PlanStoreRefusal();

	Json result = PlanDocument(*FindPlan(*plan_id));
	result["items"] = paste.items;
	return result;
}

/**
 * Plan a rail trunk line.
 * @param params The method parameters.
 * @param company The company of the call.
 * @return The result document.
 */
Json RailRoutePlanOp(const Json &params, CompanyID company)
{
	/* The cheap arguments are checked before the tiles, so a malformed call never reaches the map. */
	RailRouteOptions options;
	int64_t tracks = 0;
	switch (ReadInt(params, "tracks", tracks)) {
		case ParamState::Invalid: return MakeError("invalid_arguments", "`tracks` must be an integer");
		case ParamState::Absent: return MakeError("invalid_arguments", "`tracks` is required");
		case ParamState::Present:
			if (tracks < 1 || tracks > RailRouteOptions::MAX_TRACKS) {
				return MakeError("invalid_arguments", "`tracks` is out of range", {{"min", 1}, {"max", RailRouteOptions::MAX_TRACKS}});
			}
			options.tracks = static_cast<uint8_t>(tracks);
			break;
	}

	bool signals = options.signals;
	switch (ReadBool(params, "signals", signals)) {
		case ParamState::Invalid: return MakeError("invalid_arguments", "`signals` must be a boolean");
		case ParamState::Present: options.signals = signals; break;
		case ParamState::Absent: break;
	}
	int64_t interval = options.signal_interval;
	switch (ReadInt(params, "signal_interval", interval)) {
		case ParamState::Invalid: return MakeError("invalid_arguments", "`signal_interval` must be an integer");
		case ParamState::Present:
			if (interval < 1 || interval > UINT8_MAX) {
				return MakeError("invalid_arguments", "`signal_interval` is out of range", {{"min", 1}, {"max", UINT8_MAX}});
			}
			options.signal_interval = static_cast<uint8_t>(interval);
			break;
		case ParamState::Absent: break;
	}
	int64_t signal_type = to_underlying(options.signal_type);
	switch (ReadInt(params, "signal_type", signal_type)) {
		case ParamState::Invalid: return MakeError("invalid_arguments", "`signal_type` must be an integer");
		case ParamState::Present:
			if (signal_type < 0 || signal_type >= to_underlying(SignalType::End)) {
				return MakeError("invalid_arguments", "`signal_type` is out of range", {{"min", 0}, {"max", to_underlying(SignalType::End) - 1}});
			}
			options.signal_type = static_cast<SignalType>(signal_type);
			break;
		case ParamState::Absent: break;
	}
	bool semaphore = false;
	switch (ReadBool(params, "semaphore", semaphore)) {
		case ParamState::Invalid: return MakeError("invalid_arguments", "`semaphore` must be a boolean");
		case ParamState::Present: options.signal_variant = semaphore ? SignalVariant::Semaphore : SignalVariant::Electric; break;
		case ParamState::Absent: break;
	}
	bool terraform = options.terraform;
	switch (ReadBool(params, "terraform", terraform)) {
		case ParamState::Invalid: return MakeError("invalid_arguments", "`terraform` must be a boolean");
		case ParamState::Present: options.terraform = terraform; break;
		case ParamState::Absent: break;
	}
	bool water = options.remove_water;
	switch (ReadBool(params, "remove_water", water)) {
		case ParamState::Invalid: return MakeError("invalid_arguments", "`remove_water` must be a boolean");
		case ParamState::Present: options.remove_water = water; break;
		case ParamState::Absent: break;
	}
	bool demolish = options.demolish;
	switch (ReadBool(params, "demolish", demolish)) {
		case ParamState::Invalid: return MakeError("invalid_arguments", "`demolish` must be a boolean");
		case ParamState::Present: options.demolish = demolish; break;
		case ParamState::Absent: break;
	}
	bool sea_level = options.raise_sea_level;
	switch (ReadBool(params, "raise_sea_level", sea_level)) {
		case ParamState::Invalid: return MakeError("invalid_arguments", "`raise_sea_level` must be a boolean");
		case ParamState::Present: options.raise_sea_level = sea_level; break;
		case ParamState::Absent: break;
	}
	if (const auto error = ReadRailTypeLabelParam(params, "railtype", options.railtype)) return *error;

	std::string start;
	switch (ReadString(params, "start", start)) {
		case ParamState::Invalid: return MakeError("invalid_arguments", "`start` must be a string");
		case ParamState::Present:
			if (start == "auto") options.start = RailRouteStart::Auto;
			else if (start == "x") options.start = RailRouteStart::X;
			else if (start == "y") options.start = RailRouteStart::Y;
			else if (start == "ns") options.start = RailRouteStart::NS;
			else if (start == "ew") options.start = RailRouteStart::EW;
			else return MakeError("invalid_arguments", "`start` must be auto, x, y, ns or ew", {{"start", start}});
			break;
		case ParamState::Absent: break;
	}

	std::string task_id;
	std::string world_id;
	std::string epoch;
	if (const auto error = ReadBinding(params, task_id, world_id, epoch)) return *error;

	CompanyID actor = company;
	if (const auto error = ReadCompany(params, company, actor)) return *error;

	TileIndex from = INVALID_TILE;
	TileIndex to = INVALID_TILE;
	switch (ReadTile(params, "from", from)) {
		case ParamState::Invalid: return MakeError("invalid_arguments", "`from` must be an object with `x` and `y` inside the map");
		case ParamState::Absent: return MakeError("invalid_arguments", "`from` is required");
		case ParamState::Present: break;
	}
	switch (ReadTile(params, "to", to)) {
		case ParamState::Invalid: return MakeError("invalid_arguments", "`to` must be an object with `x` and `y` inside the map");
		case ParamState::Absent: return MakeError("invalid_arguments", "`to` is required");
		case ParamState::Present: break;
	}

	RailRoutePlan route;
	{
		const AutoRestoreBackup<CompanyID> context(_current_company, actor);
		route = PlanRailRoute(from, to, options);
	}
	if (!route.error.empty()) {
		return MakeError("plan_failed", route.error, {{"from", TileDocument(from)}, {"to", TileDocument(to)}});
	}

	BlueprintToolPlan plan;
	plan.route = true;
	plan.name = fmt::format("{},{} - {},{}", TileX(from), TileY(from), TileX(to), TileY(to));
	plan.origin = route.origin;
	plan.company = actor;
	plan.task_id = std::move(task_id);
	plan.world_id = std::move(world_id);
	plan.epoch = std::move(epoch);
	plan.prepare_count = route.prepare.size();
	plan.chunks = std::move(route.prepare);
	for (BlueprintChunkData &chunk : route.build) plan.chunks.push_back(std::move(chunk));
	if (plan.chunks.empty()) return MakeError("nothing_to_build", "the trunk line is already built", {{"from", TileDocument(from)}, {"to", TileDocument(to)}});
	if (plan.chunks.size() > MAX_PLAN_CHUNKS) {
		return MakeError("plan_too_large", "the trunk line needs more chunks than one plan may hold",
				{{"chunks", plan.chunks.size()}, {"max_chunks", MAX_PLAN_CHUNKS}});
	}

	const std::optional<uint32_t> plan_id = StoreBlueprintToolPlan(std::move(plan));
	if (!plan_id.has_value()) return PlanStoreRefusal();

	Json result = PlanDocument(*FindPlan(*plan_id));
	result["length"] = route.length;
	result["bends"] = route.bends;
	result["track_tiles"] = route.tracks.size();
	result["heights"] = Json{{"min", route.min_height}, {"max", route.max_height}};
	result["end"] = TileDocument(route.end);
	result["estimate"] = Json{
			{"clearing", static_cast<int64_t>(route.estimate.clearing)},
			{"terraform", static_cast<int64_t>(route.estimate.terraform)},
			{"track", static_cast<int64_t>(route.estimate.track)},
			{"total", static_cast<int64_t>(route.estimate.Total())},
			{"houses", route.estimate.houses},
			{"industries", route.estimate.industries},
			{"water", route.estimate.water},
			{"blocked", route.estimate.blocked},
	};
	return result;
}

/**
 * Execute the next chunk of a plan.
 * @param params The method parameters.
 * @param actor The company of the call.
 * @return The result document.
 */
Json PlanExecuteChunkOp(const Json &params, CompanyID actor)
{
	BlueprintToolPlan *plan = nullptr;
	std::string task_id;
	std::string world_id;
	std::string epoch;
	if (const auto error = FindBoundPlan(params, plan, task_id, world_id, epoch)) return *error;
	if (const auto error = CheckPlanCompany(*plan, actor)) return *error;

	int64_t index = 0;
	switch (ReadInt(params, "chunk_index", index)) {
		case ParamState::Invalid: return MakeError("invalid_arguments", "`chunk_index` must be an integer");
		case ParamState::Absent: return MakeError("invalid_arguments", "`chunk_index` is required");
		case ParamState::Present: break;
	}
	if (index < 0 || static_cast<size_t>(index) > plan->chunks.size()) {
		return MakeError("invalid_chunk_index", "the plan has no chunk with that index",
				{{"chunk_index", index}, {"chunks_total", plan->chunks.size()}});
	}
	const size_t chunk_index = static_cast<size_t>(index);

	if (plan->stopped) return MakeError("plan_stopped", "the plan was stopped; it will not execute another chunk", {{"plan_id", plan->plan_id}});
	if (plan->pending) {
		return MakeError("chunk_in_flight", "the previous chunk of this plan has no result yet",
				{{"plan_id", plan->plan_id}, {"next_chunk_index", plan->next_chunk}});
	}
	if (plan->next_chunk >= plan->chunks.size()) {
		return MakeError("plan_done", "the plan has no chunks left", {{"plan_id", plan->plan_id}, {"chunks_total", plan->chunks.size()}});
	}
	if (chunk_index != plan->next_chunk) {
		return MakeError("out_of_order", "chunks of a plan are executed in order",
				{{"plan_id", plan->plan_id}, {"chunk_index", chunk_index}, {"next_chunk_index", plan->next_chunk}});
	}
	if (_in_flight.size() >= MAX_IN_FLIGHT) return MakeError("too_many_in_flight", "too many chunks wait for their result");

	const bool prepare = chunk_index < plan->prepare_count;
	const BlueprintChunkData &chunk = plan->chunks[chunk_index];
	const uint32_t plan_id = plan->plan_id;

	_in_flight.push_back({plan_id, chunk_index});
	plan->pending = true;
	_callback_seen = false;
	bool ok;
	{
		const AutoRestoreBackup<CompanyID> context(_current_company, plan->company);
		ok = prepare
				? Command<Commands::PrepareRailRoute>::Post(STR_NULL, &AgentBlueprintChunkCallback, plan->origin, chunk)
				: Command<Commands::PasteBlueprint>::Post(STR_NULL, &AgentBlueprintChunkCallback, plan->origin, chunk);
	}

	/* The engine refused the command before it ran, so no result will come. */
	if (!ok && !_callback_seen) {
		plan->pending = false;
		if (!_in_flight.empty() && _in_flight.back().plan_id == plan_id) _in_flight.pop_back();
		if (_pause_mode.Any()) return MakeError("paused", "the game is paused; the chunk was not posted", {{"plan_id", plan_id}});
		return MakeError("command_refused", "the engine refused the chunk command", {{"plan_id", plan_id}, {"chunk_index", chunk_index}});
	}

	Json result = PlanDocument(*plan);
	result["chunk_index"] = chunk_index;
	result["chunk_kind"] = prepare ? "prepare" : "build";
	result["chunk_bytes"] = chunk.data.size();
	result["executed"] = plan->next_chunk > chunk_index;
	result["submitted"] = plan->pending;
	result["command_failed"] = !result["executed"].get<bool>() && !result["submitted"].get<bool>();
	if (result["command_failed"].get<bool>()) result["message"] = "the engine did not build anything for this chunk";
	return result;
}

/**
 * Report the progress of a plan.
 *
 * The status is bound to the task, world and epoch of the call like every other plan operation, so a
 * read can never disclose the plan of another chat or of a world that has been left.
 * @param params The method parameters.
 * @return The result document.
 */
Json PlanStatusOp(const Json &params)
{
	BlueprintToolPlan *plan = nullptr;
	std::string task_id;
	std::string world_id;
	std::string epoch;
	if (const auto error = FindBoundPlan(params, plan, task_id, world_id, epoch)) return *error;
	return PlanDocument(*plan);
}

/**
 * Stop a plan: it will never post another chunk, and its chunks are dropped.
 * @param params The method parameters.
 * @return The result document.
 */
Json PlanStopOp(const Json &params)
{
	BlueprintToolPlan *plan = nullptr;
	std::string task_id;
	std::string world_id;
	std::string epoch;
	if (const auto error = FindBoundPlan(params, plan, task_id, world_id, epoch)) return *error;
	if (plan->pending) {
		return MakeError("chunk_in_flight", "the previous chunk of this plan has no result yet",
				{{"plan_id", plan->plan_id}, {"next_chunk_index", plan->next_chunk}});
	}

	const size_t dropped = plan->chunks.size() - plan->next_chunk;
	_stored_bytes -= PlanBytes(*plan);
	plan->chunks.clear();
	plan->chunks.shrink_to_fit();
	plan->prepare_count = 0;
	plan->stopped = true;

	Json result = PlanDocument(*plan);
	result["dropped_chunks"] = dropped;
	return result;
}

/**
 * Drop a plan from the store.
 * @param params The method parameters.
 * @return The result document.
 */
Json PlanDropOp(const Json &params)
{
	BlueprintToolPlan *plan = nullptr;
	std::string task_id;
	std::string world_id;
	std::string epoch;
	if (const auto error = FindBoundPlan(params, plan, task_id, world_id, epoch)) return *error;
	if (plan->pending) {
		return MakeError("chunk_in_flight", "the previous chunk of this plan has no result yet",
				{{"plan_id", plan->plan_id}, {"next_chunk_index", plan->next_chunk}});
	}

	Json result = PlanDocument(*plan);
	result["dropped"] = true;
	const uint32_t plan_id = plan->plan_id;
	_stored_bytes -= PlanBytes(*plan);
	_plans.erase(std::remove_if(_plans.begin(), _plans.end(), [plan_id](const BlueprintToolPlan &p) { return p.plan_id == plan_id; }), _plans.end());
	return result;
}

/**
 * Read the operation of a call.
 * @param params The method parameters.
 * @param op Receives the operation name.
 * @return An error document, or nothing when the operation is usable.
 */
std::optional<Json> ReadOp(const Json &params, std::string &op)
{
	switch (ReadString(params, "op", op)) {
		case ParamState::Invalid: return MakeError("invalid_arguments", "`op` must be a string");
		case ParamState::Absent: return MakeError("invalid_arguments", "`op` is required");
		case ParamState::Present: break;
	}
	return std::nullopt;
}

/**
 * Dispatch one world.blueprint call.
 * @param params The method parameters.
 * @param company The company of the call.
 * @return The result document.
 */
Json DispatchBlueprint(const Json &params, CompanyID company)
{
	std::string op;
	if (const auto error = ReadOp(params, op)) return *error;

	if (op == "list") return BlueprintListOp(params);
	if (op == "inspect") return BlueprintInspectOp(params);
	if (op == "capture") return BlueprintCaptureOp(params, company);
	if (op == "plan") return BlueprintPlanOp(params, company);
	if (op == "status") return PlanStatusOp(params);
	if (op == "execute_chunk") return PlanExecuteChunkOp(params, company);
	if (op == "stop") return PlanStopOp(params);
	if (op == "drop") return PlanDropOp(params);

	return MakeError("unsupported_op", "the blueprint tool module does not implement this operation yet",
			{{"method", "world.blueprint"}, {"op", op}});
}

/**
 * Dispatch one world.rail_route call.
 * @param params The method parameters.
 * @param company The company of the call.
 * @return The result document.
 */
Json DispatchRailRoute(const Json &params, CompanyID company)
{
	std::string op;
	if (const auto error = ReadOp(params, op)) return *error;

	if (op == "plan") return RailRoutePlanOp(params, company);
	if (op == "status") return PlanStatusOp(params);
	if (op == "execute_chunk") return PlanExecuteChunkOp(params, company);
	if (op == "stop") return PlanStopOp(params);
	if (op == "drop") return PlanDropOp(params);

	return MakeError("unsupported_op", "the rail route tool module does not implement this operation yet",
			{{"method", "world.rail_route"}, {"op", op}});
}

/**
 * Add one method to a capability document.
 * @param methods The method list to add to.
 * @param method The method name.
 * @param authority "read" or "write", as the other tool modules publish it.
 * @param implemented The implemented operations.
 * @param gaps The known gaps.
 */
void AddCapability(Json &methods, std::string_view method, std::string_view authority,
		std::initializer_list<const char *> implemented, std::initializer_list<const char *> gaps)
{
	Json ops = Json::array();
	for (const char *op : implemented) ops.push_back(op);
	Json missing = Json::array();
	for (const char *op : gaps) missing.push_back(op);
	/* `authority` is the field the other modules publish; `kind` is kept as the same value for readers
	 * of the older spelling. */
	methods.push_back(Json{{"method", std::string(method)}, {"authority", std::string(authority)}, {"kind", std::string(authority)}, {"implemented", std::move(ops)}, {"gaps", std::move(missing)}});
}

} // anonymous namespace

Json BlueprintToolCapabilities()
{
	Json methods = Json::array();
	AddCapability(methods, "world.blueprint", "write",
			{"list", "inspect", "capture", "plan", "status", "execute_chunk", "stop", "drop"},
			{"rename", "delete", "compare", "window"});
	AddCapability(methods, "world.rail_route", "write",
			{"plan", "status", "execute_chunk", "stop", "drop"},
			{"preview_tiles", "start_pieces", "window"});

	Json query_ops = Json::array();
	query_ops.push_back("world.blueprint op=list");
	query_ops.push_back("world.blueprint op=inspect");
	query_ops.push_back("world.blueprint op=status");
	query_ops.push_back("world.rail_route op=status");

	Json bounds = Json::object();
	bounds["blueprint_file_bytes"] = MAX_BLUEPRINT_FILE_BYTES;
	bounds["capture_tiles"] = MAX_CAPTURE_TILES;
	bounds["plans"] = MAX_PLANS;
	bounds["plan_chunks"] = MAX_PLAN_CHUNKS;
	bounds["plan_bytes"] = MAX_PLAN_BYTES;
	bounds["plan_store_bytes"] = MAX_PLAN_STORE_BYTES;
	bounds["chunks_in_flight"] = MAX_IN_FLIGHT;
	bounds["list_limit"] = MAX_LIST_LIMIT;
	bounds["item_limit"] = MAX_ITEM_LIMIT;
	bounds["name_bytes"] = MAX_NAME_BYTES;

	Json result = Json::object();
	result["ok"] = true;
	result["module"] = "agent_blueprint_tools";
	result["methods"] = std::move(methods);
	result["query_ops"] = std::move(query_ops);
	result["bounds"] = std::move(bounds);
	result["notes"] = Json::array({
			"Blueprints are the engine's own files in the native blueprint directory; names are simple file names, so a call can never read or write another path.",
			"A plan is a bounded record of one blueprint paste or one trunk line: the module hands out the plan id from a 32 bit counter that never hands out the same id twice and refuses new plans when the id space is used up, keeps at most a fixed number of plans, chunks and bytes, and refuses a plan that does not fit instead of truncating it.",
			"Every chunk is one normal Commands::PasteBlueprint or Commands::PrepareRailRoute command posted for the plan's company, so the engine's own eligibility, pause and finance checks apply and the reported cost is the engine's cost.",
			"One chunk per call: execute_chunk executes exactly the next chunk of the plan and reports the engine's result for it; nothing runs in the background, so nothing keeps building after the bridge stops calling and the player's own paste job is not touched.",
			"A plan is bound to the request binding the bridge uses: task_id, world_id and epoch (the bridge's UUID string of the current load of the world), plus the company the chunks are posted for and the lifetime generation of that company taken from the engine's own company pool when the plan was made. plan, status, execute_chunk, stop and drop all require that binding and refuse a plan of another task, world or epoch, so a read can never disclose the plan of another chat; the bridge checks the world, the epoch and the task barrier before every call and injects the three fields from the request, never from the model's parameters.",
			"capture, plan, execute_chunk, stop and drop are reported as mutations because they change the plan store or the world; list, inspect and status are the read-only operations.",
			"Chunk indices are executed in order; a chunk that the engine did not build (built and failed both zero) is not counted as done, so submitted work is never reported as done.",
			"execute_chunk posts the chunks for the company the plan was made for and refuses to execute a plan of a company that was deleted or whose id was reused (company_lifetime_mismatch), or for a request whose actor is another company (plan_actor_mismatch); a company_id in the parameters must be the company of the request and never overrides it.",
			"Performance: capture is one pass over at most 8192 tiles, planning a blueprint is linear in its items plus an ordered split of its station tiles, and planning a trunk line runs the engine's route search whose cost grows with the route length times the number of tracks; no call waits for the network or the model, and no call scans the whole map.",
	});
	return result;
}

std::optional<Json> DispatchBlueprintTool(std::string_view method, const Json &params, CompanyID company)
{
	if (method != "world.blueprint" && method != "world.rail_route") return std::nullopt;

	/* The actor of the request is pinned by the bridge; the parameters may name it but not override it. */
	if (const auto error = CheckRequestCompany(params, company)) return *error;

	if (method == "world.blueprint") return DispatchBlueprint(params, company);
	return DispatchRailRoute(params, company);
}

bool IsBlueprintMutation(std::string_view method, const Json &params)
{
	if (method != "world.blueprint" && method != "world.rail_route") return false;

	std::string op;
	if (ReadString(params, "op", op) != ParamState::Present) return true;

	if (method == "world.blueprint") {
		if (op == "list" || op == "inspect" || op == "status") return false;
		return true;
	}
	if (op == "status") return false;
	return true;
}

std::optional<uint32_t> StoreBlueprintToolPlan(BlueprintToolPlan plan)
{
	if (plan.chunks.empty() || plan.prepare_count > plan.chunks.size()) return std::nullopt;
	if (plan.chunks.size() > MAX_PLAN_CHUNKS) return std::nullopt;
	const size_t bytes = PlanBytes(plan);
	if (bytes > MAX_PLAN_BYTES) return std::nullopt;

	/* The id space is 32 bit; once it is used up the store refuses instead of reusing an old id. The
	 * check comes first, so an exhausted store never evicts a plan it could not replace. */
	if (_next_plan_id > UINT32_MAX) return std::nullopt;

	/* Make room: finished and stopped plans are dropped first, then the oldest plan without a result in flight. */
	auto evict = [](bool only_finished) {
		for (auto it = _plans.begin(); it != _plans.end(); ++it) {
			if (it->pending) continue;
			if (only_finished && !IsPlanFinished(*it)) continue;
			_stored_bytes -= PlanBytes(*it);
			_plans.erase(it);
			return true;
		}
		return false;
	};
	while (_plans.size() >= MAX_PLANS || _stored_bytes + bytes > MAX_PLAN_STORE_BYTES) {
		if (!evict(true) && !evict(false)) return std::nullopt;
	}

	plan.plan_id = static_cast<uint32_t>(_next_plan_id++);
	/* The plan belongs to this lifetime of the company, not just to its id. */
	plan.company_generation = CompanyLifetime(plan.company);
	plan.next_chunk = 0;
	plan.cost = 0;
	plan.built = 0;
	plan.failed = 0;
	plan.last_error = INVALID_STRING_ID;
	plan.out_of_money = false;
	plan.pending = false;
	plan.stopped = false;
	_stored_bytes += bytes;
	_plans.push_back(std::move(plan));
	return _plans.back().plan_id;
}

uint64_t BlueprintToolPlanIdCounter()
{
	return _next_plan_id;
}

void SetBlueprintToolPlanIdCounterForTest(uint64_t next_plan_id)
{
	_next_plan_id = std::max<uint64_t>(next_plan_id, 1);
}

void AgentBlueprintChunkCallback(Commands, const CommandCost &result, Money extra_money, uint32_t built, uint32_t failed, StringID last_error)
{
	_callback_seen = true;
	if (_in_flight.empty()) return;

	const InFlightChunk in_flight = _in_flight.front();
	_in_flight.pop_front();

	BlueprintToolPlan *plan = FindPlan(in_flight.plan_id);
	if (plan == nullptr) return;
	plan->pending = false;
	AccountChunk(*plan, result, extra_money, built, failed, last_error);
}

} // namespace InGameAgent
