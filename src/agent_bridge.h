/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file agent_bridge.h Native bridge between the running game and the in-game agent backend. */

#ifndef AGENT_BRIDGE_H
#define AGENT_BRIDGE_H

#include "3rdparty/nlohmann/json.hpp"
#include "tile_type.h"

#include <functional>
#include <string>

/**
 * Native bridge between the running game and the in-game agent backend.
 *
 * The bridge connects outward to the backend over authenticated HTTP, polls for
 * requests and executes them on the main thread. It is shared by the engine
 * (which owns the polling) and by the native chat UI (which uses Request()).
 */
namespace InGameAgent {

using Json = nlohmann::json;

/**
 * Run one step of the bridge. Must be called from the main thread once per
 * game loop iteration; it also runs while the game is paused.
 */
void Tick();

/**
 * Forget everything about the current world and start a fresh epoch. Called
 * whenever a new game is started or a savegame is loaded.
 */
void ResetWorld();

/**
 * Identity of the world the bridge is currently bound to.
 * @return {world_id, epoch, connection_id, connected}.
 */
Json Context();

/**
 * Describe the objects attached to a tile, as typed attachment candidates.
 *
 * Every entry carries the generation of the object and the world/epoch it was
 * read in, so a later request can name the exact object it was planned for: the
 * object the tile carries (station, waypoint or buoy, industry, the town of a
 * house, rail/road/ship depot), then every vehicle and every sign standing on it,
 * then the tile itself. The tile is named in the canonical `{"x": .., "y": ..}`
 * shape the protocol schema uses.
 * @param tile The tile to inspect.
 * @return Array of {type, id?, generation?, name?, what?, tile, world_id, epoch} entries, most specific first.
 */
Json PickObjects(TileIndex tile);

/**
 * Describe the typed objects the tile itself carries, most specific first.
 *
 * A tile object is identified by the tile and by a state fingerprint derived from the native tile
 * data, not by a pool index: `terrain`, `water`, `tree`, `rail`, `signal`, `road`, `tram`,
 * `bridge`, `tunnel`, `mapobject` and `house`. Every row carries `type`, `what`, `subtype`,
 * `identity` ("tile"), `tile`, `owner` and `fingerprint`. `owner` is the owner the engine records
 * for that category (rail, signal, bridge, tunnel, water and mapobject: the tile owner; road and
 * tram: the owner of their own transport type; terrain, tree and house: null). A house also
 * carries `town_id` and `town_generation` (the lifetime of its town is part of its identity, and a
 * house has no pool generation of its own), a map object carries `object_id` and
 * `object_generation` (the pooled object behind the tile). Only the requested tile is read, so a
 * call is bounded and never scans the map.
 *
 * The fingerprint is FNV-1a over the values the engine stores for the object (tile type and
 * height, the recorded owner, both halves of every 64 bit pool lifetime, plus the subtype specific
 * fields listed at the implementation), not over the bytes of the map array and not over an
 * address, so it is stable across runs for the same state. Signal states change with passing
 * traffic, so a signal reference can go stale without anyone building anything.
 * @param tile The tile to describe.
 * @return Array of typed tile object rows.
 */
Json DescribeTileObjects(TileIndex tile);

/**
 * Resolve one typed reference against the current world, as `world.objects` does.
 *
 * A reference is `{type, id?, tile?, generation?, fingerprint?, what?, town_generation?,
 * object_generation?, world_id?, epoch?}`. A read resolves the object as it is now and reports its
 * identity; a write (`require_identity`) additionally rejects a pooled reference without a
 * generation, a tile object without a fingerprint (or, for a house, without the generation of its
 * town, or for a map object without the generation of the pooled object behind it) and any
 * reference whose recorded state is no longer current. A `point` reference names a location only
 * and has no identity to check.
 * @param ref The reference.
 * @param require_identity True for a write's `expected_refs`.
 * @return The resolved object, or a stale_ref entry with `reason`.
 */
Json ResolveObjectRef(const Json &ref, bool require_identity);

/**
 * Send a request to the backend on behalf of the native UI. The UI token is
 * used for authentication, and the callback is always invoked on the main
 * thread.
 * @param method HTTP method, e.g. "GET" or "POST".
 * @param path Path below the backend base URL, e.g. "/v1/chats".
 * @param body Request body; ignored for GET.
 * @param callback Called with the parsed response and whether it succeeded.
 */
void Request(std::string method, std::string path, Json body, std::function<void(Json, bool)> callback);

} // namespace InGameAgent

#endif /* AGENT_BRIDGE_H */
