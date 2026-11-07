/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file agent_bridge.cpp Native bridge between the running game and the in-game agent backend. */

#include "stdafx.h"
#include "agent_bridge.h"

#include "agent_admin_tools.h"
#include "agent_blueprint_tools.h"
#include "agent_capture.h"
#include "agent_chat_gui.h"
#include "agent_command_catalog.h"
#include "agent_game_tools.h"
#include "agent_transport_tools.h"
#include "bridge_map.h"
#include "clear_map.h"
#include "command_func.h"
#include "company_base.h"
#include "company_cmd.h"
#include "company_func.h"
#include "core/backup_type.hpp"
#include "core/math_func.hpp"
#include "debug.h"
#include "depot_base.h"
#include "depot_map.h"
#include "engine_base.h"
#include "genworld.h"
#include "group.h"
#include "industry.h"
#include "map_func.h"
#include "misc_cmd.h"
#include "network/core/http.h"
#include "network/network.h"
#include "object_base.h"
#include "object_map.h"
#include "openttd.h"
#include "progress.h"
#include "rail_map.h"
#include "rev.h"
#include "road_map.h"
#include "signs_base.h"
#include "signs_cmd.h"
#include "station_base.h"
#include "station_cmd.h"
#include "station_map.h"
#include "strings_func.h"
#include "table/strings.h"
#include "timer/timer_game_calendar.h"
#include "timer/timer_game_tick.h"
#include "tile_map.h"
#include "town.h"
#include "town_cmd.h"
#include "town_map.h"
#include "tree_map.h"
#include "tunnel_map.h"
#include "tunnelbridge_map.h"
#include "vehicle_base.h"
#include "vehicle_cmd.h"
#include "video/video_driver.hpp"
#include "water_map.h"
#include "waypoint_base.h"
#include "waypoint_cmd.h"

#include "3rdparty/fmt/format.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "safeguards.h"

namespace InGameAgent {

namespace {

using Clock = std::chrono::steady_clock;

/** Delay before retrying a failed hello. */
constexpr auto HELLO_RETRY = std::chrono::seconds(5);
/** Delay before retrying a failed poll. */
constexpr auto POLL_RETRY = std::chrono::milliseconds(2000);
/**
 * Delay between two polls while the backend is healthy. This is the tool latency, not the
 * heartbeat: a request waits at most one idle interval before the engine picks it up, while the
 * heartbeat the backend asks for stays a separate, much slower connection signal. The floor keeps
 * the loop from turning into a busy poll of the HTTP layer.
 */
constexpr auto POLL_IDLE = std::chrono::milliseconds(150);
/**
 * Budget of the plan for one ordinary query/batch main-thread slice, in microseconds:
 * "no individual ordinary query/batch main-thread slice above 5 ms on the named reference map"
 * (docs/in-game-agent-plan.md, E16). It is a measurement threshold only; nothing aborts a slow
 * request, and a synchronous command may exceed it.
 */
constexpr uint64_t INLINE_BUDGET_US = 5000;
/**
 * Budget for one deferred job on the draw tick (a capture or a camera operation), in microseconds:
 * one 60 Hz frame. The plan measures screenshots separately and bounds them by size, so this is not
 * the 5 ms query target; it is a measurement threshold only.
 */
constexpr uint64_t DEFERRED_BUDGET_US = 16000;
/** Delay before retrying a failed result delivery. */
constexpr auto RESULT_RETRY = std::chrono::seconds(1);
/** Maximum number of results kept while the backend is unreachable. */
constexpr size_t MAX_PENDING_RESULTS = 256;
/** Maximum number of request receipts kept for idempotent replay. */
constexpr size_t MAX_RECEIPTS = 512;
/** Default page size of world.query. */
constexpr int DEFAULT_QUERY_LIMIT = 50;
/** Maximum page size of world.query. */
constexpr int MAX_QUERY_LIMIT = 500;
/** Maximum number of tiles a single world.query may cover. */
constexpr int MAX_QUERY_TILES = 4096;
/** Maximum number of references a single world.objects or expected_refs may carry. */
constexpr size_t MAX_OBJECT_REFS = 64;

/**
 * Maximum number of requests that may wait for the next draw tick. A capture is
 * taken and the camera is read or moved on the draw thread, so a request that is
 * scheduled but not drawn yet holds a receipt and a result slot; the bound keeps
 * that reservation small.
 */
constexpr size_t MAX_PENDING_DEFERRED = 4;

/**
 * Generate a random (version 4) UUID.
 * @return The UUID as a lower case string.
 */
std::string GenerateUuid()
{
	static std::mt19937_64 rng{std::random_device{}()};
	uint64_t hi = rng();
	uint64_t lo = rng();

	hi = (hi & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;
	lo = (lo & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;

	return fmt::format("{:08x}-{:04x}-{:04x}-{:04x}-{:012x}",
			static_cast<uint32_t>(hi >> 32), static_cast<uint16_t>(hi >> 16), static_cast<uint16_t>(hi),
			static_cast<uint16_t>(lo >> 48), static_cast<uint64_t>(lo & 0xFFFFFFFFFFFFULL));
}

/**
 * Lower case an ASCII string.
 * @param text The string to lower case.
 * @return The lower cased string.
 */
std::string LowerAscii(std::string_view text)
{
	std::string result(text);
	for (char &c : result) {
		if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
	}
	return result;
}

/**
 * Check whether a string contains another string, ignoring ASCII case.
 * @param haystack The string to search in.
 * @param needle The lower cased string to search for.
 * @return True iff \a haystack contains \a needle.
 */
bool ContainsAsciiCI(std::string_view haystack, std::string_view needle)
{
	if (needle.empty()) return true;
	const std::string lowered = LowerAscii(haystack);
	return lowered.find(needle) != std::string::npos;
}

/**
 * Read an integer from a JSON object, tolerating floats and missing keys.
 * @param object The object to read from.
 * @param key The key to read.
 * @param fallback The value to return when the key is missing or not a number.
 * @return The value of the key, or \a fallback.
 */
int JsonInt(const Json &object, const char *key, int fallback)
{
	if (!object.is_object() || !object.contains(key)) return fallback;
	const Json &value = object[key];
	if (value.is_number_integer()) return value.get<int>();
	if (value.is_number_float()) return static_cast<int>(value.get<double>());
	return fallback;
}

/**
 * Read a string from a JSON object, tolerating missing keys and other types.
 * @param object The object to read from.
 * @param key The key to read.
 * @param fallback The value to return when the key is missing or not a string.
 * @return The value of the key, or \a fallback.
 */
std::string JsonString(const Json &object, const char *key, std::string_view fallback = {})
{
	if (!object.is_object() || !object.contains(key)) return std::string(fallback);
	const Json &value = object[key];
	if (!value.is_string()) return std::string(fallback);
	return value.get<std::string>();
}

/** A finished HTTP request, waiting to be handled on the main thread. */
struct Completion {
	uint64_t id;      ///< Identifier of the request.
	bool ok;          ///< Whether the request completed without a transport error.
	std::string body; ///< The received body.
};

std::vector<Completion> _completions; ///< Completions waiting for the main thread.

/**
 * HTTP callback that collects the response body and hands it to the main thread.
 *
 * The callbacks are invoked from NetworkHTTPSocketHandler::HTTPReceive(), which
 * runs on the main thread, so no locking is needed here.
 */
class BridgeHTTPCallback : public HTTPCallback {
public:
	/**
	 * Create the callback.
	 * @param id Identifier of the request this callback belongs to.
	 */
	explicit BridgeHTTPCallback(uint64_t id) : id(id) {}

	void OnFailure() override
	{
		_completions.push_back({this->id, false, std::string{}});
	}

	void OnReceiveData(std::unique_ptr<char[]> data, size_t length) override
	{
		if (data == nullptr) {
			_completions.push_back({this->id, true, std::move(this->body)});
			this->body.clear();
			return;
		}
		this->body.append(data.get(), length);
	}

	bool IsCancelled() const override { return false; }

private:
	uint64_t id;      ///< Identifier of the request this callback belongs to.
	std::string body; ///< Body received so far.
};

/** The kind of request that is in flight. */
enum class RequestKind {
	Hello, ///< POST /v1/engine/hello
	Poll,  ///< POST /v1/engine/poll
	Result,///< POST /v1/engine/result
	Ui,    ///< A request made by the native UI.
};

/** Bookkeeping for a request that is in flight. */
struct PendingRequest {
	RequestKind kind;                                ///< The kind of request.
	std::string epoch;                               ///< The epoch the request was issued for.
	std::function<void(bool, const Json &)> callback;///< Callback for UI requests.
};

/** A stored result of an executed request, kept so that a replay is answered identically. */
struct Receipt {
	Json result;               ///< The result of the request.
	bool acknowledged = false; ///< Whether the backend acknowledged delivery of the result.
};

/**
 * Measured duration of one class of game-thread work.
 *
 * The numbers are the time the bridge itself spent inside the execution, on the game thread, and
 * not the time a request waited for its answer: the HTTP round trip and the poll cadence are
 * deliberately outside of them.
 */
struct TimingStats {
	uint64_t count = 0;    ///< Number of measured executions.
	uint64_t last = 0;     ///< Duration of the last execution, in microseconds.
	uint64_t max = 0;      ///< Longest execution since the epoch started, in microseconds.
	uint64_t total = 0;    ///< Sum of all measured executions, in microseconds.
	uint64_t overruns = 0; ///< Executions that took longer than the budget of their class.
};

/** All state of the bridge. */
struct BridgeState {
	bool initialised = false; ///< Whether the runtime configuration has been read.
	bool enabled = false;     ///< Whether a backend is configured.
	std::string base_url;     ///< Base URL of the backend, without a trailing slash.
	std::string engine_token; ///< Token for the engine routes.
	std::string ui_token;     ///< Token for the chat routes.
	std::string engine_id;    ///< Stable identity of this engine.

	std::string world_id;      ///< Identity of the world, persisted with the savegame.
	std::string epoch;         ///< Identity of this load of the world.
	std::string connection_id; ///< Connection handed out by the backend.
	bool connected = false;    ///< Whether the backend accepted our hello.
	int heartbeat_seconds = 5; ///< Liveness interval requested by the backend; unrelated to the poll cadence.
	int64_t cursor = 0;        ///< Highest request sequence we have received.

	bool hello_in_flight = false;  ///< Whether a hello is in flight.
	bool poll_in_flight = false;   ///< Whether a poll is in flight.
	bool result_in_flight = false; ///< Whether a result delivery is in flight.
	Clock::time_point next_hello{};  ///< Earliest time to send the next hello.
	Clock::time_point next_poll{};   ///< Earliest time to send the next poll.
	Clock::time_point next_result{}; ///< Earliest time to send the next result.

	std::deque<Json> results;              ///< Results waiting to be delivered.
	Json inflight_result;                  ///< Result currently being delivered.
	std::map<std::string, Receipt> receipts; ///< Results of executed requests, by request id.
	std::deque<std::string> receipt_order; ///< Order in which receipts were stored.
	std::set<std::string> barrier_tasks;   ///< Tasks whose barrier has been acknowledged.

	uint32_t pause_revision_at_epoch = 0; ///< Pause revision the current epoch started with.
	TimingStats query_timing;             ///< Inline reads answered on the game thread.
	TimingStats command_timing;           ///< Inline writes executed on the game thread.
	TimingStats batch_timing;             ///< One poll's request batch on the game thread.
	TimingStats deferred_timing;          ///< Capture and camera jobs run on the draw tick.
};

BridgeState _state;                                        ///< The bridge state.
uint64_t _next_request_id = 0;                             ///< Identifier of the next request.
std::map<uint64_t, PendingRequest> _pending;               ///< Requests that are in flight.
std::map<uint64_t, std::unique_ptr<BridgeHTTPCallback>> _callbacks; ///< Callbacks of the requests in flight.

/**
 * Record one measured execution on the game thread.
 * @param stats The class of work the execution belongs to.
 * @param micros How long the execution took, in microseconds.
 * @param budget The budget of that class, in microseconds.
 */
void RecordExecution(TimingStats &stats, uint64_t micros, uint64_t budget)
{
	stats.count++;
	stats.last = micros;
	stats.max = std::max(stats.max, micros);
	stats.total += micros;
	if (micros > budget) stats.overruns++;
}

/**
 * Describe one measured class of work.
 * @param stats The measured class.
 * @param budget The budget of that class, in microseconds.
 * @return The read-only description of the class.
 */
Json TimingJson(const TimingStats &stats, uint64_t budget)
{
	return {
		{"count", stats.count},
		{"last", stats.last},
		{"max", stats.max},
		{"total", stats.total},
		{"average", stats.count == 0 ? 0 : stats.total / stats.count},
		{"overruns", stats.overruns},
		{"budget", budget},
	};
}

/** A request that waits for the next draw tick to be executed. */
struct DeferredRequest {
	std::string method;   ///< The method the request calls.
	std::string world_id; ///< The world the request was issued for.
	std::string epoch;    ///< The epoch the request was issued for.
	std::string task_id;  ///< The task the request belongs to, if any.
	Json params;          ///< The parameters of the request.
};

std::map<std::string, DeferredRequest> _deferred; ///< Requests waiting for the draw tick, by request id.

/**
 * Build a successful method result.
 * @param value The value of the result.
 * @return The result.
 */
Json Ok(Json value)
{
	return {{"ok", true}, {"value", std::move(value)}};
}

/**
 * Build a failed method result.
 * @param code The machine readable error code.
 * @param message Optional human readable message.
 * @return The result.
 */
Json Error(std::string_view code, std::string_view message = {})
{
	Json result = {{"ok", false}, {"error", std::string(code)}};
	if (!message.empty()) result["message"] = std::string(message);
	return result;
}

/**
 * Map a tool module result onto the bridge result shape.
 *
 * The tool modules report `{ok, ...}` with their own diagnostics: the actual cost, the created
 * ids, the engine's message id, partial receipts and the gap detail. A success becomes
 * `{ok: true, value: <module result>}`, which is what the backend unwraps, so nothing is lost. A
 * failure keeps the module's error code and diagnostics at the top level, which is the bridge's
 * error shape, so a gap or a rejected command is never reported as a success.
 * @param result The result of the tool module.
 * @return The bridge result.
 */
Json FromModule(Json result)
{
	if (!result.is_object()) return Ok(std::move(result));

	const auto ok = result.find("ok");
	if (ok != result.end() && ok->is_boolean() && ok->get<bool>()) return Ok(std::move(result));

	if (!result.contains("error") || !result["error"].is_string()) result["error"] = "tool_failed";
	if (!result.contains("message") || !result["message"].is_string()) result["message"] = "the tool module reported a failure";
	return result;
}

/**
 * Check whether the world may be read or changed right now.
 *
 * While a world is generated or a savegame is loaded, the map and the pools are
 * owned by the progress thread and are not consistent yet. The bridge must not
 * query or mutate them then, and must not advertise a world that is not usable.
 * @return True iff a loaded world exists and no modal progress is running.
 */
bool IsWorldReady()
{
	if (_game_mode != GameMode::Normal && _game_mode != GameMode::Editor) return false;
	if (_generating_world) return false;
	if (HasModalProgress()) return false;
	return true;
}

/**
 * Resolve the identity of the current world, if it is not known yet.
 *
 * The savegame id is persisted with the savegame, so it is a stable identity
 * for the world across loads.
 */
void ResolveWorldId()
{
	if (!_state.world_id.empty()) return;
	_state.world_id = _game_session_stats.savegame_id;
	if (_state.world_id.empty()) _state.world_id = GenerateUuid();
}

/**
 * Read a credential from an environment variable, or from the file named by
 * the matching `*_FILE` variable.
 *
 * The file variant keeps the secret out of the process environment, which is
 * world readable on most systems. The value is never logged.
 * @param env_name Name of the environment variable holding the credential.
 * @param file_env_name Name of the environment variable holding a file name.
 * @param credential Receives the credential.
 * @return True iff a non-empty credential was found.
 */
bool ReadCredential(const char *env_name, const char *file_env_name, std::string &credential)
{
	const char *value = std::getenv(env_name);
	if (value != nullptr && *value != '\0') {
		credential = value;
		return true;
	}

	const char *path = std::getenv(file_env_name);
	if (path == nullptr || *path == '\0') return false;

	std::ifstream file(path, std::ios::binary);
	if (!file) {
		Debug(Facility::Net, Severity::Warning, "In-game agent bridge: cannot read the file named by {}", file_env_name);
		return false;
	}

	std::string contents(4096, '\0');
	file.read(contents.data(), static_cast<std::streamsize>(contents.size()));
	contents.resize(static_cast<size_t>(file.gcount()));

	/* A credential file usually ends with a newline; strip the surrounding whitespace. */
	const size_t begin = contents.find_first_not_of(" \t\r\n");
	if (begin == std::string::npos) return false;
	const size_t end = contents.find_last_not_of(" \t\r\n");
	credential = contents.substr(begin, end - begin + 1);
	return !credential.empty();
}

/**
 * Read the runtime configuration, once.
 * @return True iff a backend is configured.
 */
bool EnsureInitialised()
{
	if (_state.initialised) return _state.enabled;
	_state.initialised = true;

	const char *url = std::getenv("AGENT_BACKEND_URL");
	if (url == nullptr || *url == '\0') {
		Debug(Facility::Net, Severity::Info, "In-game agent bridge: AGENT_BACKEND_URL is not set; bridge disabled");
		return false;
	}

	_state.base_url = url;
	while (!_state.base_url.empty() && _state.base_url.back() == '/') _state.base_url.pop_back();

	/* A configured backend is only talked to with credentials; never send an unauthenticated request. */
	if (!ReadCredential("AGENT_ENGINE_TOKEN", "AGENT_ENGINE_TOKEN_FILE", _state.engine_token) ||
			!ReadCredential("AGENT_UI_TOKEN", "AGENT_UI_TOKEN_FILE", _state.ui_token)) {
		Debug(Facility::Net, Severity::Warning, "In-game agent bridge: AGENT_ENGINE_TOKEN(_FILE) and AGENT_UI_TOKEN(_FILE) are required; bridge disabled");
		return false;
	}

	const char *engine_id = std::getenv("AGENT_ENGINE_ID");
	_state.engine_id = (engine_id != nullptr && *engine_id != '\0') ? engine_id : GenerateUuid();

	if (_state.epoch.empty()) _state.epoch = GenerateUuid();

	_state.enabled = true;
	_state.next_hello = Clock::now();
	Debug(Facility::Net, Severity::Info, "In-game agent bridge: backend {} configured", _state.base_url);
	return true;
}

/**
 * Send a request to the backend.
 * @param kind The kind of request.
 * @param path The path below the backend base URL.
 * @param payload The body of the request; empty for a GET.
 * @param token The token to authenticate with.
 * @param epoch The epoch the request is issued for.
 * @param callback Optional callback for UI requests.
 */
void Send(RequestKind kind, const std::string &path, std::string payload, const std::string &token, const std::string &epoch, std::function<void(bool, const Json &)> callback)
{
	const uint64_t id = ++_next_request_id;
	_callbacks[id] = std::make_unique<BridgeHTTPCallback>(id);
	_pending[id] = PendingRequest{kind, epoch, std::move(callback)};

	HTTPHeaders headers;
	headers.emplace_back("Authorization", "Bearer " + token);

	NetworkHTTPSocketHandler::Connect(_state.base_url + path, _callbacks[id].get(), std::move(payload), std::move(headers));
}

/**
 * Check whether another receipt can be stored.
 *
 * A receipt may only be forgotten once the backend acknowledged the delivery of
 * its result; otherwise a replay of that request would execute it a second time.
 * A request that waits for the draw tick already holds a receipt slot, so the
 * pending requests are counted here: an ordinary request must not take the slot
 * a deferred request was promised, or its result could never be stored.
 * @return True iff there is room for another receipt.
 */
bool HasReceiptCapacity()
{
	if (_state.receipts.size() + _deferred.size() < MAX_RECEIPTS) return true;
	for (const std::string &request_id : _state.receipt_order) {
		const auto receipt = _state.receipts.find(request_id);
		if (receipt != _state.receipts.end() && receipt->second.acknowledged) return true;
	}
	return false;
}

/**
 * Forget the oldest acknowledged receipt, to make room for a new one.
 *
 * Only acknowledged receipts are evicted: an unacknowledged one is the only
 * answer a replay of its request can get, so dropping it would execute the
 * request a second time. A pending request holds a receipt slot as well, so the
 * room is made for both.
 */
void MakeReceiptRoom()
{
	while (_state.receipts.size() + _deferred.size() >= MAX_RECEIPTS) {
		const auto oldest = std::find_if(_state.receipt_order.begin(), _state.receipt_order.end(), [](const std::string &request_id) {
			const auto receipt = _state.receipts.find(request_id);
			return receipt != _state.receipts.end() && receipt->second.acknowledged;
		});
		if (oldest == _state.receipt_order.end()) return;
		_state.receipts.erase(*oldest);
		_state.receipt_order.erase(oldest);
	}
}

/**
 * Store the result of an executed request, so a replay is answered identically.
 * @param request_id The request the result belongs to.
 * @param result The result of the request.
 */
void StoreReceipt(const std::string &request_id, const Json &result)
{
	MakeReceiptRoom();
	_state.receipts[request_id] = Receipt{result, false};
	_state.receipt_order.push_back(request_id);
}

/**
 * Mark the receipt of a delivered result as acknowledged by the backend.
 * @param request_id The request whose result was delivered.
 */
void AcknowledgeReceipt(const std::string &request_id)
{
	const auto receipt = _state.receipts.find(request_id);
	if (receipt != _state.receipts.end()) receipt->second.acknowledged = true;
}

/**
 * Queue a result for delivery to the backend.
 * @param request_id The request the result belongs to.
 * @param result The result of the request.
 * @return False iff the queue is full; the caller has to retry later.
 */
bool QueueResult(const std::string &request_id, const Json &result)
{
	/*
	 * A request that waits for the draw tick already holds a result slot. Counting
	 * the pending requests here keeps that slot free: an ordinary result may not
	 * consume it, because the deferred result is produced later and cannot be
	 * replayed from the request.
	 */
	if (_state.results.size() + _deferred.size() >= MAX_PENDING_RESULTS) {
		Debug(Facility::Net, Severity::Warning, "In-game agent bridge: result queue is full; the backend has to catch up");
		return false;
	}
	_state.results.push_back({{"request_id", request_id}, {"result", result}});
	return true;
}

/** The typed category of an object a tile carries, as the protocol names it. */
enum class TileObjectKind : uint8_t {
	Terrain,   ///< Clear landscape: grass, rough, rocks, fields, snow or desert.
	Water,     ///< Water, coast, river, canal or lock.
	Tree,      ///< Trees on the tile.
	Rail,      ///< Plain rail track.
	Signal,    ///< A signal on a rail tile.
	Road,      ///< Road.
	Tram,      ///< Tram track.
	Bridge,    ///< A bridge head or ramp tile.
	Tunnel,    ///< A tunnel portal tile.
	MapObject, ///< A NewGRF map object.
	House,     ///< A house of a town.
};

/**
 * Get the protocol name of a typed tile object category.
 * @param kind The category.
 * @return The name used in references and rows.
 */
std::string_view TileObjectKindName(TileObjectKind kind)
{
	switch (kind) {
		case TileObjectKind::Terrain: return "terrain";
		case TileObjectKind::Water: return "water";
		case TileObjectKind::Tree: return "tree";
		case TileObjectKind::Rail: return "rail";
		case TileObjectKind::Signal: return "signal";
		case TileObjectKind::Road: return "road";
		case TileObjectKind::Tram: return "tram";
		case TileObjectKind::Bridge: return "bridge";
		case TileObjectKind::Tunnel: return "tunnel";
		case TileObjectKind::MapObject: return "mapobject";
		case TileObjectKind::House: return "house";
	}
	return "unknown";
}

/**
 * Map a protocol name onto a typed tile object category.
 *
 * The plural and legacy spellings the protocol already used are accepted, so a reference built
 * from an older answer still resolves.
 * @param name The requested name.
 * @return The category, or nothing when the name is not a typed tile category.
 */
std::optional<TileObjectKind> TileObjectKindFromName(std::string_view name)
{
	if (name == "terrain" || name == "landscape" || name == "clear") return TileObjectKind::Terrain;
	if (name == "water") return TileObjectKind::Water;
	if (name == "tree" || name == "trees") return TileObjectKind::Tree;
	if (name == "rail" || name == "railway" || name == "track") return TileObjectKind::Rail;
	if (name == "signal" || name == "signals") return TileObjectKind::Signal;
	if (name == "road") return TileObjectKind::Road;
	if (name == "tram") return TileObjectKind::Tram;
	if (name == "bridge") return TileObjectKind::Bridge;
	if (name == "tunnel") return TileObjectKind::Tunnel;
	if (name == "mapobject" || name == "map_object" || name == "object") return TileObjectKind::MapObject;
	if (name == "house") return TileObjectKind::House;
	return std::nullopt;
}

/** Start value of the FNV-1a fingerprint of a tile object. */
constexpr uint32_t FINGERPRINT_SEED = 2166136261u;

/**
 * Mix one native value into a state fingerprint.
 *
 * The fingerprint is FNV-1a over the values the engine stores for the object, not over the bytes
 * of the map array and not over an address, so it is stable across runs and builds for the same
 * state and changes when the state the object is drawn from changes.
 * @param hash The fingerprint so far.
 * @param value The value to mix in.
 */
void MixFingerprint(uint32_t &hash, uint32_t value)
{
	hash = (hash ^ value) * 16777619u;
}

/**
 * Mix a 64 bit lifetime into the fingerprint, both halves.
 *
 * Pool generations are 64 bit and only their low half would collide after 2^32 lifetimes, so both
 * halves are mixed in.
 * @param hash The fingerprint so far.
 * @param value The value to mix in.
 */
void MixFingerprint64(uint32_t &hash, uint64_t value)
{
	MixFingerprint(hash, static_cast<uint32_t>(value & 0xFFFFFFFFu));
	MixFingerprint(hash, static_cast<uint32_t>(value >> 32));
}

/**
 * Readable name of a clear-land subtype.
 * @param ground The clear ground.
 * @return The protocol name.
 */
std::string_view ClearGroundName(ClearGround ground)
{
	switch (ground) {
		case ClearGround::Grass: return "grass";
		case ClearGround::Rough: return "rough";
		case ClearGround::Rocks: return "rocks";
		case ClearGround::Fields: return "fields";
		case ClearGround::Desert: return "desert";
		default: return "clear";
	}
}

/**
 * Readable name of a water subtype.
 * @param tile The water tile.
 * @return The protocol name.
 */
std::string_view WaterSubtypeName(TileIndex tile)
{
	if (IsCoastTile(tile)) return "coast";
	if (IsCanal(tile)) return "canal";
	if (IsRiver(tile)) return "river";
	switch (GetWaterTileType(tile)) {
		case WaterTileType::Lock: return "lock";
		case WaterTileType::Depot: return "depot";
		case WaterTileType::ClearRocks: return "clear_rocks";
		case WaterTileType::CoastRocks: return "coast_rocks";
		default: return "water";
	}
}

/**
 * Readable name of the ground trees stand on.
 * @param ground The tree ground.
 * @return The protocol name.
 */
std::string_view TreeGroundName(TreeGround ground)
{
	switch (ground) {
		case TreeGround::Grass: return "grass";
		case TreeGround::Rough: return "rough";
		case TreeGround::SnowOrDesert: return "snow_or_desert";
		case TreeGround::Shore: return "shore";
		case TreeGround::RoughSnow: return "rough_snow";
		default: return "unknown";
	}
}

/**
 * Readable name of the track bits of a plain rail tile.
 * @param bits The track bits.
 * @return The protocol name.
 */
std::string_view TrackBitsName(TrackBits bits)
{
	if (bits == TrackBits{Track::X}) return "x";
	if (bits == TrackBits{Track::Y}) return "y";
	if (bits == TrackBits{Track::Upper}) return "upper";
	if (bits == TrackBits{Track::Lower}) return "lower";
	if (bits == TrackBits{Track::Left}) return "left";
	if (bits == TrackBits{Track::Right}) return "right";
	if (bits == TRACK_BIT_CROSS) return "cross";
	if (bits == TRACK_BIT_HORZ) return "horizontal";
	if (bits == TRACK_BIT_VERT) return "vertical";
	if (bits == TRACK_BIT_ALL) return "all";
	return "mixed";
}

/**
 * Readable name of a transport type, as used by a bridge or a tunnel.
 * @param type The transport type.
 * @return The protocol name.
 */
std::string_view TransportTypeName(TransportType type)
{
	switch (type) {
		case TransportType::Rail: return "rail";
		case TransportType::Road: return "road";
		case TransportType::Water: return "water";
		case TransportType::Air: return "air";
		case TransportType::Invalid: return "invalid";
	}
	return "unknown";
}

/**
 * The owner a tile object reports, if the engine records one for it.
 *
 * Road and tram are reported with the owner of their own transport type, because the road and the
 * tram on one tile can belong to different companies; water and map objects report the tile owner
 * the engine stores for them (a canal, lock or ship depot has one, open sea does not). Houses,
 * trees and terrain carry no company owner, so they report none instead of a made up company.
 * @param tile The tile.
 * @param kind The category of the object.
 * @return The owner, or INVALID_OWNER when the category has none.
 */
Owner TileObjectOwner(TileIndex tile, TileObjectKind kind)
{
	switch (kind) {
		case TileObjectKind::Rail:
		case TileObjectKind::Signal:
		case TileObjectKind::Bridge:
		case TileObjectKind::Tunnel:
		case TileObjectKind::Water:
		case TileObjectKind::MapObject: return GetTileOwner(tile);
		case TileObjectKind::Road: return GetRoadOwner(tile, RoadTramType::Road);
		case TileObjectKind::Tram: return GetRoadOwner(tile, RoadTramType::Tram);
		default: return INVALID_OWNER;
	}
}

/**
 * Compute the state fingerprint of a typed tile object.
 *
 * Only the native values that identify the object and its state are mixed in, per category:
 * - every category: the tile type and the tile height;
 * - every category the engine records an owner for: that owner (road and tram use the owner of
 *   their own transport type, water and map objects the tile owner), because an ownership change
 *   is part of the precondition a caller observed;
 * - terrain: clear ground, clear density and slope;
 * - water: water tile type and water class (the coast, canal and river flags are part of it);
 * - tree: ground, density and tree type;
 * - rail: rail tile type, rail type and, for plain rail, the track bits;
 * - signal: the signal variant of every track, the present signals and the signal states (traffic
 *   can change these, so a signal reference may go stale without anyone building anything);
 * - road and tram: the road type and the road bits of that tram type;
 * - bridge and tunnel: transport type and direction, plus the bridge type for a bridge;
 * - mapobject: the object type, the pooled object id and the full 64 bit generation of that
 *   object, so replacing an object with one of the same type is a different identity;
 * - house: the house type, its random bits, the town it belongs to and the full 64 bit generation
 *   of that town. A house has no pool generation of its own: a same-type reconstruction on the
 *   same tile in the same town with the same random bits is not distinguishable, and the row says
 *   so by carrying no house generation.
 * @param kind The category of the object.
 * @param tile The tile.
 * @return The fingerprint.
 */
uint32_t TileObjectFingerprint(TileObjectKind kind, TileIndex tile)
{
	uint32_t hash = FINGERPRINT_SEED;
	MixFingerprint(hash, to_underlying(GetTileType(tile)));
	MixFingerprint(hash, TileHeight(tile));

	/* Ownership is part of the observed precondition, not only the type, height and bits. */
	switch (kind) {
		case TileObjectKind::Rail:
		case TileObjectKind::Signal:
		case TileObjectKind::Bridge:
		case TileObjectKind::Tunnel:
		case TileObjectKind::Water:
		case TileObjectKind::MapObject:
			MixFingerprint(hash, GetTileOwner(tile).base());
			break;

		case TileObjectKind::Road:
			/* The road and the tram on one tile can belong to different companies. */
			MixFingerprint(hash, GetTileOwner(tile).base());
			MixFingerprint(hash, GetRoadOwner(tile, RoadTramType::Road).base());
			break;

		case TileObjectKind::Tram:
			MixFingerprint(hash, GetTileOwner(tile).base());
			MixFingerprint(hash, GetRoadOwner(tile, RoadTramType::Tram).base());
			break;

		default:
			/* Terrain, trees and houses carry no company owner on the tile. */
			break;
	}

	switch (kind) {
		case TileObjectKind::Terrain:
			MixFingerprint(hash, to_underlying(GetClearGround(tile)));
			MixFingerprint(hash, GetClearDensity(tile));
			MixFingerprint(hash, to_underlying(GetTileSlope(tile)));
			break;

		case TileObjectKind::Water:
			MixFingerprint(hash, to_underlying(GetWaterTileType(tile)));
			MixFingerprint(hash, to_underlying(GetWaterClass(tile)));
			break;

		case TileObjectKind::Tree:
			MixFingerprint(hash, to_underlying(GetTreeGround(tile)));
			MixFingerprint(hash, GetTreeDensity(tile));
			MixFingerprint(hash, to_underlying(GetTreeType(tile)));
			break;

		case TileObjectKind::Rail: {
			const RailTileType rail_type = GetRailTileType(tile);
			MixFingerprint(hash, to_underlying(rail_type));
			MixFingerprint(hash, to_underlying(GetRailType(tile)));
			if (rail_type == RailTileType::Normal) MixFingerprint(hash, GetTrackBits(tile).base());
			break;
		}

		case TileObjectKind::Signal:
			for (const Track track : {Track::X, Track::Y, Track::Upper, Track::Lower, Track::Left, Track::Right}) {
				MixFingerprint(hash, to_underlying(GetSignalVariant(tile, track)));
			}
			MixFingerprint(hash, GetPresentSignals(tile));
			MixFingerprint(hash, GetSignalStates(tile));
			break;

		case TileObjectKind::Road:
		case TileObjectKind::Tram: {
			const RoadTramType tram_type = kind == TileObjectKind::Tram ? RoadTramType::Tram : RoadTramType::Road;
			MixFingerprint(hash, to_underlying(GetRoadType(tile, tram_type)));
			MixFingerprint(hash, GetRoadBits(tile, tram_type).base());
			break;
		}

		case TileObjectKind::Bridge:
		case TileObjectKind::Tunnel:
			MixFingerprint(hash, to_underlying(GetTunnelBridgeTransportType(tile)));
			MixFingerprint(hash, to_underlying(GetTunnelBridgeDirection(tile)));
			if (kind == TileObjectKind::Bridge) MixFingerprint(hash, GetBridgeType(tile));
			break;

		case TileObjectKind::MapObject: {
			const Object *object = Object::GetByTile(tile);
			/* ObjectType is a plain integer type, not an enum, so it is cast, not unwrapped. */
			MixFingerprint(hash, object == nullptr ? static_cast<uint32_t>(INVALID_OBJECT_TYPE) : static_cast<uint32_t>(object->type));
			MixFingerprint(hash, object == nullptr ? 0 : object->index.base());
			MixFingerprint64(hash, object == nullptr ? 0 : object->generation);
			break;
		}

		case TileObjectKind::House: {
			/* Both the stored id and its translation: a NewGRF mapping can translate two different
			 * stored ids to the same HouseID, so the translated type alone misses the raw identity. */
			MixFingerprint(hash, GetCleanHouseType(tile));
			MixFingerprint(hash, GetHouseType(tile));
			MixFingerprint(hash, GetHouseRandomBits(tile));
			const Town *town = Town::GetIfValid(GetTownIndex(tile));
			MixFingerprint(hash, town == nullptr ? 0 : town->index.base());
			MixFingerprint64(hash, town == nullptr ? 0 : town->generation);
			break;
		}
	}

	return hash;
}

/**
 * Build the row of one typed tile object.
 *
 * The identity of a tile object is the tile and its state fingerprint. A house also carries the
 * lifetime of its town, and a map object the lifetime of the pooled object behind it, because those
 * are part of the identity and neither is a house's own pool generation.
 * @param tile The tile.
 * @param kind The category.
 * @param subtype The readable subtype.
 * @return The row: type, what, subtype, identity, tile, owner, fingerprint (+ town_* / object_*).
 */
Json TileObjectRow(TileIndex tile, TileObjectKind kind, std::string_view subtype)
{
	const Owner owner = TileObjectOwner(tile, kind);
	Json row = {
		{"type", std::string(TileObjectKindName(kind))},
		{"what", std::string(TileObjectKindName(kind))},
		{"subtype", std::string(subtype)},
		/* The identity of this object is the tile and its fingerprint, not a pool index. */
		{"identity", "tile"},
		{"tile", Json{{"x", TileX(tile)}, {"y", TileY(tile)}}},
		/* OWNER_WATER is what the engine stores for open sea, not a company; report no owner. */
		{"owner", (owner == INVALID_OWNER || owner == OWNER_NONE || owner == OWNER_WATER) ? Json(nullptr) : Json(owner.base())},
		{"fingerprint", TileObjectFingerprint(kind, tile)},
	};

	/* A house belongs to a town; the town's lifetime is part of the house's identity. */
	if (kind == TileObjectKind::House) {
		const Town *town = Town::GetIfValid(GetTownIndex(tile));
		row["town_id"] = town == nullptr ? Json(nullptr) : Json(town->index.base());
		row["town_generation"] = town == nullptr ? Json(nullptr) : Json(town->generation);
	}

	/* A map object is a pooled object: its id and full generation are part of the identity, so a
	 * same-type replacement is not mistaken for the object the caller planned for. */
	if (kind == TileObjectKind::MapObject) {
		const Object *object = Object::GetByTile(tile);
		row["object_id"] = object == nullptr ? Json(nullptr) : Json(object->index.base());
		row["object_generation"] = object == nullptr ? Json(nullptr) : Json(object->generation);
	}

	return row;
}

/**
 * Describe the typed objects the tile itself carries, most specific first.
 *
 * Only the requested tile is read, so this is bounded per call and never scans the map. A rail
 * tile with signals carries a `signal` object besides its `rail` object; a tile that carries both
 * road and tram carries both objects. A station, industry or void tile carries no typed landscape
 * object of its own: the pooled objects of `PickObjects()` describe it.
 * @param tile The tile.
 * @return Array of typed tile object rows.
 */
Json BuildTileObjectRows(TileIndex tile)
{
	Json objects = Json::array();
	if (!IsValidTile(tile)) return objects;

	switch (GetTileType(tile)) {
		case TileType::Clear:
			objects.push_back(TileObjectRow(tile, TileObjectKind::Terrain, ClearGroundName(GetClearGround(tile))));
			break;

		case TileType::Water:
			objects.push_back(TileObjectRow(tile, TileObjectKind::Water, WaterSubtypeName(tile)));
			break;

		case TileType::Trees:
			objects.push_back(TileObjectRow(tile, TileObjectKind::Tree, TreeGroundName(GetTreeGround(tile))));
			break;

		case TileType::Railway: {
			const RailTileType rail_type = GetRailTileType(tile);
			if (rail_type == RailTileType::Signals) {
				/* A signal is a sub-object of the rail tile, so it is the more specific answer. */
				objects.push_back(TileObjectRow(tile, TileObjectKind::Signal,
						GetSignalVariant(tile, Track::X) == SignalVariant::Semaphore ? "semaphore" : "electric"));
			}
			const std::string_view subtype = rail_type == RailTileType::Normal
					? TrackBitsName(GetTrackBits(tile))
					: (rail_type == RailTileType::Depot ? "depot" : "signals");
			objects.push_back(TileObjectRow(tile, TileObjectKind::Rail, subtype));
			break;
		}

		case TileType::Road:
			if (GetRoadType(tile, RoadTramType::Road) != INVALID_ROADTYPE) {
				objects.push_back(TileObjectRow(tile, TileObjectKind::Road, "road"));
			}
			if (GetRoadType(tile, RoadTramType::Tram) != INVALID_ROADTYPE) {
				objects.push_back(TileObjectRow(tile, TileObjectKind::Tram, "tram"));
			}
			break;

		case TileType::TunnelBridge:
			objects.push_back(TileObjectRow(tile, IsBridgeTile(tile) ? TileObjectKind::Bridge : TileObjectKind::Tunnel,
					TransportTypeName(GetTunnelBridgeTransportType(tile))));
			break;

		case TileType::Object: {
			const Object *object = Object::GetByTile(tile);
			const std::string subtype = object == nullptr ? std::string("object") : fmt::format("type_{}", object->type);
			objects.push_back(TileObjectRow(tile, TileObjectKind::MapObject, subtype));
			break;
		}

		case TileType::House:
			objects.push_back(TileObjectRow(tile, TileObjectKind::House, fmt::format("house_{}", GetHouseType(tile))));
			break;

		default:
			/* Station, industry and void tiles carry no typed landscape object of their own. */
			break;
	}

	return objects;
}

/**
 * Get the name of a tile type, as used in the protocol.
 * @param type The tile type.
 * @return The name of the tile type.
 */
std::string_view TileTypeName(TileType type)
{
	switch (type) {
		case TileType::Clear: return "clear";
		case TileType::Railway: return "rail";
		case TileType::Road: return "road";
		case TileType::House: return "house";
		case TileType::Trees: return "trees";
		case TileType::Station: return "station";
		case TileType::Water: return "water";
		case TileType::Void: return "void";
		case TileType::Industry: return "industry";
		case TileType::TunnelBridge: return "tunnelbridge";
		case TileType::Object: return "object";
		default: return "unknown";
	}
}

/**
 * Describe a single tile.
 * @param tile The tile to describe.
 * @return The description of the tile.
 */
Json TileRow(TileIndex tile)
{
	const TileType type = GetTileType(tile);
	Json row = {
		{"x", TileX(tile)},
		{"y", TileY(tile)},
		{"type", std::string(TileTypeName(type))},
	};

	if (IsValidTile(tile)) {
		row["height"] = TileHeight(tile);
		if (type != TileType::House && type != TileType::Industry) {
			const Owner owner = GetTileOwner(tile);
			if (owner != OWNER_NONE && owner != INVALID_OWNER) row["owner"] = owner.base();
		}
	}

	return row;
}

/**
 * Does this world.project call ask for a screen projection instead of an administrative op?
 *
 * The administrative operations of world.project (`power`, `power_fuel`, `station_town`,
 * `district`) are named by `op` and stay with the admin module. A call without an op that
 * names a tile, a town or a screen pixel is a projection of the live viewport, which is a
 * read of the camera and not an administrative operation.
 * @param params The method parameters.
 * @return True iff the call is a projection request.
 */
bool IsProjectionRequest(const Json &params)
{
	if (!params.is_object() || params.contains("op")) return false;
	for (const char *key : {"tile", "tile_x", "tile_y", "town", "center", "screen_x", "screen_y"}) {
		if (params.contains(key)) return true;
	}
	return false;
}

/**
 * The methods that change the world, and therefore need the write gate.
 *
 * The administrative module knows which of its own operations are read-only
 * (`world.company op=info`, `world.towns op=needs`, `world.project op=power/district`),
 * so it decides for its families; the rest is fixed by the method name. Moving the camera
 * is a UI operation, but it is a write of the viewport state, so it takes the same gate.
 * @param method The method name.
 * @param params The method parameters.
 * @return True when the call is expected to change the world.
 */
bool IsMutation(std::string_view method, const Json &params)
{
	if (method == "world.pause" || method == "world.rename" || method == "world.settings.set" ||
			method == "world.construct" || method == "world.vehicles" || method == "world.orders" ||
			method == "world.command" || method == "world.camera.set") {
		return true;
	}
	/* A projection through world.project is a read of the camera; the administrative ops keep their own answer. */
	if (method == "world.project" && IsProjectionRequest(params)) return false;
	/* The rail blueprint module knows which of its own ops are read-only (status, list, inspect). */
	if (IsBlueprintMutation(method, params)) return true;
	return IsAdminMutation(method, params);
}

/**
 * The world and epoch the bridge is bound to, as included in read results.
 * @return {world_id, epoch}.
 */
Json WorldBinding()
{
	ResolveWorldId();
	return {{"world_id", _state.world_id}, {"epoch", _state.epoch}};
}

/**
 * The parameters of a rail blueprint or trunk line call, with the bridge's own binding forced in.
 *
 * The module refuses a plan of another task, world or epoch, so the binding it reads has to be the
 * one the bridge checked and never what the model echoed back in its parameters: `task_id` is the
 * outer request's task, `world_id` and `epoch` are this load's identity as the bridge reports it.
 * The three values are always strings, so an echoed number cannot turn a valid call into an
 * `invalid_arguments` answer.
 * @param request The request, as received from the backend.
 * @param params The parameters of the request.
 * @return The parameters with the authoritative binding injected.
 */
Json BlueprintModuleParams(const Json &request, const Json &params)
{
	ResolveWorldId();
	Json module_params = params.is_object() ? params : Json::object();
	module_params["task_id"] = JsonString(request, "task_id");
	module_params["world_id"] = _state.world_id;
	module_params["epoch"] = _state.epoch;
	return module_params;
}

/**
 * Implement world.info.
 * @return The result of the method.
 */
Json WorldInfo()
{
	Json companies = Json::array();
	for (const Company *c : Company::Iterate()) {
		/*
		 * A company row is a typed reference of the company itself, with its lifetime, so a write
		 * can guard the company it was planned for without a second lookup.
		 */
		companies.push_back({
			{"type", "company"},
			{"id", c->index.base()},
			{"generation", c->generation},
			{"name", c->name.empty() ? GetString(STR_COMPANY_NAME, c->index) : c->name},
			{"is_ai", c->is_ai},
			{"money", static_cast<int64_t>(c->money)},
		});
	}

	/*
	 * The company the player is acting as and the company the engine is scoped to, exactly as they
	 * are: `null` when there is none (spectator, menu, deity scope), never a made up company 0, so
	 * the backend can bind the acting company without guessing.
	 */
	const auto company_id = [](CompanyID id) -> Json {
		return Company::IsValidID(id) ? Json(id.base()) : Json(nullptr);
	};

	/* The same moment as a human reads it; the raw day counter stays below for compatibility. */
	const TimerGameCalendar::YearMonthDay ymd = TimerGameCalendar::ConvertDateToYMD(TimerGameCalendar::date);

	Json value = {
		{"date", TimerGameCalendar::date.base()},
		/*
		 * A model that is only given the day ordinal answers with a number no player recognises, so
		 * the calendar is reported next to it. `date_iso` is the engine's own ISO rendering, so the
		 * text and the numbers cannot disagree; `month` is 1..12 and `day` 1..31, as a player reads
		 * them, and `year` is the engine's calendar year (the one the status bar shows).
		 */
		{"calendar", {
			{"year", ymd.year.base()},
			{"month", static_cast<int>(ymd.month) + 1},
			{"day", static_cast<int>(ymd.day)},
			{"date_iso", GetString(STR_JUST_DATE_ISO, TimerGameCalendar::date)},
		}},
		{"ticks", TimerGameTick::counter},
		{"paused", _pause_mode.Any()},
		/*
		 * The pause revision moves with every accepted pause decision, whoever made it. It is not
		 * bound to the epoch by itself: an epoch that starts while the pause state is left alone
		 * begins with the revision of the previous epoch, so an equal revision can come from either
		 * side of a load. The world/epoch of the request is what actually separates them; a
		 * revision below `revision_at_epoch_start` is only an additional stale check.
		 */
		{"pause", {
			{"paused", _pause_mode.Any()},
			{"normal", _pause_mode.Test(PauseMode::Normal)},
			{"revision", PauseRevision()},
			{"revision_at_epoch_start", _state.pause_revision_at_epoch},
			{"epoch", _state.epoch},
		}},
		/*
		 * Measured game-thread work, not HTTP latency: one inline request execution for reads and
		 * writes, one draw-tick job for captures and camera operations. `enforced` is false, so the
		 * budgets are thresholds that count overruns and never abort anything.
		 */
		{"timing", {
			{"unit", "microseconds"},
			{"clock", "steady_clock"},
			{"scope", "game thread: one inline request execution, one poll's request batch, or one deferred draw-tick job"},
			{"enforced", false},
			{"since_epoch", _state.epoch},
			{"queries", TimingJson(_state.query_timing, INLINE_BUDGET_US)},
			{"commands", TimingJson(_state.command_timing, INLINE_BUDGET_US)},
			/* The plan's target is a query/batch slice, so the whole batch of one poll is measured too. */
			{"batches", TimingJson(_state.batch_timing, INLINE_BUDGET_US)},
			{"deferred", TimingJson(_state.deferred_timing, DEFERRED_BUDGET_US)},
		}},
		{"local_company_id", company_id(_local_company)},
		{"current_company_id", company_id(_current_company)},
		{"map", {{"width", Map::SizeX()}, {"height", Map::SizeY()}}},
		{"companies", companies},
		{"build", std::string(_openttd_revision)},
	};
	/* The read is bound to the world and epoch it was taken in, like every other read. */
	value.update(WorldBinding());
	return Ok(std::move(value));
}

/**
 * Map a query kind onto its canonical plural name, accepting the legacy
 * singular aliases of the first kernel version.
 * @param kind The kind as requested.
 * @return The canonical kind, or an empty view when the kind is not implemented.
 */
std::string_view CanonicalQueryKind(std::string_view kind)
{
	if (kind == "towns" || kind == "town") return "towns";
	if (kind == "companies" || kind == "company") return "companies";
	if (kind == "stations" || kind == "station") return "stations";
	if (kind == "industries" || kind == "industry") return "industries";
	if (kind == "vehicles" || kind == "vehicle") return "vehicles";
	if (kind == "engines" || kind == "engine") return "engines";
	if (kind == "orders" || kind == "order") return "orders";
	if (kind == "groups" || kind == "group") return "groups";
	if (kind == "cargos" || kind == "cargo") return "cargos";
	if (kind == "signs" || kind == "sign") return "signs";
	if (kind == "tiles" || kind == "tile") return "tiles";
	return {};
}

/** A rectangular tile filter, as used by world.query. */
struct TileRange {
	bool active = false; ///< Whether the filter was given at all.
	int x0 = 0;          ///< First column.
	int y0 = 0;          ///< First row.
	int x1 = 0;          ///< Last column.
	int y1 = 0;          ///< Last row.

	/**
	 * Test whether a tile is inside the range.
	 * @param tile The tile to test.
	 * @return True iff the tile is inside the range, or the filter is inactive.
	 */
	bool Contains(TileIndex tile) const
	{
		if (!this->active) return true;
		const int x = TileX(tile);
		const int y = TileY(tile);
		return x >= this->x0 && x <= this->x1 && y >= this->y0 && y <= this->y1;
	}
};

/**
 * Read the spatial filter of a query.
 * @param filters The filters of the query.
 * @return The tile range; inactive when no bound was given.
 */
TileRange ReadTileRange(const Json &filters)
{
	TileRange range;
	if (!filters.contains("x0") && !filters.contains("y0") && !filters.contains("x1") && !filters.contains("y1")) return range;

	range.active = true;
	range.x0 = Clamp(JsonInt(filters, "x0", 0), 0, static_cast<int>(Map::MaxX()));
	range.y0 = Clamp(JsonInt(filters, "y0", 0), 0, static_cast<int>(Map::MaxY()));
	range.x1 = Clamp(JsonInt(filters, "x1", static_cast<int>(Map::MaxX())), range.x0, static_cast<int>(Map::MaxX()));
	range.y1 = Clamp(JsonInt(filters, "y1", static_cast<int>(Map::MaxY())), range.y0, static_cast<int>(Map::MaxY()));
	return range;
}

/**
 * Read the name filter of a query, accepting the canonical `name_contains` and
 * the legacy `name` alias.
 * @param filters The filters of the query.
 * @return The lower cased needle; empty when no name filter was given.
 */
std::string ReadNameFilter(const Json &filters)
{
	std::string name = JsonString(filters, "name_contains");
	if (name.empty()) name = JsonString(filters, "name");
	return LowerAscii(name);
}

/**
 * Implement world.query for towns, companies, stations, industries, vehicles,
 * signs and tiles.
 *
 * The canonical kind names are plural, as advertised by the backend registry;
 * the singular names of the first kernel version are accepted as aliases. The
 * filters are applied before pagination, so `total` is the number of matching
 * objects and not the number of returned rows.
 * @param params The parameters of the method.
 * @return The result of the method.
 */
Json WorldQuery(const Json &params)
{
	const std::string requested = JsonString(params, "kind");
	const std::string_view kind = CanonicalQueryKind(requested);
	if (kind.empty()) return Error("unsupported_query_kind", requested);

	const Json filters = params.contains("filters") && params["filters"].is_object() ? params["filters"] : Json::object();
	const int limit = Clamp(JsonInt(params, "limit", DEFAULT_QUERY_LIMIT), 1, MAX_QUERY_LIMIT);
	const int offset = std::max(0, JsonInt(params, "offset", 0));

	const std::string name = ReadNameFilter(filters);
	const int id = JsonInt(filters, "id", -1);
	/* The registry advertises company_id both as a filter and as a top level parameter. */
	const int company_id = filters.contains("company_id") ? JsonInt(filters, "company_id", -1) : JsonInt(params, "company_id", -1);
	const int town_id = JsonInt(filters, "town_id", -1);
	const TileRange range = ReadTileRange(filters);

	Json rows = Json::array();
	int total = 0;

	auto add_row = [&](Json row) {
		if (total >= offset && static_cast<int>(rows.size()) < limit) rows.push_back(std::move(row));
		total++;
	};

	if (kind == "tiles") {
		const int x = JsonInt(filters, "x", -1);
		const int y = JsonInt(filters, "y", -1);
		if (x >= 0 && y >= 0) {
			if (x > static_cast<int>(Map::MaxX()) || y > static_cast<int>(Map::MaxY())) return Error("invalid_params", "tile is outside the map");
			add_row(TileRow(TileXY(static_cast<uint>(x), static_cast<uint>(y))));
		} else {
			const int x0 = Clamp(JsonInt(filters, "x0", 0), 0, static_cast<int>(Map::MaxX()));
			const int y0 = Clamp(JsonInt(filters, "y0", 0), 0, static_cast<int>(Map::MaxY()));
			const int x1 = Clamp(JsonInt(filters, "x1", x0), x0, static_cast<int>(Map::MaxX()));
			const int y1 = Clamp(JsonInt(filters, "y1", y0), y0, static_cast<int>(Map::MaxY()));

			const int64_t count = static_cast<int64_t>(x1 - x0 + 1) * static_cast<int64_t>(y1 - y0 + 1);
			if (count > MAX_QUERY_TILES) return Error("range_too_large", "the tile range is too large");

			total = static_cast<int>(count);
			int index = 0;
			for (int ty = y0; ty <= y1 && static_cast<int>(rows.size()) < limit; ty++) {
				for (int tx = x0; tx <= x1 && static_cast<int>(rows.size()) < limit; tx++, index++) {
					if (index < offset) continue;
					rows.push_back(TileRow(TileXY(static_cast<uint>(tx), static_cast<uint>(ty))));
				}
			}
		}
	} else if (kind == "towns") {
		for (const Town *t : Town::Iterate()) {
			if (id >= 0 && static_cast<int>(t->index.base()) != id) continue;
			if (!ContainsAsciiCI(t->GetCachedName(), name)) continue;
			if (!range.Contains(t->xy)) continue;
			add_row({
				{"id", t->index.base()},
				{"generation", t->generation},
				{"name", t->GetCachedName()},
				{"tile", {TileX(t->xy), TileY(t->xy)}},
				{"population", t->cache.population},
			});
		}
	} else if (kind == "companies") {
		for (const Company *c : Company::Iterate()) {
			if (id >= 0 && static_cast<int>(c->index.base()) != id) continue;
			if (company_id >= 0 && static_cast<int>(c->index.base()) != company_id) continue;
			const std::string company_name = c->name.empty() ? GetString(STR_COMPANY_NAME, c->index) : c->name;
			if (!ContainsAsciiCI(company_name, name)) continue;
			add_row({
				{"id", c->index.base()},
				{"generation", c->generation},
				{"name", company_name},
				{"is_ai", c->is_ai},
				{"money", static_cast<int64_t>(c->money)},
			});
		}
	} else if (kind == "stations") {
		for (const Station *st : Station::Iterate()) {
			if (id >= 0 && static_cast<int>(st->index.base()) != id) continue;
			if (company_id >= 0 && static_cast<int>(st->owner.base()) != company_id) continue;
			if (town_id >= 0 && (st->town == nullptr || static_cast<int>(st->town->index.base()) != town_id)) continue;
			if (!ContainsAsciiCI(st->GetCachedName(), name)) continue;
			if (!range.Contains(st->xy)) continue;
			add_row({
				{"id", st->index.base()},
				{"generation", st->generation},
				{"name", st->GetCachedName()},
				{"tile", {TileX(st->xy), TileY(st->xy)}},
				{"owner", st->owner.base()},
				{"town_id", st->town != nullptr ? st->town->index.base() : -1},
			});
		}
	} else if (kind == "industries") {
		for (const Industry *ind : Industry::Iterate()) {
			if (id >= 0 && static_cast<int>(ind->index.base()) != id) continue;
			if (town_id >= 0 && (ind->town == nullptr || static_cast<int>(ind->town->index.base()) != town_id)) continue;
			if (!ContainsAsciiCI(ind->GetCachedName(), name)) continue;
			if (!range.Contains(ind->location.tile)) continue;
			add_row({
				{"id", ind->index.base()},
				{"generation", ind->generation},
				{"name", ind->GetCachedName()},
				{"tile", {TileX(ind->location.tile), TileY(ind->location.tile)}},
				{"town_id", ind->town != nullptr ? ind->town->index.base() : -1},
			});
		}
	} else if (kind == "vehicles") {
		for (const Vehicle *v : Vehicle::Iterate()) {
			if (id >= 0 && static_cast<int>(v->index.base()) != id) continue;
			if (company_id >= 0 && static_cast<int>(v->owner.base()) != company_id) continue;
			if (!ContainsAsciiCI(v->name, name)) continue;
			if (!range.Contains(v->tile)) continue;
			add_row({
				{"id", v->index.base()},
				{"generation", v->generation},
				{"name", v->name},
				{"owner", v->owner.base()},
				{"tile", {TileX(v->tile), TileY(v->tile)}},
			});
		}
	} else if (kind == "signs") {
		for (const Sign *sign : Sign::Iterate()) {
			if (id >= 0 && static_cast<int>(sign->index.base()) != id) continue;
			if (company_id >= 0 && static_cast<int>(sign->owner.base()) != company_id) continue;
			if (!ContainsAsciiCI(sign->name, name)) continue;
			const TileIndex tile = TileVirtXY(static_cast<uint>(std::max(0, sign->x)), static_cast<uint>(std::max(0, sign->y)));
			if (!range.Contains(tile)) continue;
			add_row({
				{"id", sign->index.base()},
				{"generation", sign->generation},
				{"name", sign->name},
				{"owner", sign->owner.base()},
				{"tile", {TileX(tile), TileY(tile)}},
			});
		}
	}

	Json value = {{"rows", rows}, {"total", total}};
	value.update(WorldBinding());
	return Ok(std::move(value));
}

/**
 * Build the resolution of a reference that is not current.
 * @param type The reference type.
 * @param reason Machine readable reason, e.g. `deleted` or `generation_mismatch`.
 * @param error The protocol error code.
 * @return The resolution row.
 */
Json RefFailure(std::string_view type, std::string_view reason, std::string_view error = "stale_ref")
{
	return {{"type", std::string(type)}, {"found", false}, {"error", std::string(error)}, {"reason", std::string(reason)}};
}

/**
 * Read an integer member without narrowing it first.
 *
 * Ids and generations travel as JSON integers. A member of another type, a negative value or a
 * value above \a maximum is reported as unusable instead of being truncated into a pool index:
 * truncation would silently resolve a different object.
 * @param object The object to read from.
 * @param key The member name.
 * @param maximum The largest accepted value.
 * @param value Receives the value.
 * @return True iff the member is present and a usable integer.
 */
bool ReadBoundedInteger(const Json &object, const char *key, int64_t maximum, int64_t &value)
{
	if (!object.is_object() || !object.contains(key)) return false;
	const Json &member = object[key];
	if (member.is_number_unsigned()) {
		const uint64_t raw = member.get<uint64_t>();
		if (raw > static_cast<uint64_t>(maximum)) return false;
		value = static_cast<int64_t>(raw);
		return true;
	}
	if (member.is_number_integer()) {
		const int64_t raw = member.get<int64_t>();
		if (raw < 0 || raw > maximum) return false;
		value = raw;
		return true;
	}
	return false;
}

/**
 * Read the tile location of a reference.
 *
 * The canonical form is the object `{"x": .., "y": ..}`, which is what the protocol schema and
 * this bridge emit. The first kernel version used a two element array `[x, y]`, and a bare point
 * (`type` = `point`) is a tile without an object, so both shapes are accepted. The coordinates are
 * range checked against the map before they are narrowed to a TileIndex.
 * @param ref The reference.
 * @param tile Receives the tile.
 * @return True iff the reference carries a usable tile.
 */
bool ReadRefTile(const Json &ref, TileIndex &tile)
{
	if (!ref.contains("tile")) return false;
	const Json &location = ref["tile"];
	const int64_t max_x = static_cast<int64_t>(Map::MaxX());
	const int64_t max_y = static_cast<int64_t>(Map::MaxY());

	int64_t x = 0;
	int64_t y = 0;
	if (location.is_object()) {
		if (!ReadBoundedInteger(location, "x", max_x, x)) return false;
		if (!ReadBoundedInteger(location, "y", max_y, y)) return false;
	} else if (location.is_array() && location.size() == 2 && location[0].is_number_integer() && location[1].is_number_integer()) {
		x = location[0].get<int64_t>();
		y = location[1].get<int64_t>();
		if (x < 0 || y < 0 || x > max_x || y > max_y) return false;
	} else {
		return false;
	}

	tile = TileXY(static_cast<uint>(x), static_cast<uint>(y));
	return true;
}

/**
 * Describe a resolved pool item, or report why the reference is stale.
 * @tparam T The pool item type.
 * @tparam F Type of the function that adds the type specific fields.
 * @param type The protocol name of the type.
 * @param item The item, or nullptr when no such object exists.
 * @param generation The generation the reference named, if it named one.
 * @param require_identity True for a write: the reference must name a lifetime.
 * @param fill Adds the type specific fields of \a item to the row.
 * @return The resolution of the reference.
 */
template <typename T, typename F>
Json ResolvePoolItem(std::string_view type, const T *item, std::optional<uint64_t> generation, bool require_identity, F fill)
{
	if (item == nullptr) return RefFailure(type, "deleted");

	/*
	 * A write has to be planned for one lifetime of one object. A pooled reference without a
	 * generation names whatever object holds the id now, so a mutation must not accept it: it
	 * would be applied to a recreated object. A read may resolve it, and reports the generation so
	 * the caller can build a strict reference from the answer.
	 */
	if (require_identity && !generation.has_value()) return RefFailure(type, "generation_required");

	Json row = {{"type", std::string(type)}, {"found", true}, {"identity", "pool"},
			{"id", item->index.base()}, {"generation", item->generation}};
	fill(row, *item);

	/* A reference that names a generation is only current while it is the same lifetime of the object. */
	if (generation.has_value() && *generation != item->generation) {
		row["found"] = false;
		row["error"] = "stale_ref";
		row["reason"] = "generation_mismatch";
	}
	return row;
}

/**
 * Resolve a reference to a pooled object of one type.
 *
 * The id is bounded by the pool before it is narrowed to the pool index type, so an id from
 * another pool, a negative id or a value that would truncate is an invalid reference and not a
 * lookup of a random object.
 * @tparam Tindex The pool index type of the object.
 * @tparam Titem The pool item type of the object.
 * @tparam F Type of the function that adds the type specific fields.
 * @param type The protocol name of the type.
 * @param ref The reference to resolve.
 * @param generation The generation the reference named, if it named one.
 * @param require_identity True for a write: the reference must name a lifetime.
 * @param fill Adds the type specific fields of the resolved object to the row.
 * @return The resolution of the reference.
 */
template <typename Tindex, typename Titem, typename F>
Json ResolveTypedRef(std::string_view type, const Json &ref, std::optional<uint64_t> generation, bool require_identity, F fill)
{
	int64_t id = 0;
	if (!ReadBoundedInteger(ref, "id", static_cast<int64_t>(Tindex::End().base()) - 1, id)) return RefFailure(type, "invalid_id", "invalid_params");
	return ResolvePoolItem(type, Titem::GetIfValid(static_cast<Tindex>(id)), generation, require_identity, fill);
}

/**
 * Name of a vehicle type, as used in the protocol.
 * @param type The vehicle type.
 * @return The protocol name.
 */
std::string_view VehicleKindName(VehicleType type)
{
	switch (type) {
		case VehicleType::Train: return "train";
		case VehicleType::Road: return "road";
		case VehicleType::Ship: return "ship";
		case VehicleType::Aircraft: return "aircraft";
		default: return "unknown";
	}
}

/**
 * Name of a station type, as used in the protocol.
 * @param type The station type.
 * @return The protocol name.
 */
std::string_view StationKindName(StationType type)
{
	switch (type) {
		case StationType::Rail: return "rail";
		case StationType::Airport: return "airport";
		case StationType::Truck: return "truck_stop";
		case StationType::Bus: return "bus_stop";
		case StationType::Oilrig: return "oilrig";
		case StationType::Dock: return "dock";
		case StationType::Buoy: return "buoy";
		case StationType::RailWaypoint: return "rail_waypoint";
		case StationType::RoadWaypoint: return "road_waypoint";
		default: return "unknown";
	}
}

/**
 * Resolve a reference to an object the tile itself carries.
 *
 * A reference is `{type, tile, fingerprint?, what?, town_generation?, world_id?, epoch?}`. `type`
 * is one of the typed tile categories (`terrain`, `water`, `tree`, `rail`, `signal`, `road`,
 * `tram`, `bridge`, `tunnel`, `mapobject`, `house`), the generic `tile` spelling — then `what`
 * names the category, or the most specific object of the tile is used — or `point`, which names a
 * location only.
 *
 * A read resolves whatever the tile carries now and reports the current fingerprint, so a caller
 * can build a strict reference from the answer. A write (`require_identity`) has to name the
 * fingerprint, for a house also the generation of its town and for a map object the generation of
 * the pooled object behind it, so a command never runs against a tile whose object changed after
 * the caller looked at it. The pool lifetime is checked before the fingerprint, so a replaced
 * object reports that lifetime instead of a generic state change.
 * @param ref_type The reference type as requested.
 * @param ref The reference.
 * @param require_identity True for a write's `expected_refs`.
 * @return The resolved tile object, or a stale_ref entry.
 */
Json ResolveTileObjectRef(std::string_view ref_type, const Json &ref, bool require_identity)
{
	TileIndex tile = INVALID_TILE;
	if (!ReadRefTile(ref, tile) || !IsValidTile(tile)) return RefFailure(ref_type, "invalid_tile", "invalid_params");

	const Json location = {{"x", TileX(tile)}, {"y", TileY(tile)}};

	/* A point names a location and nothing else: it has no identity, so it is never stale. */
	if (ref_type == "point") {
		Json row = TileRow(tile);
		row["what"] = row["type"];
		row["type"] = "point";
		row["identity"] = "point";
		row["tile"] = location;
		row["found"] = true;
		return row;
	}

	/* The category the reference names; the generic `tile` spelling may leave it to the answer. */
	std::optional<TileObjectKind> requested;
	if (ref_type != "tile") {
		requested = TileObjectKindFromName(ref_type);
		if (!requested.has_value()) return RefFailure(ref_type, "unknown_type", "unsupported_ref_type");
	} else {
		const std::string what = JsonString(ref, "what");
		if (!what.empty()) {
			requested = TileObjectKindFromName(what);
			if (!requested.has_value()) return RefFailure(ref_type, "unknown_what", "invalid_params");
		}
	}

	const Json objects = BuildTileObjectRows(tile);
	const Json *found = nullptr;
	for (const Json &object : objects) {
		if (requested.has_value()) {
			if (object["type"] == std::string(TileObjectKindName(*requested))) {
				found = &object;
				break;
			}
		} else if (found == nullptr) {
			/* The generic spelling without `what` names the most specific object of the tile. */
			found = &object;
		}
	}

	if (found == nullptr) {
		/* The tile no longer carries that kind of object, so the reference is stale, not invalid. */
		Json row = RefFailure(ref_type, "tile_type_mismatch");
		row["tile"] = location;
		row["what"] = Json::array();
		for (const Json &object : objects) row["what"].push_back(object["type"]);
		return row;
	}

	Json row = *found;
	row["found"] = true;

	/*
	 * The pool lifetime a tile object may have is part of its identity, and it is checked before the
	 * fingerprint so that replacing the object behind the tile reports the lifetime that changed
	 * instead of a generic state change. A house has no generation of its own; its lifetime is the
	 * lifetime of its town.
	 */
	const auto check_lifetime = [&](const char *key, const char *missing_reason, const char *mismatch_reason) -> std::optional<Json> {
		if (!row.contains(key) || row[key].is_null()) return std::nullopt;

		std::optional<uint64_t> named;
		if (ref.contains(key) && !ref[key].is_null()) {
			int64_t value = 0;
			if (!ReadBoundedInteger(ref, key, std::numeric_limits<int64_t>::max(), value)) {
				return RefFailure(ref_type, "invalid_generation", "invalid_params");
			}
			named = static_cast<uint64_t>(value);
		}
		/* A read reports the lifetime the object has now; only a write has to match it. */
		if (!require_identity) return std::nullopt;
		if (!named.has_value()) {
			row["found"] = false;
			row["error"] = "stale_ref";
			row["reason"] = missing_reason;
			return row;
		}
		if (*named != row[key].get<uint64_t>()) {
			row["found"] = false;
			row["error"] = "stale_ref";
			row["reason"] = mismatch_reason;
			row["generation_now"] = row[key];
			return row;
		}
		return std::nullopt;
	};

	if (row["type"] == "house") {
		if (const std::optional<Json> stale = check_lifetime("town_generation", "town_generation_required", "town_lifetime_mismatch"); stale.has_value()) return *stale;
	}
	if (row["type"] == "mapobject") {
		if (const std::optional<Json> stale = check_lifetime("object_generation", "object_generation_required", "object_lifetime_mismatch"); stale.has_value()) return *stale;
	}

	std::optional<uint32_t> fingerprint;
	if (ref.contains("fingerprint") && !ref["fingerprint"].is_null()) {
		int64_t value = 0;
		if (!ReadBoundedInteger(ref, "fingerprint", std::numeric_limits<uint32_t>::max(), value)) {
			return RefFailure(ref_type, "invalid_fingerprint", "invalid_params");
		}
		fingerprint = static_cast<uint32_t>(value);
	}

	/*
	 * A read resolves whatever the tile carries now and reports the fingerprint it has now, so a
	 * caller can see the current state and build a strict reference from it. A write has to name
	 * the state it was planned for and match it.
	 */
	if (!require_identity) return row;

	if (!fingerprint.has_value()) {
		row["found"] = false;
		row["error"] = "stale_ref";
		row["reason"] = "fingerprint_required";
		return row;
	}

	const uint32_t current = row["fingerprint"].get<uint32_t>();
	if (*fingerprint != current) {
		row["found"] = false;
		row["error"] = "stale_ref";
		row["reason"] = "tile_state_changed";
		row["fingerprint_now"] = current;
		return row;
	}

	return row;
}

/**
 * Resolve one typed object reference against the current world.
 *
 * A reference is `{type, id?, tile?, generation?, world_id?, epoch?}`, exactly as the protocol
 * schema and `PickObjects()` describe it. The world and the epoch are checked first: a reference
 * read from another world or epoch can never resolve to a current object, so it is reported as
 * stale instead of being matched by id. Then the id and the generation are read as bounded
 * integers, and only then narrowed into the pool.
 *
 * A pooled reference that names a generation is stale when that lifetime is gone, even if the id
 * has been reused. `require_identity` is set for a write: there a pooled reference must name a
 * generation, because a write to "whatever object has this id now" is not the object the caller
 * planned for. Reads stay lenient and report the current generation, so a caller can build a
 * strict reference from a read.
 *
 * @param ref The reference to resolve.
 * @param require_identity True for a write's `expected_refs`: pooled types must carry a generation.
 * @return The resolved object, or a stale_ref / unsupported_ref_type entry.
 */
Json ResolveRef(const Json &ref, bool require_identity)
{
	if (!ref.is_object()) return RefFailure({}, "invalid_ref", "invalid_params");

	const std::string type = JsonString(ref, "type");
	if (type.empty()) return RefFailure(type, "missing_type", "invalid_params");

	/* A reference is only usable in the world and epoch it was observed in. */
	const std::string world_id = JsonString(ref, "world_id");
	const std::string epoch = JsonString(ref, "epoch");
	if (!world_id.empty() || !epoch.empty()) ResolveWorldId();
	if (!world_id.empty() && world_id != _state.world_id) return RefFailure(type, "world_mismatch");
	if (!epoch.empty() && epoch != _state.epoch) return RefFailure(type, "epoch_mismatch");

	std::optional<uint64_t> generation;
	if (ref.contains("generation") && !ref["generation"].is_null()) {
		int64_t value = 0;
		if (!ReadBoundedInteger(ref, "generation", std::numeric_limits<int64_t>::max(), value)) return RefFailure(type, "invalid_generation", "invalid_params");
		generation = static_cast<uint64_t>(value);
	}

	if (type == "town") {
		return ResolveTypedRef<TownID, Town>(type, ref, generation, require_identity, [](Json &row, const Town &t) {
			row["name"] = t.GetCachedName();
			row["tile"] = Json{{"x", TileX(t.xy)}, {"y", TileY(t.xy)}};
			row["population"] = t.cache.population;
		});
	}
	if (type == "company") {
		return ResolveTypedRef<CompanyID, Company>(type, ref, generation, require_identity, [](Json &row, const Company &c) {
			row["name"] = c.name.empty() ? GetString(STR_COMPANY_NAME, c.index) : c.name;
			row["is_ai"] = c.is_ai;
			row["money"] = static_cast<int64_t>(c.money);
		});
	}
	if (type == "station") {
		return ResolveTypedRef<StationID, Station>(type, ref, generation, require_identity, [](Json &row, const Station &st) {
			row["name"] = st.GetCachedName();
			row["tile"] = Json{{"x", TileX(st.xy)}, {"y", TileY(st.xy)}};
			row["owner"] = st.owner.base();
			row["town_id"] = st.town != nullptr ? st.town->index.base() : -1;
		});
	}
	if (type == "waypoint") {
		/* A waypoint is a station pool item; the schema names it separately. */
		return ResolveTypedRef<StationID, Waypoint>(type, ref, generation, require_identity, [](Json &row, const Waypoint &wp) {
			row["name"] = wp.GetCachedName();
			row["tile"] = Json{{"x", TileX(wp.xy)}, {"y", TileY(wp.xy)}};
			row["owner"] = wp.owner.base();
		});
	}
	if (type == "industry") {
		return ResolveTypedRef<IndustryID, Industry>(type, ref, generation, require_identity, [](Json &row, const Industry &ind) {
			row["name"] = ind.GetCachedName();
			row["tile"] = Json{{"x", TileX(ind.location.tile)}, {"y", TileY(ind.location.tile)}};
			row["town_id"] = ind.town != nullptr ? ind.town->index.base() : -1;
		});
	}
	if (type == "vehicle") {
		return ResolveTypedRef<VehicleID, Vehicle>(type, ref, generation, require_identity, [](Json &row, const Vehicle &v) {
			row["name"] = v.name;
			row["what"] = std::string(VehicleKindName(v.type));
			row["owner"] = v.owner.base();
			row["tile"] = Json{{"x", TileX(v.tile)}, {"y", TileY(v.tile)}};
		});
	}
	if (type == "depot") {
		return ResolveTypedRef<DepotID, Depot>(type, ref, generation, require_identity, [](Json &row, const Depot &d) {
			row["name"] = d.name;
			row["what"] = std::string(TileTypeName(GetTileType(d.xy)));
			row["tile"] = Json{{"x", TileX(d.xy)}, {"y", TileY(d.xy)}};
			row["town_id"] = d.town != nullptr ? d.town->index.base() : -1;
		});
	}
	if (type == "engine") {
		return ResolveTypedRef<EngineID, Engine>(type, ref, generation, require_identity, [](Json &row, const Engine &e) {
			row["name"] = e.name;
			row["what"] = std::string(VehicleKindName(e.type));
		});
	}
	if (type == "sign") {
		return ResolveTypedRef<SignID, Sign>(type, ref, generation, require_identity, [](Json &row, const Sign &sign) {
			row["name"] = sign.name;
			row["owner"] = sign.owner.base();
			const TileIndex tile = TileVirtXY(static_cast<uint>(std::max(0, sign.x)), static_cast<uint>(std::max(0, sign.y)));
			row["tile"] = Json{{"x", TileX(tile)}, {"y", TileY(tile)}};
		});
	}
	if (type == "group") {
		return ResolveTypedRef<GroupID, Group>(type, ref, generation, require_identity, [](Json &row, const Group &g) {
			row["name"] = g.name;
			row["owner"] = g.owner.base();
		});
	}
	/* The objects a tile itself carries: typed categories, the generic `tile` and `point`. */
	if (type == "tile" || type == "point" || TileObjectKindFromName(type).has_value()) {
		return ResolveTileObjectRef(type, ref, require_identity);
	}

	/*
	 * Types the kernel cannot resolve are an explicit gap, never a silent success: `cargo` is not a
	 * pooled object at all, so it has neither an id nor a lifetime to check.
	 */
	return RefFailure(type, type == "cargo" ? "not_a_pooled_object" : "unknown_type", "unsupported_ref_type");
}

/**
 * Implement world.objects: resolve typed references to current state.
 * @param params The parameters of the method.
 * @return The result of the method.
 */
Json WorldObjects(const Json &params)
{
	if (!params.contains("refs") || !params["refs"].is_array()) return Error("invalid_params", "refs must be an array");
	const Json &refs = params["refs"];
	if (refs.empty() || refs.size() > MAX_OBJECT_REFS) return Error("invalid_params", "refs must contain 1 to 64 references");

	Json objects = Json::array();
	for (const Json &ref : refs) {
		if (!ref.is_object()) return Error("invalid_params", "every ref must be an object");
		/* world.objects is a read: it resolves the current object and reports its generation. */
		objects.push_back(ResolveRef(ref, false));
	}

	Json value = {{"objects", objects}};
	value.update(WorldBinding());
	return Ok(std::move(value));
}

/**
 * Implement world.pause.
 * @param params The parameters of the method.
 * @return The result of the method.
 */
Json WorldPause(const Json &params)
{
	if (!params.contains("paused") || !params["paused"].is_boolean()) return Error("invalid_params", "paused must be a boolean");
	const bool paused = params["paused"].get<bool>();

	const uint32_t revision = PauseRevision();
	const bool normal_now = _pause_mode.Test(PauseMode::Normal);
	const bool previous_paused = _pause_mode.Any();

	/*
	 * An optional revision guard: the caller observed the pause state at `expected_revision` and
	 * only wants to change it while that is still the decision in force. A newer decision of the
	 * player (or of another task) is reported as stale before anything is posted, so a temporary
	 * agent pause is never restored over a pause the player made afterwards.
	 *
	 * The world/epoch of the request is the real protection across a load: an epoch that starts
	 * while the pause state is left alone begins with the revision of the previous epoch, so an
	 * equal revision alone cannot tell the two epochs apart. The epoch comparison below is only an
	 * additional check for a revision that is older than the current epoch.
	 */
	std::optional<uint32_t> expected;
	if (params.contains("expected_revision") && !params["expected_revision"].is_null()) {
		int64_t value = 0;
		if (!ReadBoundedInteger(params, "expected_revision", std::numeric_limits<uint32_t>::max(), value)) {
			return Error("invalid_params", "`expected_revision` must be a pause revision");
		}
		expected = static_cast<uint32_t>(value);
	}

	const PauseRequestDecision decision = DecidePauseRequest(normal_now, paused, expected, revision, _state.pause_revision_at_epoch);
	if (decision.guard == PauseGuardResult::EpochChanged) {
		Json error = Error("stale_pause_revision", "the revision was read before the current epoch");
		error["reason"] = "epoch_changed";
		error["expected_revision"] = *expected;
		error["revision"] = revision;
		error["revision_at_epoch_start"] = _state.pause_revision_at_epoch;
		return error;
	}
	if (decision.guard == PauseGuardResult::RevisionMismatch) {
		Json error = Error("stale_pause_revision", "the pause state changed after the revision was read");
		error["reason"] = "revision_mismatch";
		error["expected_revision"] = *expected;
		error["revision"] = revision;
		error["paused"] = previous_paused;
		error["normal"] = normal_now;
		return error;
	}

	/* The command only runs when it would really change the pause mode the tool controls, so a
	 * revision only moves on a real decision and a no-op request is answered without one. */
	if (decision.post && !Command<Commands::Pause>::Post(PauseMode::Normal, paused)) return Error("command_failed", "pause");

	return Ok({
		{"paused", _pause_mode.Any()},
		{"normal", _pause_mode.Test(PauseMode::Normal)},
		{"revision", PauseRevision()},
		{"changed", decision.post},
		{"previous", {{"paused", previous_paused}, {"normal", normal_now}, {"revision", revision}}},
	});
}

/**
 * Implement world.rename for towns, companies and stations.
 * @param params The parameters of the method.
 * @param request_company The company the request was made for, if any.
 * @return The result of the method.
 */
Json WorldRename(const Json &params, CompanyID request_company)
{
	const std::string kind = JsonString(params, "kind");
	const std::string name = JsonString(params, "name");
	if (name.empty()) return Error("invalid_params", "name must not be empty");

	/*
	 * Every id is bounded by the pool of its kind before it is narrowed, so an id of another kind,
	 * a negative id or a value that would truncate is an invalid request and not a rename of a
	 * random object. An id inside the pool that names no object is `unknown_object`.
	 */
	int64_t id = 0;

	if (kind == "town") {
		if (!ReadBoundedInteger(params, "id", static_cast<int64_t>(TownID::End().base()) - 1, id)) return Error("invalid_params", "`id` must be a town id");
		const TownID town_id = static_cast<TownID>(id);
		if (!Town::IsValidID(town_id)) return Error("unknown_object", "no such town");
		if (!Command<Commands::RenameTown>::Post(STR_ERROR_CAN_T_RENAME_TOWN, town_id, name)) return Error("command_failed", "rename town");
	} else if (kind == "company") {
		CompanyID company_id = request_company;
		if (params.contains("id")) {
			if (!ReadBoundedInteger(params, "id", static_cast<int64_t>(CompanyID::End().base()) - 1, id)) return Error("invalid_params", "`id` must be a company id");
			company_id = static_cast<CompanyID>(id);
		}
		if (!Company::IsValidID(company_id)) return Error("unknown_object", "no such company");

		/* RenameCompany operates on the current company, so scope it to the requested one. */
		Backup<CompanyID> backup(_current_company, company_id);
		const bool posted = Command<Commands::RenameCompany>::Post(STR_ERROR_CAN_T_CHANGE_COMPANY_NAME, name);
		backup.Restore();
		if (!posted) return Error("command_failed", "rename company");

		/* Verify the object that was actually renamed, not just that the command was accepted. */
		const Company *company = Company::GetIfValid(company_id);
		if (company == nullptr || company->name != name) return Error("command_failed", "the company name was not changed");
	} else if (kind == "station") {
		if (!ReadBoundedInteger(params, "id", static_cast<int64_t>(StationID::End().base()) - 1, id)) return Error("invalid_params", "`id` must be a station id");
		const StationID station_id = static_cast<StationID>(id);
		if (!Station::IsValidID(station_id)) return Error("unknown_object", "no such station");
		if (!Command<Commands::RenameStation>::Post(STR_ERROR_CAN_T_RENAME_STATION, station_id, name)) return Error("command_failed", "rename station");
	} else if (kind == "waypoint") {
		if (!ReadBoundedInteger(params, "id", static_cast<int64_t>(StationID::End().base()) - 1, id)) return Error("invalid_params", "`id` must be a waypoint id");
		const StationID waypoint_id = static_cast<StationID>(id);
		if (Waypoint::GetIfValid(waypoint_id) == nullptr) return Error("unknown_object", "no such waypoint");
		if (!Command<Commands::RenameWaypoint>::Post(STR_ERROR_CAN_T_CHANGE_WAYPOINT_NAME, waypoint_id, name)) return Error("command_failed", "rename waypoint");
	} else if (kind == "vehicle") {
		if (!ReadBoundedInteger(params, "id", static_cast<int64_t>(VehicleID::End().base()) - 1, id)) return Error("invalid_params", "`id` must be a vehicle id");
		const VehicleID vehicle_id = static_cast<VehicleID>(id);
		const Vehicle *vehicle = Vehicle::GetIfValid(vehicle_id);
		if (vehicle == nullptr) return Error("unknown_object", "no such vehicle");

		/* The engine checks ownership against the current company, so scope it to the owner. */
		bool posted = false;
		{
			const Backup<CompanyID> context(_current_company, Company::IsValidID(request_company) ? request_company : vehicle->owner);
			posted = Command<Commands::RenameVehicle>::Post(STR_ERROR_CAN_T_RENAME_TRAIN + to_underlying(vehicle->type), vehicle_id, name);
		}
		if (!posted) return Error("command_failed", "rename vehicle");
	} else if (kind == "sign") {
		if (!ReadBoundedInteger(params, "id", static_cast<int64_t>(SignID::End().base()) - 1, id)) return Error("invalid_params", "`id` must be a sign id");
		const SignID sign_id = static_cast<SignID>(id);
		if (Sign::GetIfValid(sign_id) == nullptr) return Error("unknown_object", "no such sign");

		/* Colours::Invalid keeps the current text colour, which is what a rename without one means. */
		bool posted = false;
		{
			const Backup<CompanyID> context(_current_company, Company::IsValidID(request_company) ? request_company : OWNER_DEITY);
			posted = Command<Commands::RenameSign>::Post(STR_ERROR_CAN_T_CHANGE_SIGN_NAME, sign_id, name, Colours::Invalid);
		}
		if (!posted) return Error("command_failed", "rename sign");
	} else if (kind == "group") {
		/* The engine has no command that renames a group, so this is an explicit gap. */
		return Error("unsupported_rename_kind", "the engine has no command to rename a group");
	} else {
		return Error("unsupported_rename_kind", kind);
	}

	return Ok({{"ok", true}, {"name", name}});
}

/**
 * Validate the references a mutation expects to still be current.
 *
 * The check is atomic: every reference is resolved before anything is applied,
 * so a mutation never runs against a world in which one of its objects was
 * deleted and its id reused.
 * @param params The parameters of the request.
 * @return An error result when a reference is stale, nothing when all are current.
 */
std::optional<Json> CheckExpectedRefs(const Json &params)
{
	if (!params.contains("expected_refs")) return std::nullopt;
	const Json &refs = params["expected_refs"];
	if (!refs.is_array()) return Error("invalid_params", "expected_refs must be an array");
	if (refs.size() > MAX_OBJECT_REFS) return Error("invalid_params", "expected_refs must contain at most 64 references");

	for (const Json &ref : refs) {
		if (!ref.is_object()) return Error("invalid_params", "every expected ref must be an object");
		/* A write must name the lifetime it was planned for: a reference without a generation is rejected. */
		Json resolved = ResolveRef(ref, true);
		if (!resolved.value("found", false)) {
			Json error = Error("stale_ref", "an expected object is no longer the one the request was made for");
			error["detail"] = std::move(resolved);
			return error;
		}
	}
	return std::nullopt;
}

/**
 * Add the lifetime generation to the rows of a delegated read.
 *
 * A query row is what a caller turns into a reference, and a write's reference must name the
 * lifetime it was planned for. The tool modules report the inventory without a generation, so the
 * bridge completes the rows from the pool; a row whose id does not resolve keeps no generation and
 * stays a read-only hint.
 * @param kind The canonical query kind.
 * @param rows The rows to complete in place.
 */
void AddRowGenerations(std::string_view kind, Json &rows)
{
	for (Json &row : rows) {
		if (!row.is_object() || row.contains("generation")) continue;

		int64_t id = 0;
		if (!ReadBoundedInteger(row, "id", std::numeric_limits<int64_t>::max(), id)) continue;

		if (kind == "vehicles") {
			if (id >= static_cast<int64_t>(VehicleID::End().base())) continue;
			if (const Vehicle *v = Vehicle::GetIfValid(static_cast<VehicleID>(id)); v != nullptr) row["generation"] = v->generation;
		} else if (kind == "engines") {
			if (id >= static_cast<int64_t>(EngineID::End().base())) continue;
			if (const Engine *e = Engine::GetIfValid(static_cast<EngineID>(id)); e != nullptr) row["generation"] = e->generation;
		} else if (kind == "groups") {
			if (id >= static_cast<int64_t>(GroupID::End().base())) continue;
			if (const Group *g = Group::GetIfValid(static_cast<GroupID>(id)); g != nullptr) row["generation"] = g->generation;
		}
	}
}

/**
 * Check that a request belongs to the world and epoch the bridge is bound to.
 * @param request The request, as received from the backend.
 * @return An error result when the request is stale, nothing when it may be applied.
 */
std::optional<Json> CheckRequestWorld(const Json &request)
{
	const std::string world_id = JsonString(request, "world_id");
	const std::string epoch = JsonString(request, "epoch");
	if (world_id.empty() || epoch.empty()) return Error("invalid_request", "world_id and epoch are required");
	if (world_id != _state.world_id) return Error("stale_world", "the request belongs to another world");
	if (epoch != _state.epoch) return Error("stale_epoch", "the request belongs to another epoch");
	return std::nullopt;
}

/**
 * Read the company a request is made for.
 * @param request The request, as received from the backend.
 * @return The company id, or CompanyID::Invalid() when the request does not name one.
 */
CompanyID RequestCompanyId(const Json &request)
{
	const Json params = request.contains("params") && request["params"].is_object() ? request["params"] : Json::object();
	const int id = request.contains("company_id") ? JsonInt(request, "company_id", -1) : JsonInt(params, "company_id", -1);
	if (id < 0 || !Company::IsValidID(static_cast<CompanyID>(id))) return CompanyID::Invalid();
	return static_cast<CompanyID>(id);
}

/**
 * Turn a failed capture into a bridge error, keeping the diagnostics of the capture.
 * @param capture The failed capture result.
 * @return The error result.
 */
Json CaptureFailure(const Json &capture)
{
	Json result = Error(JsonString(capture, "error", "capture_failed"), "the frame could not be captured");
	for (const char *key : {"tick", "tick_now", "width", "height", "pixels", "max_pixels", "png_bytes", "max_bytes", "max_body_bytes"}) {
		if (capture.contains(key)) result[key] = capture[key];
	}
	return result;
}

/**
 * Turn a camera or projection result into a bridge result.
 *
 * Same shape as the other deferred method: a success is `{ok: true, value: <camera result>}`, a
 * failure keeps the camera's own error code and its diagnostics at the top level, so a missing
 * viewport is never reported as a success. A read is bound to the world and epoch it was taken in,
 * like every other read of the bridge.
 * @param camera The result of InspectGameViewport() or SetGameCamera().
 * @param bind_world Whether to add world_id/epoch to the value (reads only).
 * @return The bridge result.
 */
Json CameraResult(Json camera, bool bind_world)
{
	if (!camera.is_object()) return Ok(std::move(camera));

	if (!camera.value("ok", false)) {
		Json result = Error(JsonString(camera, "error", "camera_failed"), JsonString(camera, "message", "the camera request failed"));
		if (camera.contains("tick")) result["tick"] = camera["tick"];
		return result;
	}

	if (bind_world) camera.update(WorldBinding());
	return Ok(std::move(camera));
}

/**
 * Run one deferred request on the draw tick.
 *
 * Runs with the game state lock and the video buffer lock held, so neither the world nor the frame
 * can change underneath it. The world, the epoch and the task barrier of the request are checked
 * again, and a write is checked against `expected_refs` a second time: a load, a Stop or a changed
 * object between scheduling and drawing must not turn into a write of another world or object.
 * @param deferred The request to run.
 * @return The bridge result.
 */
Json RunDeferred(const DeferredRequest &deferred)
{
	if (!IsWorldReady()) return Error("world_not_ready", "the world is being generated or loaded");
	if (!deferred.task_id.empty() && _state.barrier_tasks.find(deferred.task_id) != _state.barrier_tasks.end()) {
		return Error("task_stopped", "the task has been stopped");
	}

	const bool mutation = IsMutation(deferred.method, deferred.params);
	if (mutation) {
		if (const std::optional<Json> stale = CheckExpectedRefs(deferred.params); stale.has_value()) return *stale;
	}

	if (deferred.method == "world.capture") {
		const Json capture = CaptureGameFrame(deferred.params);
		return capture.value("ok", false) ? Ok(capture) : CaptureFailure(capture);
	}

	/* world.camera.get and a world.project projection read the camera; world.camera.set moves it. */
	const Json camera = mutation ? SetGameCamera(deferred.params) : InspectGameViewport(deferred.params);
	return CameraResult(std::move(camera), !mutation);
}

/**
 * Finish a request that was deferred to the draw tick.
 *
 * The receipt and the result are stored only here, so a placeholder is never delivered as if the
 * request had been applied.
 *
 * The entry is removed first, which releases the receipt and result slot the request reserved. That
 * is safe because this runs under the game state lock, so no other capacity check can observe the
 * released slot before the answer of this request is stored and queued below.
 * @param request_id The request that was deferred.
 */
void FinishDeferredRequest(const std::string &request_id)
{
	const auto entry = _deferred.find(request_id);
	if (entry == _deferred.end()) return;
	const DeferredRequest deferred = std::move(entry->second);
	_deferred.erase(entry);

	/*
	 * The backend invalidates the requests of an epoch it no longer serves, so a request of a world
	 * that is gone is dropped instead of answered with a result the backend would reject as stale
	 * and the bridge would retry forever.
	 */
	if (deferred.world_id != _state.world_id || deferred.epoch != _state.epoch) {
		Debug(Facility::Net, Severity::Info, "In-game agent bridge: dropping deferred {} {} of a world that is gone", deferred.method, request_id);
		return;
	}

	const Clock::time_point started = Clock::now();
	const Json result = RunDeferred(deferred);
	/* The draw-tick job is measured on its own, apart from the inline reads and writes. */
	RecordExecution(_state.deferred_timing,
			static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - started).count()),
			DEFERRED_BUDGET_US);

	StoreReceipt(request_id, result);
	/*
	 * The reservation taken by ScheduleDeferred guarantees this slot: every other capacity check
	 * counts the pending requests, and this request has just released its own. The fallback keeps
	 * the answer deliverable even if that invariant is ever broken, because a deferred result
	 * cannot be replayed from the request.
	 */
	if (!QueueResult(request_id, result)) {
		Debug(Facility::Net, Severity::Warning, "In-game agent bridge: result queue is full; queueing the deferred result anyway");
		_state.results.push_back({{"request_id", request_id}, {"result", result}});
	}
}

/**
 * Schedule a request for the next draw tick.
 *
 * The request must not run inside the game loop: the draw thread takes the video buffer lock before
 * the game state lock, so touching the frame or the viewport from the game thread would invert that
 * order. VideoDriver::QueueOnMainThread runs the callback on the next draw tick with both locks
 * held, which is exactly the context CaptureGameFrame(), InspectGameViewport() and SetGameCamera()
 * document.
 * @param method The method the request calls.
 * @param request The request, as received from the backend.
 * @param request_id The id of the request.
 * @param params The parameters of the request.
 * @param deferred Set to true iff the request was scheduled and answers later.
 * @return The result of the request; a placeholder when the request was scheduled.
 */
Json ScheduleDeferred(std::string_view method, const Json &request, const std::string &request_id, const Json &params, bool &deferred)
{
	/* A replay of a request that is already waiting must not schedule a second execution. */
	if (_deferred.find(request_id) != _deferred.end()) {
		deferred = true;
		return Ok({{"deferred", true}});
	}

	/* Without a GUI there is no draw tick to run the callback on, so the queue would never drain. */
	VideoDriver *driver = VideoDriver::GetInstance();
	if (driver == nullptr || !driver->HasGUI()) {
		return Error("headless_unavailable", "no video driver with a GUI; the request cannot run on the draw tick");
	}

	/* Reserve the receipt and the result of the request before it is scheduled, so the answer can
	 * always be stored and delivered when the draw tick runs. */
	if (_deferred.size() >= MAX_PENDING_DEFERRED ||
			_state.results.size() + _deferred.size() >= MAX_PENDING_RESULTS ||
			_state.receipts.size() + _deferred.size() >= MAX_RECEIPTS) {
		return Error("busy", "too many requests are waiting for the next draw tick");
	}

	_deferred[request_id] = DeferredRequest{std::string(method), _state.world_id, _state.epoch, JsonString(request, "task_id"), params};
	driver->QueueOnMainThread([request_id]() { FinishDeferredRequest(request_id); });
	deferred = true;
	return Ok({{"deferred", true}});
}

/**
 * Execute one request from the backend on the main thread.
 * @param request The request, as received from the backend.
 * @param deferred Set to true iff the request answers later, from the draw tick.
 * @return The result of the request; a placeholder when the request was deferred.
 */
Json ExecuteRequestInner(const Json &request, bool &deferred)
{
	deferred = false;
	const std::string method = JsonString(request, "method");
	const Json params = request.contains("params") && request["params"].is_object() ? request["params"] : Json::object();
	const std::string task_id = JsonString(request, "task_id");

	/* A request may only be applied to the world and epoch it was issued for; check before anything else. */
	if (const std::optional<Json> stale = CheckRequestWorld(request); stale.has_value()) return *stale;

	/*
	 * The world may have started generating or loading between the poll and this
	 * execution; the map and the pools are not consistent then, so fail closed
	 * instead of reading or mutating a half built world.
	 */
	if (!IsWorldReady()) return Error("world_not_ready", "the world is being generated or loaded");

	/* The company the call is made for; every tool module scopes its commands to it. */
	const CompanyID company = RequestCompanyId(request);
	const bool mutation = IsMutation(method, params);

	/* Mutations are only supported in single player: in multiplayer Post() merely enqueues the command. */
	if (mutation && _networking) return Error("multiplayer_unsupported", "mutations are only supported in single player");

	/* The barrier is the last request of a task; nothing of that task may mutate the world afterwards. */
	if (method == "task.barrier") {
		if (!task_id.empty()) _state.barrier_tasks.insert(task_id);
		return Ok({{"barrier", true}});
	}

	if (mutation && !task_id.empty() && _state.barrier_tasks.find(task_id) != _state.barrier_tasks.end()) {
		return Error("task_stopped", "the task has been stopped");
	}

	/* A mutation may only run when the objects it was planned for are still the same objects. */
	if (mutation) {
		if (const std::optional<Json> stale = CheckExpectedRefs(params); stale.has_value()) return *stale;
	}

	if (method == "world.info") return WorldInfo();
	if (method == "world.query") {
		/*
		 * The tool modules own the kinds they implement (the game module answers vehicles, engines
		 * and orders), and the core query keeps the rest: a module returns nothing for a kind it
		 * does not claim, so a kind is never shadowed by a thinner answer and a kind a module adds
		 * later needs no change here. The module knows the canonical plural kind only, so the
		 * legacy singular alias is normalised for it; a kind this bridge does not canonicalise is
		 * passed through unchanged, so the module can own it by itself.
		 */
		const std::string requested = JsonString(params, "kind");
		const std::string_view kind = CanonicalQueryKind(requested);
		Json module_params = params;
		if (!kind.empty()) module_params["kind"] = std::string(kind);

		if (std::optional<Json> result = DispatchGameTool(method, module_params, company); result.has_value()) {
			Json mapped = FromModule(std::move(*result));
			if (mapped["ok"].is_boolean() && mapped["ok"].get<bool>() && mapped.contains("value") && mapped["value"].is_object()) {
				Json &value = mapped["value"];
				/* A row has to carry its lifetime, so a reference built from it can be strict. */
				if (value.contains("rows") && value["rows"].is_array()) AddRowGenerations(kind, value["rows"]);
				/* Every read is bound to the world and epoch it was taken in, whoever produced it. */
				value.update(WorldBinding());
			}
			return mapped;
		}
		return WorldQuery(params);
	}
	if (method == "world.objects") return WorldObjects(params);
	if (method == "world.pause") return WorldPause(params);
	if (method == "world.rename") return WorldRename(params, company);

	/* Read-only description of the native chat UI, so a GUI test can locate real targets. */
	if (method == "ui.inspect") return Ok(::DescribeAgentChatUi());

	/*
	 * The frame is copied and the camera is read or moved on the draw thread, so these answer from
	 * the next draw tick. A world.project call that asks for a screen projection is a camera read;
	 * a world.project call that names an administrative op stays with the admin module below, so
	 * neither shadows the other.
	 */
	if (method == "world.capture" || method == "world.camera.get" || method == "world.camera.set" ||
			(method == "world.project" && IsProjectionRequest(params))) {
		return ScheduleDeferred(method, request, JsonString(request, "request_id"), params, deferred);
	}

	/*
	 * Construction is split between two modules. The transport module owns the rail, water and
	 * air ops and returns nothing for the road ops it leaves to the game tool module, which is
	 * therefore asked second; only an op a module explicitly owns is executed by it.
	 */
	if (method == "world.construct") {
		if (std::optional<Json> result = DispatchTransportTool(method, params, company); result.has_value()) {
			return FromModule(std::move(*result));
		}
		if (std::optional<Json> result = DispatchGameTool(method, params, company); result.has_value()) {
			return FromModule(std::move(*result));
		}
		return Error("unsupported_method", method);
	}

	/* Vehicle and order commands belong to the game tool module. */
	if (method == "world.vehicles" || method == "world.orders") {
		if (std::optional<Json> result = DispatchGameTool(method, params, company); result.has_value()) {
			return FromModule(std::move(*result));
		}
		return Error("unsupported_method", method);
	}

	/* Administrative families: settings, companies, towns and projects. */
	if (std::optional<Json> result = DispatchAdminTool(method, params, company); result.has_value()) {
		return FromModule(std::move(*result));
	}

	/* The engine command catalog: world.commands (read) and world.command (generic write). */
	if (std::optional<Json> result = DispatchCommandCatalog(method, params, company); result.has_value()) {
		return FromModule(std::move(*result));
	}

	/*
	 * The rail blueprint and trunk line module owns world.blueprint and world.rail_route. The world,
	 * the epoch, the task barrier and the expected references were checked above; the binding of
	 * this request is forced into the parameters, so a plan can never be read or built for another
	 * task, world or epoch through a model supplied echo.
	 */
	if (method == "world.blueprint" || method == "world.rail_route") {
		if (std::optional<Json> result = DispatchBlueprintTool(method, BlueprintModuleParams(request, params), company); result.has_value()) {
			return FromModule(std::move(*result));
		}
		return Error("unsupported_method", method);
	}

	return Error("unsupported_method", method);
}

/**
 * Handle the response to a hello.
 * @param ok Whether the request succeeded.
 * @param body The parsed response.
 */
void HandleHelloResponse(bool ok, const Json &body)
{
	if (!ok) {
		_state.connected = false;
		_state.next_hello = Clock::now() + HELLO_RETRY;
		Debug(Facility::Net, Severity::Warning, "In-game agent bridge: hello failed; retrying");
		return;
	}

	_state.connection_id = JsonString(body, "connection_id");
	_state.connected = !_state.connection_id.empty();
	_state.heartbeat_seconds = Clamp(static_cast<int>(body.value("heartbeat_seconds", 5.0)), 1, 60);
	_state.next_poll = Clock::now();

	Debug(Facility::Net, Severity::Info, "In-game agent bridge: connected to the backend (heartbeat {}s)", _state.heartbeat_seconds);
}

/**
 * Run one request on the game thread and record how long it took.
 *
 * An inline request is measured as a read or as a write, whichever it is; a deferred one is
 * measured where it really runs, on the draw tick, so its scheduling is not counted as execution
 * time. The measurement covers the execution only: the poll that delivered the request and the
 * delivery of the answer are outside of it.
 * @param request The request, as received from the backend.
 * @param deferred Set to true iff the request answers later.
 * @return The result of the request.
 */
Json ExecuteRequest(const Json &request, bool &deferred)
{
	deferred = false;
	const Clock::time_point started = Clock::now();
	Json result = ExecuteRequestInner(request, deferred);

	if (!deferred) {
		const uint64_t micros = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - started).count());
		const std::string method = JsonString(request, "method");
		const Json params = request.contains("params") && request["params"].is_object() ? request["params"] : Json::object();
		if (IsMutation(method, params)) {
			RecordExecution(_state.command_timing, micros, INLINE_BUDGET_US);
		} else {
			RecordExecution(_state.query_timing, micros, INLINE_BUDGET_US);
		}
	}
	return result;
}

/**
 * Handle the response to a poll: execute the requests, in order, on the main thread.
 * @param ok Whether the request succeeded.
 * @param body The parsed response.
 */
void HandlePollResponse(bool ok, const Json &body)
{
	if (!ok) {
		_state.next_poll = Clock::now() + POLL_RETRY;
		return;
	}

	const Clock::time_point batch_started = Clock::now();
	bool complete = true;
	if (body.contains("requests") && body["requests"].is_array()) {
		for (const Json &request : body["requests"]) {
			if (!request.is_object()) continue;
			const std::string request_id = JsonString(request, "request_id");
			if (request_id.empty()) continue;

			/* A repeated request is the same request: answer with the stored receipt instead of executing it twice. */
			const auto receipt = _state.receipts.find(request_id);
			Json result;
			if (receipt != _state.receipts.end()) {
				result = receipt->second.result;
			} else {
				/* Fail closed when no receipt may be forgotten: a replay must never execute a request twice. */
				if (!HasReceiptCapacity()) {
					complete = false;
					break;
				}
				bool deferred = false;
				result = ExecuteRequest(request, deferred);
				if (deferred) {
					/* A deferred request answers from the draw tick: no receipt and no result yet. */
					continue;
				}
				StoreReceipt(request_id, result);
			}

			/* Never drop a result: an applied mutation has to reach the backend. */
			if (!QueueResult(request_id, result)) {
				complete = false;
				break;
			}
		}
	}

	/* One poll's requests are one game-thread slice, which is what the plan's batch target bounds. */
	RecordExecution(_state.batch_timing,
			static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - batch_started).count()),
			INLINE_BUDGET_US);

	/* Only advance the cursor when the whole batch was handled; otherwise the backend replays it. */
	if (complete && body.contains("cursor") && body["cursor"].is_number_integer()) {
		_state.cursor = body["cursor"].get<int64_t>();
	}

	/* A healthy poll repeats on the idle cadence; the heartbeat stays a separate, slower signal. */
	_state.next_poll = Clock::now() + (complete ? POLL_IDLE : POLL_RETRY);
}

/**
 * Handle the response to a result delivery.
 * @param ok Whether the request succeeded.
 */
void HandleResultResponse(bool ok)
{
	if (!ok) {
		_state.next_result = Clock::now() + RESULT_RETRY;
		if (!_state.inflight_result.is_null()) _state.results.push_front(_state.inflight_result);
	} else if (!_state.inflight_result.is_null()) {
		/* The backend accepted the result, so its receipt may be forgotten later on. */
		AcknowledgeReceipt(JsonString(_state.inflight_result, "request_id"));
	}
	_state.inflight_result = Json();
}

/**
 * Handle everything the HTTP layer finished since the last call.
 */
void ProcessCompletions()
{
	if (_completions.empty()) return;

	std::vector<Completion> completions;
	completions.swap(_completions);

	for (const Completion &completion : completions) {
		const auto pending = _pending.find(completion.id);
		if (pending == _pending.end()) continue;
		const PendingRequest request = std::move(pending->second);
		_pending.erase(pending);
		_callbacks.erase(completion.id);

		Json body = Json::object();
		bool parsed = completion.body.empty();
		if (completion.ok && !completion.body.empty()) {
			body = Json::parse(completion.body, nullptr, false);
			parsed = !body.is_discarded();
			if (!parsed) body = Json::object();
		}
		const bool ok = completion.ok && parsed;

		/* A completion of an older epoch must not touch the state of the current one. */
		const bool current = request.epoch == _state.epoch;

		switch (request.kind) {
			case RequestKind::Hello:
				if (current) {
					_state.hello_in_flight = false;
					HandleHelloResponse(ok, body);
				}
				break;

			case RequestKind::Poll:
				if (current) {
					_state.poll_in_flight = false;
					HandlePollResponse(ok, body);
				}
				break;

			case RequestKind::Result:
				if (current) {
					_state.result_in_flight = false;
					HandleResultResponse(ok);
				}
				break;

			case RequestKind::Ui:
				/* A late answer of an older epoch must not look like current data: the UI would act on a world that is gone. */
				if (request.callback) {
					if (current) {
						request.callback(ok, body);
					} else {
						request.callback(false, Error("stale_epoch", "the world changed while the request was in flight"));
					}
				}
				break;
		}
	}
}

/**
 * The camera methods of this file, described the way the tool modules describe theirs.
 *
 * The parameters are the ones InspectGameViewport() and SetGameCamera() document in
 * `src/agent_capture.h`; `parameters_header` names that file so an operator can read the exact
 * meaning and the returned fields without guessing.
 * @return Capability document of the camera methods.
 */
Json CameraCapabilities()
{
	const Json read_parameters = Json::array({"tile", "tile_x", "tile_y", "town", "center", "screen_x", "screen_y", "clamp_to_map"});
	const Json write_parameters = Json::array({"tile", "tile_x", "tile_y", "town", "center", "scroll", "zoom", "zoom_delta", "instant"});

	Json methods = Json::array();
	methods.push_back({
		{"method", "world.camera.get"},
		{"authority", "read"},
		{"deferred", true},
		{"implemented", Json::array({"camera", "tile_to_screen", "screen_to_tile"})},
		{"parameters", read_parameters},
	});
	methods.push_back({
		{"method", "world.camera.set"},
		{"authority", "write"},
		{"deferred", true},
		{"implemented", Json::array({"center_tile", "center_town", "scroll", "zoom", "zoom_delta"})},
		{"parameters", write_parameters},
	});
	methods.push_back({
		{"method", "world.project"},
		{"authority", "read"},
		{"deferred", true},
		{"implemented", Json::array({"projection"})},
		{"parameters", read_parameters},
	});

	Json result = Json::object();
	result["module"] = "agent_bridge_camera";
	result["parameters_header"] = "src/agent_capture.h";
	result["methods"] = std::move(methods);
	result["notes"] = Json::array({
		"InspectGameViewport() and SetGameCamera() run on the draw tick, deferred like world.capture, so the reported camera is the frame the game draws next.",
		"world.camera.get and a world.project projection are reads and run while the game is paused; they report the real viewport, zoom and scroll position, and project a tile to the screen or a screen pixel to a tile.",
		"world.project is a projection only when it carries no `op` and names a tile, a town or a screen pixel; `op` stays with the administrative module (power, power_fuel, station_town, district).",
		"world.camera.set is a write of the viewport state: it takes the single-player gate, the task barrier and expected_refs, checked when it is scheduled and again on the draw tick.",
	});
	return result;
}

/**
 * The contracts of the methods whose answer is more than the data: how to guard a pause against
 * a newer decision, and what the timing numbers actually measure. They are sent with the
 * capability document so the environment does not have to guess either of them.
 * @return The contracts document.
 */
Json CoreContracts()
{
	Json contracts = Json::object();
	contracts["pause"] = {
		{"revision_source", "every accepted change through Commands::Pause, whoever posted it"},
		{"read", "world.info.pause = {paused, normal, revision, revision_at_epoch_start, epoch}"},
		{"write", "world.pause {paused, expected_revision?} = {paused, normal, revision, changed, previous{paused, normal, revision}}"},
		{"fields", "`paused` is any pause reason, `normal` is the pause this tool controls, `changed` says whether a pause command actually ran"},
		{"stale", "world.pause answers error=stale_pause_revision with reason=revision_mismatch or epoch_changed, before anything is posted"},
		{"no_attribution", "the revision does not say who paused; do not infer a player or agent source from it"},
		{"restore", "for a temporary pause: keep previous.normal and the revision your own successful pause returned, then restore previous.normal with expected_revision set to that returned revision; never refresh the guard before restoring, so a newer player decision is rejected instead of undone"},
		{"epoch", "the world_id/epoch of the request is the real protection across a load; a revision below revision_at_epoch_start is only an additional check, and an equal revision can also be the previous epoch's when no pause command ran since"},
	};
	contracts["calendar"] = {
		{"read", "world.info.calendar = {year, month, day, date_iso}"},
		{"source", "the engine's own TimerGameCalendar::ConvertDateToYMD() and STR_JUST_DATE_ISO, so the text and the numbers cannot disagree"},
		{"fields", "`year` is the engine's calendar year (what the status bar shows), `month` is 1..12 and `day` is 1..31"},
		{"legacy", "world.info.date stays the raw day counter; it is not a date a player recognises, so do not show it"},
	};
	contracts["refs"] = {
		{"identity_kinds", "pool (id + generation), tile (tile + fingerprint, plus town_id/town_generation for a house), point (location only, never stale)"},
		{"tile_types", "terrain, water, tree, rail, signal, road, tram, bridge, tunnel, mapobject, house; `tile` is the generic spelling and takes `what`"},
		{"tile_fields", "type, what, subtype, identity, tile, owner, fingerprint"},
		{"owner", "the owner the engine records for that category: rail, signal, bridge, tunnel, water and mapobject use the tile owner; road and tram use the owner of their own transport type, so the road and the tram on one tile can report different owners; terrain, tree and house report null; OWNER_WATER (open sea) and OWNER_NONE report null"},
		{"house_fields", "a house adds town_id and town_generation: the identity is the tile (the stored house id, its translated HouseID and the random bits) plus the lifetime of its town, never the whole town. A house has no pool generation of its own, so a same-type reconstruction on the same tile in the same town with the same random bits is not distinguishable"},
		{"mapobject_fields", "a map object adds object_id and object_generation: the pooled object behind the tile is part of the identity, so replacing it with one of the same type is a different identity"},
		{"point_fields", "type=point, identity=point, tile; no fingerprint, so a point names a location and can never be stale"},
		{"fingerprint", "FNV-1a over the native tile values that identify the object and its state (tile type, height, the category fields, the recorded owner and both halves of every 64 bit pool lifetime), not over map-array bytes and not over an address"},
		{"signal_note", "signal states are changed by passing trains, so a signal reference can go stale without anyone building anything: inspect again instead of treating it as an error"},
		{"write_rule", "expected_refs of a write must name a generation for a pooled type, and a fingerprint plus town_generation (house) or object_generation (map object) for a tile object; a point needs nothing"},
		{"stale", "a stale reference answers found=false with error=stale_ref and a reason: world_mismatch, epoch_mismatch, deleted, generation_required, generation_mismatch, tile_type_mismatch, tile_state_changed, fingerprint_required, town_generation_required, town_lifetime_mismatch, object_generation_required, object_lifetime_mismatch"},
		{"invalid", "error=invalid_params with reason=invalid_ref, missing_type, invalid_id, invalid_tile, invalid_fingerprint, invalid_generation or unknown_what"},
		{"unsupported", "a type the kernel cannot resolve answers error=unsupported_ref_type with reason=unknown_type or not_a_pooled_object (a cargo has no lifetime)"},
		{"bounded", "a tile reference reads only the requested tile: no map scan and no render"},
	};
	contracts["timing"] = {
		{"read", "world.info.timing = {unit, clock, scope, enforced, since_epoch, queries, commands, batches, deferred}"},
		{"measured", "game-thread slices: one inline request execution (reads and writes separately), one poll's whole request batch, or one deferred draw-tick job"},
		{"not_measured", "HTTP latency, poll cadence and queue waiting"},
		{"budget", "a threshold that counts overruns; nothing is aborted when it is exceeded"},
		{"ordering", "a world.info answer reports the executions that finished before it"},
	};

	return contracts;
}

/**
 * The kernel methods of this file, described the way the tool modules describe theirs.
 *
 * world.query is answered by two modules: the game tool module owns vehicles, engines, orders,
 * groups and cargos, and this module owns the kinds the kernel iterates itself, so the gap one
 * module reports is never the claim of the other. The contracts of the methods whose answer is
 * more than the data travel with this document, so the hello detail carries them and the
 * environment does not have to guess how to guard a pause or what the timing numbers measure.
 * @return Capability document of the kernel methods.
 */
Json CoreCapabilities()
{
	Json methods = Json::array();
	methods.push_back({
		{"method", "world.query"},
		{"authority", "read"},
		{"implemented", Json::array({"towns", "companies", "stations", "industries", "signs", "tiles"})},
		{"gaps", Json::array()},
	});

	Json result = Json::object();
	result["module"] = "agent_bridge_core";
	result["methods"] = std::move(methods);
	result["contracts"] = CoreContracts();
	result["notes"] = Json::array({
		"world.query kind=towns, companies, stations, industries, signs and tiles is the kernel read; the game tool module answers vehicles, engines, orders, groups and cargos, so the two documents together describe the whole query.",
		"These kinds are read through the engine's own Iterate() accessors and never written, so the agent can verify a command's effect against the next read.",
		"The pause, calendar, refs and timing contracts are in this document, because the kernel methods they describe are the ones this module answers.",
	});
	return result;
}

/**
 * The methods the engine can actually serve right now.
 *
 * Composed from the core methods of this file and from the capability documents of the tool
 * modules: a method is only advertised when a module really implements at least one of its
 * operations, so a gap is never advertised as a capability. The per-module documents are sent
 * along as detail, so the backend and the operator can see what is missing; the contracts travel
 * in the core module document, because the hello detail carries only the modules.
 * @return {methods, modules, contracts}: the advertised method names, the module documents and
 * the contracts of the methods whose answer is more than the data.
 */
Json Capabilities()
{
	std::vector<std::string> methods = {"world.info", "world.query", "world.objects", "world.pause",
			"world.rename", "world.capture", "world.camera.get", "world.camera.set", "world.commands",
			"world.command", "task.barrier", "ui.inspect"};
	Json modules = Json::array();

	for (const Json &module : {CoreCapabilities(), BlueprintToolCapabilities(), GameToolCapabilities(), TransportToolCapabilities(), AdminToolCapabilities(), CameraCapabilities(), CommandCatalogCapabilities()}) {
		modules.push_back(module);
		const auto entries = module.find("methods");
		if (entries == module.end() || !entries->is_array()) continue;
		for (const Json &entry : *entries) {
			const auto implemented = entry.find("implemented");
			if (implemented == entry.end() || !implemented->is_array() || implemented->empty()) continue;
			const std::string name = JsonString(entry, "method");
			if (name.empty()) continue;
			if (std::find(methods.begin(), methods.end(), name) == methods.end()) methods.push_back(name);
		}
	}

	Json advertised = Json::array();
	for (const std::string &method : methods) advertised.push_back(method);

	Json contracts = CoreContracts();

	return {{"methods", std::move(advertised)}, {"modules", std::move(modules)}, {"contracts", std::move(contracts)}};
}

/** Send a hello for the current world and epoch. */
void SendHello()
{
	ResolveWorldId();
	_state.hello_in_flight = true;

	const Json capabilities = Capabilities();
	const Json body = {
		{"engine_id", _state.engine_id},
		{"world_id", _state.world_id},
		{"epoch", _state.epoch},
		{"build", std::string(_openttd_revision)},
		{"capabilities", capabilities["methods"]},
		{"capability_detail", capabilities["modules"]},
	};

	Send(RequestKind::Hello, "/v1/engine/hello", body.dump(), _state.engine_token, _state.epoch, nullptr);
}

/** Send a poll for the current world and epoch. */
void SendPoll()
{
	ResolveWorldId();
	_state.poll_in_flight = true;

	const Json body = {
		{"connection_id", _state.connection_id},
		{"world_id", _state.world_id},
		{"epoch", _state.epoch},
		{"results", Json::array()},
		{"cursor", _state.cursor},
	};

	Send(RequestKind::Poll, "/v1/engine/poll", body.dump(), _state.engine_token, _state.epoch, nullptr);
}

/** Deliver the next queued result, if any. */
void SendNextResult()
{
	if (_state.results.empty() || _state.result_in_flight) return;
	if (Clock::now() < _state.next_result) return;

	_state.inflight_result = _state.results.front();
	_state.results.pop_front();
	_state.result_in_flight = true;

	const Json body = {
		{"connection_id", _state.connection_id},
		{"request_id", _state.inflight_result["request_id"]},
		{"world_id", _state.world_id},
		{"epoch", _state.epoch},
		{"result", _state.inflight_result["result"]},
	};

	Send(RequestKind::Result, "/v1/engine/result", body.dump(), _state.engine_token, _state.epoch, nullptr);
}

} // namespace

/**
 * Run one step of the bridge. Must be called from the main thread once per
 * game loop iteration; it also runs while the game is paused.
 */
void Tick()
{
	if (!EnsureInitialised()) return;
	if (!_network_available) return;

	/* Make sure responses are handled even when the network background loop is not running. */
	NetworkHTTPSocketHandler::HTTPReceive();
	ProcessCompletions();

	/*
	 * The bridge only talks about a world that is actually loaded and not being
	 * generated or loaded: no hello, poll or result may describe a half built
	 * world. Completions above are still drained, so nothing is lost meanwhile.
	 */
	if (!IsWorldReady()) return;

	const Clock::time_point now = Clock::now();

	if (!_state.connected) {
		if (!_state.hello_in_flight && now >= _state.next_hello) SendHello();
		return;
	}

	if (!_state.poll_in_flight && now >= _state.next_poll) SendPoll();
	SendNextResult();
}

/**
 * Forget everything about the current world and start a fresh epoch. Called
 * whenever a new game is started or a savegame is loaded.
 */
void ResetWorld()
{
	_state.world_id.clear();
	_state.epoch = GenerateUuid();
	/* A pause revision read in another epoch can never guard a pause in this one. */
	_state.pause_revision_at_epoch = PauseRevision();
	_state.query_timing = TimingStats{};
	_state.command_timing = TimingStats{};
	_state.batch_timing = TimingStats{};
	_state.deferred_timing = TimingStats{};
	_state.connection_id.clear();
	_state.connected = false;
	_state.cursor = 0;
	_state.heartbeat_seconds = 5;

	_state.hello_in_flight = false;
	_state.poll_in_flight = false;
	_state.result_in_flight = false;
	_state.next_hello = Clock::now();
	_state.next_poll = Clock::now();
	_state.next_result = Clock::now();

	_state.results.clear();
	_state.inflight_result = Json();
	_state.receipts.clear();
	_state.receipt_order.clear();
	_state.barrier_tasks.clear();
	/* Requests waiting for the draw tick belong to the world that is gone; their callbacks find nothing and drop them. */
	_deferred.clear();
	/* Requests in flight are kept: their completions are ignored by epoch, and UI callbacks still get their answer. */
}

/**
 * Identity of the world the bridge is currently bound to.
 * @return {world_id, epoch, connection_id, connected}.
 */
Json Context()
{
	EnsureInitialised();
	ResolveWorldId();

	return {
		{"world_id", _state.world_id},
		{"epoch", _state.epoch},
		{"connection_id", _state.connection_id},
		{"connected", _state.connected},
	};
}

/**
 * Describe the objects attached to a tile, as typed attachment candidates.
 *
 * Every entry carries the generation of the object and the world/epoch it was
 * read in, so a later request can name the exact object it was planned for and
 * a reused id is detected instead of silently resolved.
 * @param tile The tile to inspect.
 * @return Array of {type, id?, generation?, name?, tile?, world_id, epoch} entries, most specific first.
 */
Json PickObjects(TileIndex tile)
{
	Json objects = Json::array();
	if (!IsWorldReady()) return objects;
	if (!IsValidTile(tile)) return objects;

	const TileType type = GetTileType(tile);
	const Json location = {{"x", TileX(tile)}, {"y", TileY(tile)}};
	const Json binding = WorldBinding();

	auto add = [&](Json entry) {
		entry.update(binding);
		objects.push_back(std::move(entry));
	};

	/* Pooled objects the tile carries, most specific first. Their identity is the pool lifetime. */
	if (type == TileType::Station) {
		/* A waypoint, a buoy and a station share the station pool but are separate protocol types. */
		if (const Waypoint *wp = Waypoint::GetByTile(tile); wp != nullptr) {
			add({{"type", "waypoint"}, {"identity", "pool"}, {"id", wp->index.base()}, {"generation", wp->generation},
					{"name", wp->GetCachedName()}, {"what", std::string(StationKindName(GetStationType(tile)))}, {"tile", location}});
		} else if (const Station *st = Station::GetByTile(tile); st != nullptr) {
			add({{"type", "station"}, {"identity", "pool"}, {"id", st->index.base()}, {"generation", st->generation},
					{"name", st->GetCachedName()}, {"what", std::string(StationKindName(GetStationType(tile)))}, {"tile", location}});
		}
	} else if (type == TileType::Industry) {
		if (const Industry *ind = Industry::GetByTile(tile); ind != nullptr) {
			add({{"type", "industry"}, {"identity", "pool"}, {"id", ind->index.base()}, {"generation", ind->generation}, {"tile", location}});
		}
	}

	/* A rail, road or ship depot is a pool object of its own. A hangar is not: see the gap below. */
	if (IsRailDepotTile(tile) || IsRoadDepotTile(tile) || IsShipDepotTile(tile)) {
		if (const Depot *depot = Depot::GetByTile(tile); depot != nullptr) {
			add({{"type", "depot"}, {"identity", "pool"}, {"id", depot->index.base()}, {"generation", depot->generation},
					{"name", depot->name}, {"what", std::string(TileTypeName(GetTileType(depot->xy)))}, {"tile", location}});
		}
	}

	/* Every vehicle standing on the tile: it is a pool object with its own lifetime. */
	for (const Vehicle *v : Vehicle::Iterate()) {
		if (v->tile != tile) continue;
		add({{"type", "vehicle"}, {"identity", "pool"}, {"id", v->index.base()}, {"generation", v->generation},
				{"name", v->name}, {"what", std::string(VehicleKindName(v->type))}, {"tile", location}});
	}

	/* Every sign on the tile. Signs are stored by world coordinate, not by tile. */
	for (const Sign *sign : Sign::Iterate()) {
		const TileIndex sign_tile = TileVirtXY(static_cast<uint>(std::max(0, sign->x)), static_cast<uint>(std::max(0, sign->y)));
		if (sign_tile != tile) continue;
		add({{"type", "sign"}, {"identity", "pool"}, {"id", sign->index.base()}, {"generation", sign->generation},
				{"name", sign->name}, {"tile", location}});
	}

	/*
	 * The objects the tile itself carries, as typed categories with a state fingerprint: a house
	 * (with the lifetime of its town), rail, signal, road, tram, bridge, tunnel, mapobject, tree,
	 * water or terrain. Each is identified by the tile and its fingerprint, not by a pool index.
	 */
	const Json tile_objects = BuildTileObjectRows(tile);
	for (const Json &object : tile_objects) {
		Json entry = object;
		add(std::move(entry));
	}

	/*
	 * A tile that carries no typed landscape object of its own (a station, an industry, the void at
	 * the map edge) stays selectable as a location, which is the honest answer for it: the tile is
	 * not an object, and a point reference names the location only. `cargo` is never offered: a
	 * cargo packet is not attached to a tile but to a vehicle or a station.
	 */
	if (tile_objects.empty()) {
		add({{"type", "point"}, {"identity", "point"}, {"what", std::string(TileTypeName(type))}, {"tile", location}});
	}
	return objects;
}

/**
 * Describe the typed objects a tile carries, for the native UI and for tests.
 * @param tile The tile to describe.
 * @return Array of typed tile object rows.
 */
Json DescribeTileObjects(TileIndex tile)
{
	return BuildTileObjectRows(tile);
}

/**
 * Resolve one typed reference against the current world, as `world.objects` does.
 * @param ref The reference.
 * @param require_identity True for a write's `expected_refs`.
 * @return The resolved object, or a stale_ref entry.
 */
Json ResolveObjectRef(const Json &ref, bool require_identity)
{
	return ResolveRef(ref, require_identity);
}

/**
 * Send a request to the backend on behalf of the native UI. The UI token is
 * used for authentication, and the callback is always invoked on the main
 * thread.
 * @param method HTTP method, e.g. "GET" or "POST".
 * @param path Path below the backend base URL, e.g. "/v1/chats".
 * @param body Request body; ignored for GET.
 * @param callback Called with the parsed response and whether it succeeded.
 */
void Request(std::string method, std::string path, Json body, std::function<void(Json, bool)> callback)
{
	if (!EnsureInitialised() || !_state.enabled) {
		if (callback) callback(Json::object(), false);
		return;
	}

	std::string payload;
	if (method != "GET" && !body.is_null()) payload = body.dump();

	Send(RequestKind::Ui, path, std::move(payload), _state.ui_token, _state.epoch,
			[cb = std::move(callback)](bool ok, const Json &response) { if (cb) cb(response, ok); });
}

} // namespace InGameAgent
