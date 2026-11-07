/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file test_agent_blueprint_tools.cpp Test the dispatch, the argument validation, the bounded plan
 * store and the honest chunk accounting of the agent blueprint and trunk line tools.
 *
 * No game is running in this test binary, so the map is empty: every call that takes a tile is only
 * validated here, and the one command a case really posts fails the way the engine fails without a
 * map. That is exactly what the last case checks: a posted chunk that the engine did not build is
 * not reported as done.
 */

#include "../stdafx.h"

#include <filesystem>
#include <fstream>

#include "../3rdparty/catch2/catch.hpp"

#include "../agent_blueprint_tools.h"
#include "../blueprint.h"
#include "../company_base.h"
#include "../core/backup_type.hpp"
#include "../fileio_func.h"
#include "../map_func.h"

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
 * Dispatch a call of the module and require that the module owns the method.
 * @param method The method name.
 * @param params The parameters.
 * @return The result document.
 */
static Json Dispatch(std::string_view method, Json params)
{
	std::optional<Json> result = DispatchBlueprintTool(method, params, CompanyID::Invalid());
	REQUIRE(result.has_value());
	return *result;
}

/* The bridge's world ids and epochs are lower case version 4 UUIDs, exactly as GenerateUuid() makes them. */
static constexpr std::string_view TASK_ID = "0f1c7f0e-6a2b-4d3c-9e58-7b1d2c3a4e60";
static constexpr std::string_view WORLD_ID = "c2d4b8a6-1f39-4e57-8a02-5d6c7b8e9f10";
static constexpr std::string_view EPOCH = "9b3e6d21-4c58-4a7f-b0d1-2e3f4a5b6c7d";
static constexpr std::string_view OTHER_TASK_ID = "7a6b5c4d-3e2f-4a1b-9c8d-0e1f2a3b4c5d";
static constexpr std::string_view OTHER_WORLD_ID = "5e4d3c2b-1a09-4f8e-9d7c-6b5a4f3e2d1c";
static constexpr std::string_view OTHER_EPOCH = "1a2b3c4d-5e6f-4a7b-8c9d-0e1f2a3b4c5d";

/**
 * Build the binding fields of a call.
 * @param plan_id The plan id.
 * @param task_id The task id.
 * @param world_id The world id.
 * @param epoch The world epoch.
 * @return The parameter object of the call.
 */
static Json Binding(uint32_t plan_id, std::string_view task_id = TASK_ID, std::string_view world_id = WORLD_ID, std::string_view epoch = EPOCH)
{
	return Json{{"plan_id", plan_id}, {"task_id", task_id}, {"world_id", world_id}, {"epoch", epoch}};
}

/**
 * Add the operation to a parameter object.
 * @param params The parameters.
 * @param op The operation.
 * @return The parameters with the operation.
 */
static Json WithOp(Json params, std::string_view op)
{
	params["op"] = std::string(op);
	return params;
}

/**
 * Build one synthetic chunk that builds a single straight track piece.
 * @return The chunk.
 */
static BlueprintChunkData TrackChunk()
{
	BlueprintChunkData chunk;
	/* Kind Track, x 0, y 0, rail type 0, TrackBits X. */
	chunk.data = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
	return chunk;
}

/**
 * Build a small plan with a number of synthetic chunks.
 * @param chunks The number of chunks.
 * @param company The company the plan is made for.
 * @return The plan.
 */
static BlueprintToolPlan MakePlan(size_t chunks, CompanyID company = static_cast<CompanyID>(0))
{
	BlueprintToolPlan plan;
	plan.route = false;
	plan.name = "synthetic";
	plan.origin = TileXY(0, 0);
	plan.company = company;
	plan.task_id = std::string(TASK_ID);
	plan.world_id = std::string(WORLD_ID);
	plan.epoch = std::string(EPOCH);
	plan.prepare_count = 0;
	for (size_t i = 0; i < chunks; i++) plan.chunks.push_back(TrackChunk());
	return plan;
}

/**
 * A real company in the engine's own company pool, removed again when the guard goes out of scope.
 *
 * The cases work on the engine's own company objects, so the lifetime generation they use is the real
 * one the pool assigns, not a number the test made up. #Remove deletes the company without ending the
 * guard, which is how a case reaches the deletion and the reuse of a company id.
 */
class CompanyGuard {
public:
	CompanyGuard()
	{
		/* The pool asserts that the allocation was checked, exactly as the bridge's own test does. */
		REQUIRE(Company::CanAllocateItem());
		this->company = Company::Create();
		REQUIRE(this->company != nullptr);
	}

	~CompanyGuard()
	{
		if (this->company != nullptr) delete this->company;
	}

	CompanyID Id() const { return this->company->index; }
	uint64_t Generation() const { return this->company->generation; }

	/** Delete the company now, so the engine can reuse its id. */
	void Remove()
	{
		delete this->company;
		this->company = nullptr;
	}

private:
	Company *company = nullptr; ///< The company, or nullptr after it was removed.
};

/**
 * A scratch personal directory that belongs to one test invocation only.
 *
 * The directory is created exclusively in the system temporary directory: the name is tried in turn
 * and only the invocation that really creates it keeps it, so two test binaries of two users (or two
 * runs of the same binary) never share a directory, and no fixed path is reused. Writability is proven
 * with a probe file, because the case really saves a blueprint there. The guard removes the directory
 * when it goes out of scope; it is declared before the AutoRestoreBackup of the personal directory, so
 * the engine's path is restored first and the directory is removed afterwards.
 */
class ScratchDirectory {
public:
	ScratchDirectory()
	{
		std::error_code error_code;
		const std::filesystem::path base = std::filesystem::temp_directory_path(error_code);
		REQUIRE(!error_code);

		static uint32_t serial = 0;
		for (uint32_t attempt = 0; attempt < 1000 && this->path.empty(); attempt++) {
			const std::filesystem::path candidate = base / fmt::format("openttd_agent_blueprint_test_{}", serial++);
			if (std::filesystem::create_directory(candidate, error_code)) this->path = candidate;
			error_code.clear();
		}
		REQUIRE_FALSE(this->path.empty());

		/* A real permission check: this user must be able to write in the directory it just made. */
		const std::filesystem::path probe = this->path / "write_probe";
		{
			std::ofstream file(probe, std::ios::binary);
			REQUIRE(file.good());
			file.put('x');
			file.flush();
			REQUIRE(file.good());
		}
		REQUIRE(std::filesystem::exists(probe, error_code));
		REQUIRE(std::filesystem::remove(probe, error_code));
		REQUIRE(!error_code);
	}

	~ScratchDirectory()
	{
		std::error_code error_code;
		std::filesystem::remove_all(this->path, error_code);
	}

	/**
	 * The directory as the engine's personal directory, with the trailing separator it expects.
	 * @return The path of the directory.
	 */
	std::string Path() const
	{
		return this->path.string() + "/";
	}

private:
	std::filesystem::path path; ///< The directory, empty until it is created.
};

TEST_CASE("Agent blueprint tools: capabilities")
{
	const Json capabilities = BlueprintToolCapabilities();
	REQUIRE(capabilities["ok"] == true);

	const Json blueprint = FindCapability(capabilities, "world.blueprint");
	REQUIRE(!blueprint.empty());
	for (const char *op : {"list", "inspect", "capture", "plan", "status", "execute_chunk", "stop", "drop"}) {
		CHECK(Contains(blueprint["implemented"], op));
	}
	/* The operations that are really missing are still listed as gaps. */
	CHECK(Contains(blueprint["gaps"], "rename"));
	CHECK(Contains(blueprint["gaps"], "delete"));

	const Json route = FindCapability(capabilities, "world.rail_route");
	REQUIRE(!route.empty());
	for (const char *op : {"plan", "status", "execute_chunk", "stop", "drop"}) {
		CHECK(Contains(route["implemented"], op));
	}

	/* The document names the module and the authority of every method, as the other tool modules do. */
	CHECK(capabilities["module"] == "agent_blueprint_tools");
	CHECK(blueprint["authority"] == "write");
	CHECK(route["authority"] == "write");
	/* `kind` is kept as the same value for readers of the older spelling. */
	CHECK(blueprint["kind"] == "write");

	/* The query operations are listed apart from the write operations. */
	REQUIRE(capabilities["query_ops"].is_array());
	CHECK(Contains(capabilities["query_ops"], "world.blueprint op=list"));
	CHECK(Contains(capabilities["query_ops"], "world.blueprint op=inspect"));
	CHECK(Contains(capabilities["query_ops"], "world.blueprint op=status"));
	CHECK(Contains(capabilities["query_ops"], "world.rail_route op=status"));

	/* The bounds are published, so the bridge can plan around them. */
	const Json bounds = capabilities["bounds"];
	CHECK(bounds["capture_tiles"] == 8192);
	CHECK(bounds["plans"] == 8);
	CHECK(bounds["plan_chunks"] == 1024);
	CHECK(bounds["plan_bytes"] == 512 * 1024);
	CHECK(bounds["plan_store_bytes"] == 2 * 1024 * 1024);
	CHECK(bounds["list_limit"] == 256);
	CHECK(bounds["item_limit"] == 512);
	CHECK(bounds["name_bytes"] == 48);
	CHECK(!capabilities["notes"].empty());

	/* Methods of the other modules are not owned by this one. */
	CHECK_FALSE(DispatchBlueprintTool("world.construct", Json::object(), CompanyID::Invalid()).has_value());
	CHECK_FALSE(DispatchBlueprintTool("world.query", Json::object(), CompanyID::Invalid()).has_value());
}

TEST_CASE("Agent blueprint tools: mutation classification")
{
	/* The read-only operations do not change the world. */
	CHECK_FALSE(IsBlueprintMutation("world.blueprint", Json::object({{"op", "list"}})));
	CHECK_FALSE(IsBlueprintMutation("world.blueprint", Json::object({{"op", "inspect"}})));
	CHECK_FALSE(IsBlueprintMutation("world.blueprint", Json::object({{"op", "status"}})));
	CHECK_FALSE(IsBlueprintMutation("world.rail_route", Json::object({{"op", "status"}})));

	/* Capturing, planning and the plan store changes are reported as mutations. */
	CHECK(IsBlueprintMutation("world.blueprint", Json::object({{"op", "capture"}})));
	CHECK(IsBlueprintMutation("world.blueprint", Json::object({{"op", "plan"}})));
	CHECK(IsBlueprintMutation("world.blueprint", Json::object({{"op", "execute_chunk"}})));
	CHECK(IsBlueprintMutation("world.blueprint", Json::object({{"op", "stop"}})));
	CHECK(IsBlueprintMutation("world.blueprint", Json::object({{"op", "drop"}})));
	CHECK(IsBlueprintMutation("world.rail_route", Json::object({{"op", "plan"}})));
	CHECK(IsBlueprintMutation("world.rail_route", Json::object({{"op", "execute_chunk"}})));

	/* An unknown operation and a call without an operation are treated as mutations. */
	CHECK(IsBlueprintMutation("world.blueprint", Json::object({{"op", "no_such_op"}})));
	CHECK(IsBlueprintMutation("world.blueprint", Json::object()));
	CHECK(IsBlueprintMutation("world.rail_route", Json::object()));

	/* The methods of the other modules are not mutations of this one. */
	CHECK_FALSE(IsBlueprintMutation("world.construct", Json::object()));
	CHECK_FALSE(IsBlueprintMutation("world.towns", Json::object({{"op", "found"}})));
}

TEST_CASE("Agent blueprint tools: dispatch and name validation")
{
	/* An unknown operation of an owned method is a reported gap. */
	const Json gap = Dispatch("world.blueprint", Json::object({{"op", "window"}}));
	CHECK(gap["ok"] == false);
	CHECK(gap["error"] == "unsupported_op");
	CHECK(gap["detail"]["op"] == "window");

	CHECK(Dispatch("world.rail_route", Json::object({{"op", "preview_tiles"}}))["error"] == "unsupported_op");
	CHECK(Dispatch("world.blueprint", Json::object())["error"] == "invalid_arguments");
	CHECK(Dispatch("world.blueprint", Json::object({{"op", 7}}))["error"] == "invalid_arguments");

	/* A blueprint name is exactly one file name in the native blueprint directory. */
	CHECK(Dispatch("world.blueprint", Json::object({{"op", "inspect"}}))["error"] == "invalid_arguments");
	CHECK(Dispatch("world.blueprint", Json::object({{"op", "inspect"}, {"name", ""}}))["error"] == "invalid_arguments");
	CHECK(Dispatch("world.blueprint", Json::object({{"op", "inspect"}, {"name", "a/b"}}))["error"] == "invalid_arguments");
	CHECK(Dispatch("world.blueprint", Json::object({{"op", "inspect"}, {"name", "../escape"}}))["error"] == "invalid_arguments");
	CHECK(Dispatch("world.blueprint", Json::object({{"op", "inspect"}, {"name", "with:colon"}}))["error"] == "invalid_arguments");
	CHECK(Dispatch("world.blueprint", Json::object({{"op", "inspect"}, {"name", "trailing "}}))["error"] == "invalid_arguments");
	CHECK(Dispatch("world.blueprint", Json::object({{"op", "inspect"}, {"name", std::string(60, 'x')}}))["error"] == "invalid_arguments");
	CHECK(Dispatch("world.blueprint", Json::object({{"op", "inspect"}, {"name", 7}}))["error"] == "invalid_arguments");
	const Json missing = Dispatch("world.blueprint", Json::object({{"op", "inspect"}, {"name", "agent_tools_no_such_blueprint"}}));
	CHECK(missing["ok"] == false);
	CHECK(missing["error"] == "unknown_blueprint");
}

TEST_CASE("Agent blueprint tools: blueprint file round trip")
{
	/* Blueprints live in the blueprint directory of the personal directory; use a scratch one that
	 * belongs to this invocation only. The guard comes first, so the personal directory is restored
	 * before the scratch directory is removed. */
	const ScratchDirectory scratch;
	const AutoRestoreBackup<std::string> personal_dir(_personal_dir, scratch.Path());

	/* Save a blueprint with the engine's own writer, then read it back through the module. */
	const std::string name = "agent_tools_test_blueprint";
	Blueprint bp;
	bp.name = name;
	bp.width = 3;
	bp.height = 1;
	bp.railtypes.push_back(RailTypeLabel{"RAIL"});
	bp.tracks.push_back({0, 0, 0, TrackBits{Track::X}});
	bp.tracks.push_back({1, 0, 0, TrackBits{Track::Y}});
	bp.signals.push_back({0, 0, Track::X, SignalType::Block, SignalVariant::Electric, 3});
	bp.depots.push_back({2, 0, 0, DiagDirection::NE});
	bp.tunnelbridges.push_back({0, 0, 1, 0, 0, true, 0});
	bp.stations.push_back({2, 0, 0, 0, Axis::X, false, UINT16_MAX});
	REQUIRE(SaveBlueprint(bp));

	/* The writer really wrote into this invocation's own directory, and the file is readable again. */
	const std::string file = GetBlueprintFileName(name);
	CHECK(file.rfind(scratch.Path(), 0) == 0);
	std::error_code error_code;
	REQUIRE(std::filesystem::exists(OTTD2FS(file), error_code));
	REQUIRE(!error_code);

	/* The listing finds it. */
	const Json list = Dispatch("world.blueprint", Json::object({{"op", "list"}, {"limit", 256}}));
	REQUIRE(list["ok"] == true);
	REQUIRE(list["total"].is_number_integer());
	bool listed = false;
	for (const Json &entry : list["blueprints"]) {
		if (entry["name"] == name) listed = true;
	}
	CHECK(listed);

	/* Inspecting it reports the real content, bounded. */
	const Json info = Dispatch("world.blueprint", Json::object({{"op", "inspect"}, {"name", name}, {"limit", 2}}));
	REQUIRE(info["ok"] == true);
	CHECK(info["name"] == name);
	CHECK(info["width"] == 3);
	CHECK(info["height"] == 1);
	CHECK(info["empty"] == false);
	CHECK(info["items"].is_array());
	CHECK(info["counts"]["tracks"] == 2);
	CHECK(info["counts"]["signals"] == 1);
	CHECK(info["counts"]["depots"] == 1);
	CHECK(info["counts"]["tunnelbridges"] == 1);
	CHECK(info["counts"]["stations"] == 1);
	CHECK(info["railtypes"][0] == "RAIL");
	CHECK(info["items_total"] == 6);
	CHECK(info["items"].size() == 2);
	CHECK(info["items"][0]["kind"] == "track");
	CHECK(info["items"][0]["railtype"] == "RAIL");
	CHECK(info["limit"] == 2);

	/* The window is applied over the whole item stream, kind by kind. */
	const Json tail = Dispatch("world.blueprint", Json::object({{"op", "inspect"}, {"name", name}, {"offset", 5}, {"limit", 8}}));
	REQUIRE(tail["ok"] == true);
	REQUIRE(tail["items"].size() == 1);
	CHECK(tail["items"][0]["kind"] == "station");
	CHECK(tail["offset"] == 5);

	/* An out of range window is refused. */
	CHECK(Dispatch("world.blueprint", Json::object({{"op", "inspect"}, {"name", name}, {"limit", 0}}))["error"] == "invalid_arguments");
	CHECK(Dispatch("world.blueprint", Json::object({{"op", "inspect"}, {"name", name}, {"offset", -1}}))["error"] == "invalid_arguments");

	REQUIRE(DeleteBlueprint(name));
	CHECK(Dispatch("world.blueprint", Json::object({{"op", "inspect"}, {"name", name}}))["error"] == "unknown_blueprint");
}

TEST_CASE("Agent blueprint tools: plan validation")
{
	/* The blueprint arguments are validated before the map is touched. */
	CHECK(Dispatch("world.blueprint", Json::object({{"op", "plan"}}))["error"] == "invalid_arguments");
	const Json bad_rotation = Dispatch("world.blueprint", Json::object({{"op", "plan"}, {"name", "agent_tools_test_blueprint"},
			{"rotation", 5}, {"task_id", TASK_ID}, {"world_id", WORLD_ID}, {"epoch", EPOCH}}));
	CHECK(bad_rotation["ok"] == false);
	CHECK(bad_rotation["error"] == "invalid_arguments");
	CHECK(Dispatch("world.blueprint", Json::object({{"op", "plan"}, {"name", "agent_tools_test_blueprint"},
			{"mirror", "yes"}, {"task_id", TASK_ID}, {"world_id", WORLD_ID}, {"epoch", EPOCH}}))["error"] == "invalid_arguments");
	CHECK(Dispatch("world.blueprint", Json::object({{"op", "plan"}, {"name", "agent_tools_test_blueprint"},
			{"railtype", "ZZZZ"}, {"task_id", TASK_ID}, {"world_id", WORLD_ID}, {"epoch", EPOCH}}))["error"] == "unknown_railtype");
	/* The binding is required: a plan belongs to one task on one world epoch. */
	CHECK(Dispatch("world.blueprint", Json::object({{"op", "plan"}, {"name", "agent_tools_test_blueprint"}}))["error"] == "invalid_arguments");
	CHECK(Dispatch("world.blueprint", Json::object({{"op", "plan"}, {"name", "agent_tools_test_blueprint"},
			{"task_id", TASK_ID}, {"world_id", WORLD_ID}, {"epoch", 7}}))["error"] == "invalid_arguments");
	/* Without a map there is no tile the blueprint could be pasted on. */
	CHECK(Dispatch("world.blueprint", Json::object({{"op", "plan"}, {"name", "agent_tools_test_blueprint"},
			{"origin", Json{{"x", 0}, {"y", 0}}}, {"task_id", TASK_ID}, {"world_id", WORLD_ID}, {"epoch", EPOCH}}))["error"] == "invalid_arguments");

	/* The trunk line arguments are validated before the map is touched too. */
	CHECK(Dispatch("world.rail_route", Json::object({{"op", "plan"}}))["error"] == "invalid_arguments");
	CHECK(Dispatch("world.rail_route", Json::object({{"op", "plan"}, {"tracks", 0}}))["error"] == "invalid_arguments");
	CHECK(Dispatch("world.rail_route", Json::object({{"op", "plan"}, {"tracks", 31}}))["error"] == "invalid_arguments");
	CHECK(Dispatch("world.rail_route", Json::object({{"op", "plan"}, {"tracks", 2}, {"start", "diagonal"}}))["error"] == "invalid_arguments");
	CHECK(Dispatch("world.rail_route", Json::object({{"op", "plan"}, {"tracks", 2}, {"signal_type", 9}}))["error"] == "invalid_arguments");
	CHECK(Dispatch("world.rail_route", Json::object({{"op", "plan"}, {"tracks", 2}, {"signal_interval", 0}}))["error"] == "invalid_arguments");
	const Json no_binding = Dispatch("world.rail_route", Json::object({{"op", "plan"}, {"tracks", 2}, {"signal_interval", 6}}));
	CHECK(no_binding["ok"] == false);
	CHECK(no_binding["error"] == "invalid_arguments");
}

TEST_CASE("Agent blueprint tools: bounded plan store")
{
	/* The store keeps a bounded number of plans and evicts the oldest one. */
	std::vector<uint32_t> ids;
	for (size_t i = 0; i < 9; i++) {
		const std::optional<uint32_t> id = StoreBlueprintToolPlan(MakePlan(1));
		REQUIRE(id.has_value());
		ids.push_back(*id);
	}
	CHECK(ids[0] != ids[8]);
	CHECK(Dispatch("world.blueprint", WithOp(Binding(ids[0]), "status"))["error"] == "unknown_plan");
	const Json newest = Dispatch("world.blueprint", WithOp(Binding(ids[8]), "status"));
	REQUIRE(newest["ok"] == true);
	CHECK(newest["kind"] == "blueprint");
	CHECK(newest["chunks"]["total"] == 1);
	CHECK(newest["next_chunk_index"] == 0);
	CHECK(newest["done"] == false);
	CHECK(newest["binding"]["task_id"] == TASK_ID);
	CHECK(newest["binding"]["epoch"] == EPOCH);

	/* An empty plan and a plan of too many chunks are refused instead of truncated. */
	CHECK_FALSE(StoreBlueprintToolPlan(MakePlan(0)).has_value());
	BlueprintToolPlan big = MakePlan(1025);
	CHECK_FALSE(StoreBlueprintToolPlan(std::move(big)).has_value());

	/* The id space is 32 bit: the last id is handed out once and the store then refuses instead of reusing one. */
	const uint64_t counter = BlueprintToolPlanIdCounter();
	CHECK(counter > 1);
	SetBlueprintToolPlanIdCounterForTest(UINT32_MAX);
	const std::optional<uint32_t> last = StoreBlueprintToolPlan(MakePlan(1));
	REQUIRE(last.has_value());
	CHECK(*last == UINT32_MAX);
	CHECK(BlueprintToolPlanIdCounter() == static_cast<uint64_t>(UINT32_MAX) + 1);
	CHECK_FALSE(StoreBlueprintToolPlan(MakePlan(1)).has_value());
	/* A refused plan reports the exhaustion, and the ids already handed out stay usable. */
	const Json exhausted = Dispatch("world.blueprint", WithOp(Binding(*last), "status"));
	REQUIRE(exhausted["ok"] == true);
	CHECK(exhausted["plan_id"] == UINT32_MAX);
	CHECK(Dispatch("world.blueprint", WithOp(Binding(*last), "drop"))["ok"] == true);
	SetBlueprintToolPlanIdCounterForTest(counter);

	/* Clean up: drop every plan this case made. */
	for (const uint32_t id : ids) {
		Dispatch("world.blueprint", Json::object({{"op", "drop"}, {"plan_id", id}, {"task_id", TASK_ID}, {"world_id", WORLD_ID}, {"epoch", EPOCH}}));
	}
}

TEST_CASE("Agent blueprint tools: chunk execution accounting")
{
	/* A real company, so the chunk is posted for a company that exists and the plan carries its
	 * lifetime generation. */
	const CompanyGuard company;
	const std::optional<uint32_t> stored = StoreBlueprintToolPlan(MakePlan(2, company.Id()));
	REQUIRE(stored.has_value());
	const uint32_t plan_id = *stored;

	/* A plan can only be executed by the task, world and epoch it was made for. */
	CHECK(Dispatch("world.blueprint", Json::object({{"op", "execute_chunk"}, {"plan_id", plan_id}, {"chunk_index", 0}}))["error"] == "invalid_arguments");
	const Json other_task = Dispatch("world.blueprint", Json::object({{"op", "execute_chunk"}, {"plan_id", plan_id},
			{"chunk_index", 0}, {"task_id", OTHER_TASK_ID}, {"world_id", WORLD_ID}, {"epoch", EPOCH}}));
	CHECK(other_task["ok"] == false);
	CHECK(other_task["error"] == "plan_binding_mismatch");
	const Json other_epoch = Dispatch("world.blueprint", Json::object({{"op", "execute_chunk"}, {"plan_id", plan_id},
			{"chunk_index", 0}, {"task_id", TASK_ID}, {"world_id", WORLD_ID}, {"epoch", OTHER_EPOCH}}));
	CHECK(other_epoch["error"] == "plan_binding_mismatch");
	CHECK(Dispatch("world.blueprint", Json::object({{"op", "execute_chunk"}, {"plan_id", 999999},
			{"chunk_index", 0}, {"task_id", TASK_ID}, {"world_id", WORLD_ID}, {"epoch", EPOCH}}))["error"] == "unknown_plan");

	/* Chunks are executed in order and inside the plan. */
	Json params = Binding(plan_id);
	params["op"] = "execute_chunk";
	params["chunk_index"] = 1;
	const Json out_of_order = Dispatch("world.blueprint", params);
	CHECK(out_of_order["ok"] == false);
	CHECK(out_of_order["error"] == "out_of_order");
	CHECK(out_of_order["detail"]["next_chunk_index"] == 0);
	params["chunk_index"] = 5;
	CHECK(Dispatch("world.blueprint", params)["error"] == "invalid_chunk_index");

	/* One chunk per call: the engine really runs it, and without a map it builds nothing. */
	params["chunk_index"] = 0;
	const Json chunk = Dispatch("world.blueprint", params);
	REQUIRE(chunk["ok"] == true);
	CHECK(chunk["plan_id"] == plan_id);
	CHECK(chunk["chunk_index"] == 0);
	CHECK(chunk["chunk_kind"] == "build");
	CHECK(chunk["chunk_bytes"] == 7);
	/* Nothing was built, so the chunk is not counted as done: submitted work is never called done. */
	CHECK(chunk["executed"] == false);
	CHECK(chunk["submitted"] == false);
	CHECK(chunk["command_failed"] == true);
	CHECK(chunk["next_chunk_index"] == 0);
	CHECK(chunk["chunks_done"] == 0);
	CHECK(chunk["done"] == false);
	CHECK(chunk["cost"] == 0);
	CHECK(chunk["built"] == 0);

	/* The status reports the same state. */
	const Json status = Dispatch("world.blueprint", WithOp(Binding(plan_id), "status"));
	REQUIRE(status["ok"] == true);
	CHECK(status["next_chunk_index"] == 0);
	CHECK(status["done"] == false);
	CHECK(status["stopped"] == false);
	CHECK(status["binding"]["task_id"] == TASK_ID);
	CHECK(status["binding"]["world_id"] == WORLD_ID);
	CHECK(status["binding"]["epoch"] == EPOCH);

	/* A read is bound like a write: another chat, another world or another epoch learns nothing. */
	CHECK(Dispatch("world.blueprint", WithOp(Binding(plan_id, OTHER_TASK_ID), "status"))["error"] == "plan_binding_mismatch");
	CHECK(Dispatch("world.blueprint", WithOp(Binding(plan_id, TASK_ID, OTHER_WORLD_ID), "status"))["error"] == "plan_binding_mismatch");
	const Json old_epoch_status = Dispatch("world.blueprint", WithOp(Binding(plan_id, TASK_ID, WORLD_ID, OTHER_EPOCH), "status"));
	CHECK(old_epoch_status["ok"] == false);
	CHECK(old_epoch_status["error"] == "plan_binding_mismatch");
	CHECK(old_epoch_status["detail"]["epoch"] == EPOCH);
	CHECK(old_epoch_status["detail"]["task_id"] == TASK_ID);
	/* The binding itself is required, so a call can never read a plan it did not name. */
	CHECK(Dispatch("world.blueprint", Json::object({{"op", "status"}, {"plan_id", plan_id}}))["error"] == "invalid_arguments");

	/* Stopping drops the remaining chunks and no chunk can run afterwards. */
	const Json stopped = Dispatch("world.blueprint", Json::object({{"op", "stop"}, {"plan_id", plan_id},
			{"task_id", TASK_ID}, {"world_id", WORLD_ID}, {"epoch", EPOCH}}));
	REQUIRE(stopped["ok"] == true);
	CHECK(stopped["stopped"] == true);
	CHECK(stopped["done"] == true);
	CHECK(stopped["dropped_chunks"] == 2);
	CHECK(stopped["chunks"]["total"] == 0);
	CHECK(Dispatch("world.blueprint", params)["error"] == "plan_stopped");

	/* Dropping removes the plan; a stopped plan can still be dropped. */
	const Json dropped = Dispatch("world.blueprint", Json::object({{"op", "drop"}, {"plan_id", plan_id},
			{"task_id", TASK_ID}, {"world_id", WORLD_ID}, {"epoch", EPOCH}}));
	REQUIRE(dropped["ok"] == true);
	CHECK(dropped["dropped"] == true);
	CHECK(Dispatch("world.blueprint", WithOp(Binding(plan_id), "status"))["error"] == "unknown_plan");

	/* Stopping and dropping belong to the owning task. */
	const std::optional<uint32_t> owned = StoreBlueprintToolPlan(MakePlan(1));
	REQUIRE(owned.has_value());
	CHECK(Dispatch("world.blueprint", Json::object({{"op", "stop"}, {"plan_id", *owned},
			{"task_id", OTHER_TASK_ID}, {"world_id", WORLD_ID}, {"epoch", EPOCH}}))["error"] == "plan_binding_mismatch");
	CHECK(Dispatch("world.blueprint", Json::object({{"op", "drop"}, {"plan_id", *owned},
			{"task_id", TASK_ID}, {"world_id", WORLD_ID}, {"epoch", EPOCH}}))["ok"] == true);
}

TEST_CASE("Agent blueprint tools: plan company lifetime")
{
	/* A real company in the engine's pool: the case uses the generation the pool assigns, so a
	 * deleted and reused company id is the real thing and not a made up number. */
	CompanyGuard company;
	const CompanyID company_id = company.Id();
	const uint64_t generation = company.Generation();
	REQUIRE(generation != 0);

	const std::optional<uint32_t> stored = StoreBlueprintToolPlan(MakePlan(1, company_id));
	REQUIRE(stored.has_value());
	const uint32_t plan_id = *stored;

	/* The plan reports the company and the lifetime it was made for. */
	Json status_params = Binding(plan_id);
	status_params["op"] = "status";
	const Json status = Dispatch("world.blueprint", status_params);
	REQUIRE(status["ok"] == true);
	CHECK(status["company_id"] == company_id.base());
	CHECK(status["company_generation"] == generation);
	CHECK(status["company_current"] == true);
	CHECK(status["binding"]["company_generation"] == generation);

	Json chunk_params = Binding(plan_id);
	chunk_params["op"] = "execute_chunk";
	chunk_params["chunk_index"] = 0;

	/* Another company of the same task cannot execute the plan of this one. */
	CompanyGuard other;
	REQUIRE(other.Id() != company_id);
	const Json other_actor = *DispatchBlueprintTool("world.blueprint", chunk_params, other.Id());
	CHECK(other_actor["ok"] == false);
	CHECK(other_actor["error"] == "plan_actor_mismatch");
	CHECK(other_actor["detail"]["actor_id"] == other.Id().base());
	CHECK(other_actor["detail"]["company_id"] == company_id.base());

	/* The company is deleted: its id is free and the plan of that lifetime cannot be executed. */
	company.Remove();
	CHECK_FALSE(Company::IsValidID(company_id));
	const Json deleted = Dispatch("world.blueprint", chunk_params);
	CHECK(deleted["ok"] == false);
	CHECK(deleted["error"] == "company_lifetime_mismatch");
	CHECK(deleted["detail"]["company_generation"] == generation);
	CHECK(deleted["detail"]["generation_now"] == 0);

	/* The engine really reuses the id: the new company is another lifetime at the same index. */
	CompanyGuard again;
	REQUIRE(again.Id() == company_id);
	REQUIRE(again.Generation() != generation);
	const Json reused = Dispatch("world.blueprint", chunk_params);
	CHECK(reused["ok"] == false);
	CHECK(reused["error"] == "company_lifetime_mismatch");
	CHECK(reused["detail"]["generation_now"] == again.Generation());

	/* The status of the stored plan is honest about the lifetime that is gone. */
	const Json stale = Dispatch("world.blueprint", status_params);
	REQUIRE(stale["ok"] == true);
	CHECK(stale["company_current"] == false);

	/* The parameters may name the actor of the request but never override it: here the request actor is
	 * the other company and the parameters name the plan's (reused) id, so the two really differ. */
	Json named_actor = chunk_params;
	named_actor["company_id"] = company_id.base();
	const Json overridden = *DispatchBlueprintTool("world.blueprint", named_actor, other.Id());
	CHECK(overridden["ok"] == false);
	CHECK(overridden["error"] == "invalid_arguments");
	CHECK(overridden["detail"]["actor_id"] == other.Id().base());
	CHECK(overridden["detail"]["company_id"] == company_id.base());

	/* Clean up: the store does not keep a plan of a company that is gone. */
	Dispatch("world.blueprint", Json::object({{"op", "drop"}, {"plan_id", plan_id}, {"task_id", TASK_ID}, {"world_id", WORLD_ID}, {"epoch", EPOCH}}));
}

TEST_CASE("Agent blueprint tools: capture validation")
{
	/* A capture needs both corners inside the map. */
	CHECK(Dispatch("world.blueprint", Json::object({{"op", "capture"}}))["error"] == "invalid_arguments");
	CHECK(Dispatch("world.blueprint", Json::object({{"op", "capture"}, {"corner1", Json{{"x", 0}, {"y", 0}}}}))["error"] == "invalid_arguments");
	/* The test binary has no map, so no tile is inside it. */
	const Json no_map = Dispatch("world.blueprint", Json::object({{"op", "capture"},
			{"corner1", Json{{"x", 0}, {"y", 0}}}, {"corner2", Json{{"x", 1}, {"y", 1}}}}));
	CHECK(no_map["ok"] == false);
	CHECK(no_map["error"] == "invalid_arguments");
}
