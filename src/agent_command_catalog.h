/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/gpl-2.0>.
 */

/** @file agent_command_catalog.h Typed catalog of every engine command and a generic normal-command dispatcher. */

#ifndef AGENT_COMMAND_CATALOG_H
#define AGENT_COMMAND_CATALOG_H

#include "3rdparty/nlohmann/json.hpp"
#include "command_type.h"
#include "company_type.h"
#include "tile_type.h"

#include <optional>
#include <string_view>

/**
 * Catalog of every engine command (`Commands`), plus a generic dispatcher that turns ordered typed
 * JSON arguments into a real `Command<Tcmd>::Post` of the engine's normal player command path.
 *
 * The catalog is generated from the engine itself: it walks `Commands` from 0 to `Commands::End`
 * and reads `CommandTraits<Tcmd>` for each, so the number of entries can never drift from the
 * engine's own command table. Nothing here is a hand written list of commands.
 *
 * ## Methods
 *
 * `world.commands` (read, no company needed):
 *   `{"op": "list"}` - one compact entry per engine command.
 *   `{"op": "describe"}` - every command with its full argument schema.
 *   `{"op": "describe", "command": <name|id>}` - one command with its full argument schema.
 *   `{"op": "validate", "command": <name|id>, "args": [...], "expected_targets": [...],
 *     "company": N}` - decode one command's arguments without running it; the result reports the
 *     arguments and the entities they name. With `expected_targets` it applies exactly the guard a
 *     write applies (complete set equality, see below); with `company` the actor company is part of
 *     that guard too, so the check is bit for bit the one `world.command` will run. A gap command
 *     answers `unsupported_argument_types`, bad arguments `invalid_arguments`, an unsatisfied guard
 *     `target_mismatch`, an absent guard is allowed here (the result is the target list the bridge
 *     has to send with the write).
 *
 * A command entry looks like:
 * @code{.json}
 * {
 *   "id": 12,                          // numeric Commands value, 0 .. Commands::End - 1
 *   "name": "CmdBuildRoad",            // CommandTraits<Tcmd>::name, i.e. the proc name
 *   "alias": "BuildRoad",              // the same name without the leading "Cmd"
 *   "type": "LandscapeConstruction",   // CommandType
 *   "flags": ["Auto", "NoWater", "Deity"], // CommandFlag names
 *   "location_argument": false,        // CommandFlag::Location: Post needs an explicit tile
 *   "pause": {"allowed_when_paused": true},
 *   "company": {"server_only": false, "spectator_allowed": false, "deity_allowed": true,
 *               "offline_only": false, "needs_client_id": false},
 *   "support": {"supported": true, "status": "dispatchable"},
 *   "targets": [                       // the entities the arguments name, for expected refs
 *     {"index": 0, "target": {"entity": "town", "id_path": null}}
 *   ],
 *   "target_scope": "entities",        // "company" when no argument names an entity
 *   "expected_refs": {                 // the contract the bridge builds its refs from
 *     "required_for_write": true, "set_equality": true, "includes_actor_company": true,
 *     "sources": [ ... ]               // same descriptors as "targets"
 *   },
 *   "arguments": [                     // only for op=describe
 *     {
 *       "index": 0, "name": null, "type": "TileIndex", "kind": "strong_id", "supported": true,
 *       "target": {"entity": "town", "id_path": null}, // null when the argument names no entity
 *       "schema": null                 // the JSON schema of a structured argument, else null
 *     }
 *   ],
 *   "returns": ["Money"]               // only for op=describe; extra typed returns of the proc
 * }
 * @endcode
 *
 * ## Entities the arguments name
 *
 * The bridge owns the authority; the catalog says exactly which existing objects a command will
 * touch, so the bridge can derive its expected refs from the arguments it already observed.
 * Every argument whose type is an object id carries `target`, and every such argument appears in the
 * command's `targets` list with its index:
 *   - `{"entity": "town", "id_path": null}` - the value itself is the id (town, station, vehicle,
 *     industry, group, sign, depot, company, engine; waypoints are stations, aircraft hangars are
 *     stations);
 *   - `{"entity_by": {"field": field, "map": {value: kind}}, "id_path": field}` - the id sits at
 *     `id_path` inside the structured argument and the entity kind follows `entity_by.field` through
 *     that map. `Order` uses `destination_entity`, `Source` uses `type`, `VehicleListIdentifier` uses
 *     `list`, a variant uses `type`.
 * `target_scope` is `company` for commands that only act on the company itself (build commands).
 *
 * ## The guard: expected_targets is complete, not advisory
 *
 * A write has to send `expected_targets`: the `{entity, id}` pairs the bridge resolved and guarded.
 * The catalog checks set equality in both directions before it estimates or posts:
 *   - every entity the typed arguments name must be in the list (`detail.missing` otherwise), so a
 *     write can never act on an object the bridge did not resolve - the bridge only checks the refs
 *     it was given, so an unchecked argument would be an unguarded write;
 *   - every listed entity must be named by the arguments (`detail.unexpected` otherwise), so an
 *     unrelated valid ref cannot authorize a reused target id.
 * The actor company is always an entity target (`via: "company_context"`), so it has to be guarded
 * too; for a command with no entity argument it is the only one. `target_mismatch` carries
 * `detail.targets`, `detail.missing` and `detail.unexpected`; `missing_expected_targets` means the
 * list was absent altogether. Ids are bound to the actual argument values (nested `Order`
 * destinations included), not just to their entity kind.
 *
 * ## Exact schemas of the structured arguments
 *
 * `EncodedString` (kind `encoded_string`): either a plain JSON string, which is encoded through the
 * engine's own raw-text string (`STR_JUST_STRING`) with the text as its parameter, so it can never be
 * read as a control code; or `{"string_id": N, "params": [...]}` with parameters as a string, an
 * integer, `{"text": string}` or `{"id": integer}`. `{"text": ...}` and `{"string_id": ...}` are
 * mutually exclusive and `text` takes no `params`.
 *
 * `Order` (kind `order`): `{"type": name, "destination": N}` plus the optional fields of that type.
 *   - `goto_station` / `goto_waypoint` / `implicit`: `destination` is a StationID.
 *   - `goto_depot`: `destination` is a DepotID, plus optional `depot_order` and `action` bitsets.
 *     An aircraft hangar is a station, not a depot: say `"destination_entity": "station"` and the id
 *     is guarded as a station. `destination_entity` is otherwise optional and defaults to the id kind
 *     of the order type; a value that contradicts the type is rejected.
 *   - `loading`: optional `ordered`; `leave_station` and `dummy`: no destination.
 *   - optional for all: `non_stop`, `stop_location`, `load`, `unload`, `refit_cargo`, `wait_time`,
 *     `travel_time`, `max_speed`. Unknown fields are rejected, so a typo cannot build another order.
 *   - built with the engine's own `Order::Make*` helpers; the normalized value carries
 *     `destination_entity` (`station` / `depot` / `waypoint`) for the target metadata.
 *
 * `Source` (kind `source`): `{"type": "town"|"industry"|"headquarters", "id": N}`; 65535 (the
 * engine's invalid marker) is rejected.
 *
 * `VehicleListIdentifier` (kind `vehicle_list`): `{"list": "company"|"vehicle_shared_orders"|
 * "station"|"depot"|"group", "vtype": VehicleType, "company": CompanyID, "index": N}`; `index` is
 * the id the list is about and is reported as a target of the matching entity kind.
 *
 * `std::variant` (kind `variant`; the news reference of `CmdCustomNewsItem`): `{"type": name,
 * "value": value}` names the alternative and carries its value, `{"type": "none"}` is the engine's
 * `std::monostate`. The alternatives are the engine's own (tile, vehicle, station, industry, town,
 * engine), and the id is reported as a target of that kind.
 *
 * `BlueprintChunkData` has no trusted JSON form: `CmdPasteBlueprint` and `CmdPrepareRailRoute` take
 * the chunk bytes `PlanBlueprintPaste` builds from a `Blueprint` (offsets, tracks, signals, depots,
 * tunnels/bridges, railtypes, rotation/mirror), so a raw blob would be an unchecked bypass and is not
 * accepted. They stay an explicit gap (`support.status: "gap"`) until the blueprint worker produces
 * the chunks from a high level description; the next step there is a semantic `Blueprint` schema that
 * feeds `PlanBlueprintPaste`, not a byte array.

 * Enums are checked against the engine's own end markers where one exists (`VehicleType`, `NewsType`,
 * `GoalQuestionType`), so a value that fits the storage but is not an enumerator is rejected;
 * `EnumBitSet` arguments are checked against the bitset's own mask. Enums without an end marker are
 * only range checked against their storage type, which `schema.values` states per argument.
 *
 * `support.status` is `dispatchable` when the generic decoder can build every argument of the
 * command, and `gap` otherwise; a gap entry carries `support.reason` naming the arguments whose
 * types have no JSON decoding yet.
 *
 * `world.command` (write, needs a valid company):
 *   `{"command": <name|id>, "args": [...], "dry_run": false, "location": <tile>}`
 *   - `command`: proc name (`CmdBuildRoad`), alias (`BuildRoad`) or numeric id, case insensitive.
 *   - `args`: ordered positional arguments; the order and types are the ones `op=describe` reports.
 *   - `dry_run`: estimate only (`DoCommandFlag::QueryCost`, no execution).
 *   - `location`: tile for commands whose `location_argument` is true; defaults to 0 (no location).
 *   - `expected_targets` (required): `[{"entity": kind, "id": N}]`, the entities the bridge has
 *     resolved and guarded, complete. The call is rejected before `Post` with
 *     `missing_expected_targets` when the list is absent and with `target_mismatch` unless the typed
 *     arguments name exactly those entities (both directions, nested `Order` destinations and the
 *     actor company included). See "The guard" above.
 *   Result: `{ok, command, id, dry_run, cost, expense_type, arguments, targets, guard}` (`guard` is
 *   always `checked`, because a write cannot run unguarded; a dry run also reports
 *   `estimate_reliable`, which is false for the
 *   commands the engine cannot estimate - `CommandFlag::NoEst` - or whose test run may change state -
 *   `CommandFlag::NoTest`, exactly as in the engine's own GUI); on rejection by the engine
 *   also `error: "command_failed"`, `message` and `message_id` (the engine's own error StringID).
 *   Errors before the command runs: `unknown_command`, `unsupported_argument_types` (a catalog gap),
 *   `invalid_arguments` (wrong arity, wrong type, overflow, bits outside the enum),
 *   `command_not_run` (Post refused before the proc ran, e.g. the game is paused for it),
 *   `missing_expected_targets`, `target_mismatch`, `invalid_company`, `multiplayer_unsupported`,
 *   `invalid_params`.
 *
 * Argument names are positional (`"name": null`): the engine's own headers carry them, but reading
 * them needs a generator. The smallest useful one emits `template <Commands> struct CommandArgNames;`
 * from the `*_cmd.h` prototypes (about 60 lines of parsing over a format the engine already enforces),
 * is committed as a generated header and checked by a build step; until then the catalog stays
 * positional and honest about it.
 *
 * ## Registration
 *
 * The module is registered by the agent bridge owner:
 *   - `src/agent_command_catalog.cpp` in the `src/CMakeLists.txt` source list,
 *   - `src/tests/test_agent_command_catalog.cpp` in `src/tests/CMakeLists.txt`,
 *   - `InGameAgent::AgentCommandCatalogCallback` in the `_callback_tuple` of
 *     `src/network/network_command.cpp` (`Command<Tcmd>::Post` asserts that its callback is
 *     network registered),
 *   - `DispatchCommandCatalog` in the bridge dispatch, `CommandCatalogCapabilities` in the
 *     bridge capability document.
 *
 * `world.command` is already classified as a write by the bridge (`IsMutation`), the catalog reads
 * run in parallel with the other read methods.
 */
namespace InGameAgent {

/** JSON value type used by the agent tool protocol. */
using Json = nlohmann::json;

/**
 * Describe the command catalog and the gaps that are still open.
 *
 * The counts and the list of undecodable argument types are computed from the engine's own command
 * table, so they cannot claim more than `DispatchCommandCatalog` actually accepts.
 *
 * @return Capability document: methods, command counts, supported argument kinds and known gaps.
 */
Json CommandCatalogCapabilities();

/**
 * Dispatch one catalog method.
 *
 * The caller owns authentication, task admission, the world epoch and the task's expected refs;
 * this function only runs the engine command and reports what the engine actually did. Every
 * company context change is scoped to the call and restored afterwards.
 *
 * Writes go through `Command<Tcmd>::Post`, i.e. the normal player command path, so the engine's own
 * pause, finance and eligibility checks apply; that path only exists in single player, so a write
 * in a network game is rejected instead of being executed behind the server's back. A `dry_run`
 * only asks for the cost (`DoCommandFlag::QueryCost`) and never executes.
 *
 * @param method Engine method name: `world.commands` or `world.command`.
 * @param params Method parameters as sent by the backend.
 * @param company Company that pays and owns the result; must be a valid company for `world.command`.
 * @return The method result, or `std::nullopt` when the method is not a catalog method at all.
 */
std::optional<Json> DispatchCommandCatalog(std::string_view method, const Json &params, CompanyID company);

/*
 * Command callback. `Command<Tcmd>::Post` only accepts callbacks that are registered for the
 * network command dispatch, so this one is declared here and listed in network_command.cpp. It has
 * the engine's common callback shape, which the engine also calls for commands whose proc returns
 * more than a `CommandCost`; the created ids of those typed returns are not reported (see the
 * `special_return_values` gap), only the actual `CommandCost` is. In single player the callback runs
 * synchronously, before `Post` returns.
 */

/**
 * Capture the actual cost of the command that was just posted.
 * @param cmd The command that ran.
 * @param result The actual cost, or the failure of the command.
 * @param tile Location the command was posted for.
 */
void AgentCommandCatalogCallback(Commands cmd, const CommandCost &result, TileIndex tile);

} // namespace InGameAgent

#endif /* AGENT_COMMAND_CATALOG_H */
