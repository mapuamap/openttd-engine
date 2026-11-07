/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/>.
 */

/** @file agent_transport_tools.h Native construction tool methods the agent bridge dispatches onto real commands. */

#ifndef AGENT_TRANSPORT_TOOLS_H
#define AGENT_TRANSPORT_TOOLS_H

#include "3rdparty/nlohmann/json.hpp"
#include "command_type.h"
#include "company_type.h"

#include <optional>
#include <string_view>

/** Native construction tool methods the agent bridge dispatches onto real OpenTTD commands. */
namespace InGameAgent {

/** JSON value type used by the agent tool protocol. */
using Json = nlohmann::json;

/**
 * Describe the construction tool methods this module implements, and the families it does not.
 *
 * The bridge uses this to advertise capabilities and to answer a call for a family that has no
 * implementation yet with an explicit gap instead of a fake success.
 *
 * @return Capability document: implemented methods/ops plus the known gaps.
 */
Json TransportToolCapabilities();

/**
 * Dispatch one engine method onto the real OpenTTD commands.
 *
 * The caller owns authentication, task admission, the world epoch and the `expected_refs` check;
 * this function only runs the command and reports what the engine actually did. Every company
 * context change is scoped to the call and restored afterwards.
 *
 * @param method Engine method name, e.g. `world.construct`.
 * @param params Method parameters as sent by the backend.
 * @param company Company that pays and owns the result; must be a valid company for write methods.
 * @return The tool result, or `std::nullopt` when the operation is not owned by this module, so
 *         the bridge can fall through to the module that does own it.
 */
std::optional<Json> DispatchTransportTool(std::string_view method, const Json &params, CompanyID company);

/*
 * Command callback. `Command<Tcmd>::Post` only accepts callbacks that are registered for the
 * network command dispatch, so this is declared here and registered in `network_command.cpp`:
 * `_callback_tuple` has to list `&InGameAgent::CcAgentTransportCommand` next to the engine's own
 * callbacks, and that file has to include this header. It captures the actual result of the
 * command that was just posted; in single player the callback runs synchronously, before `Post`
 * returns.
 *
 * One `CommandCallbackData` callback serves every command this module posts: it receives the
 * command id and the real `CommandCost`, which is all the receipts need. Generated ids (station,
 * depot, waypoint) are read back from the map after the command, not decoded from the argument
 * buffer. Every op added here reuses that one callback, so no further registration is needed.
 */

/**
 * Capture the cost of a posted command.
 * @param cmd The command that ran.
 * @param result The actual cost, or the failure of the command.
 * @param data The command arguments, unused.
 * @param result_data The command return values, unused.
 */
CommandCallbackData CcAgentTransportCommand;

} // namespace InGameAgent

#endif /* AGENT_TRANSPORT_TOOLS_H */
