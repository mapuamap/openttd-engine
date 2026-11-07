/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/>.
 */

/** @file agent_admin_tools.h Administrative and game-settings tool methods the agent bridge dispatches onto real commands. */

#ifndef AGENT_ADMIN_TOOLS_H
#define AGENT_ADMIN_TOOLS_H

#include "3rdparty/nlohmann/json.hpp"
#include "command_type.h"
#include "company_type.h"
#include "economy_type.h"
#include "signs_type.h"
#include "tile_type.h"
#include "town_type.h"

#include <optional>
#include <string_view>

/** Administrative and game-settings tool methods the agent bridge dispatches onto real OpenTTD commands. */
namespace InGameAgent {

/** JSON value type used by the agent tool protocol. */
using Json = nlohmann::json;

/**
 * Describe the administrative tool methods this module implements, and the parts it does not.
 *
 * The bridge uses this to advertise capabilities and to answer a call for an operation that has no
 * implementation yet with an explicit gap instead of a fake success.
 *
 * @return Capability document: implemented methods/ops plus the known gaps.
 */
Json AdminToolCapabilities();

/**
 * Dispatch one administrative tool call onto the engine.
 *
 * The bridge validates the world, the epoch, the task and the expected references before it calls
 * this; the module validates the ids, the enums and the bounds of the call itself before it posts
 * anything. Every mutation goes through Command<Commands::...>::Post, so the engine's own
 * eligibility, pause and finance checks apply and the reported cost is the real one.
 *
 * @param method The method name, e.g. "world.settings.set".
 * @param params The method parameters.
 * @param company The company the call is made for; used as the default company of the call.
 * @return The result document, or std::nullopt when the method does not belong to this module.
 */
std::optional<Json> DispatchAdminTool(std::string_view method, const Json &params, CompanyID company);

/**
 * Does this call change the world?
 *
 * The bridge needs this to decide whether a call needs a task and expected references. The query
 * operations of the administrative methods are not mutations: world.settings.get, world.company
 * op=info/livery, world.towns op=needs/growth/settings/power/list, world.industry
 * op=list/info/needs/production/cargo, world.signs op=list/info, world.landscape op=object_types
 * and world.project op=power/district/catchment/settings.
 *
 * @param method The method name.
 * @param params The method parameters.
 * @return True when the call is expected to change the world.
 */
bool IsAdminMutation(std::string_view method, const Json &params);

/**
 * Callback of every command this module posts that returns only a cost.
 *
 * The command path is synchronous in single player, so the result is available right after Post
 * returns. The callback is registered in the network command callback table.
 *
 * @param cmd The command that ran.
 * @param result The result the engine reported.
 * @param tile The tile the command ran on.
 */
void AgentAdminCommandCallback(Commands cmd, const CommandCost &result, TileIndex tile);

/**
 * Callback of Commands::FoundTown, which also returns the cost of the town and the new town id.
 *
 * The parent registers this in the network command callback table next to AgentAdminCommandCallback;
 * the module cannot register it itself without touching the network command file.
 *
 * @param cmd The command that ran.
 * @param result The result the engine reported.
 * @param cost The price of the founded town.
 * @param town_id The id of the new town, or an invalid id when the command failed.
 */
void AgentAdminFoundTownCallback(Commands cmd, const CommandCost &result, Money cost, TownID town_id);

/**
 * Callback of Commands::PlaceSign, which also returns the id of the new sign.
 *
 * The parent registers this in the network command callback table next to AgentAdminCommandCallback.
 *
 * @param cmd The command that ran.
 * @param result The result the engine reported.
 * @param sign_id The id of the new sign, or an invalid id when the command failed.
 */
void AgentAdminPlaceSignCallback(Commands cmd, const CommandCost &result, SignID sign_id);

/**
 * Callback of Commands::BuildIndustryArea, which also returns the number of industries it built.
 *
 * The parent registers this in the network command callback table next to AgentAdminCommandCallback.
 *
 * @param cmd The command that ran.
 * @param result The result the engine reported.
 * @param count The number of industries the command built.
 */
void AgentAdminBuildIndustryAreaCallback(Commands cmd, const CommandCost &result, uint32_t count);

} // namespace InGameAgent

#endif /* AGENT_ADMIN_TOOLS_H */
