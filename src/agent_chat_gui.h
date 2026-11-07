/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file agent_chat_gui.h Interface of the native in-game agent chat window. */

#ifndef AGENT_CHAT_GUI_H
#define AGENT_CHAT_GUI_H

#include "3rdparty/nlohmann/json.hpp"

/**
 * Open the in-game agent chat window, or bring it to the front when it is already open.
 * The window is also reachable with the "agent_chat" hotkey (Ctrl+Shift+A by default) and
 * from the newspaper menu of the main toolbar.
 */
void ShowAgentChatWindow();

/**
 * Drive the agent chat: refresh the chat list, poll messages and events of the chats
 * with running tasks, and keep the status indicator in sync. The bridge itself is
 * ticked by the game loop, so this only polls the UI state.
 * Called from a realtime timer, so it runs whether or not the chat window is open.
 */
void AgentChatGuiTick();

/**
 * Read-only description of the agent chat UI, meant for GUI tests.
 *
 * The returned object contains the actual screen rectangles of the window widgets and of
 * the individual targets inside them (chat rows, attachment chips and their remove areas,
 * object disambiguation rows), so a test can locate a target and then inject real input.
 * Nothing in here executes an action or changes any state.
 *
 * Layout of the returned object:
 * - "coordinate_space": string, always "screen"; every rectangle in this object is in screen
 *   pixels, i.e. window-local widget rectangles already have the window position added.
 * - "hotkey": string, the hotkey that opens the window.
 * - "window_open": bool, whether the chat window is open.
 * - "window_rect": rectangle of the chat window itself, only present when it is open.
 * - "connected", "world_id", "epoch", "connection_id": transport state.
 * - "active_chat": string id of the selected chat, empty when there is none.
 * - "input": current text of the message input.
 * - "widgets": object mapping widget names to rectangles, only present when the window is open.
 *   Names are caption, chat_list, chat_scroll, new_chat, transcript, transcript_scroll, tray,
 *   add_point, add_object, add_company, input, status, send, stop and closebox (the close box has no widget
 *   id of its own, so it is looked up in the nested tree; Esc closes the window as well). The
 *   caption is the title bar, i.e. the area a real input test can drag the window by.
 * - "chats": array of {id, title, selected, task_active, status}. "task_active" stays true while
 *   the task of the chat is queued, running, stopping or settling, i.e. exactly while Stop is
 *   available; "status" is the translated status text of that task.
 * - "messages": array of {seq, role, text, status, attachments}.
 * - "tray": array of {type, id, generation, tile, name, world_id, epoch, stale, generation_known,
 *   rect, remove_rect}.
 * - "candidates": array of {type, id, generation, name, world_id, epoch, rect}.
 *
 * An attachment is {type, id, generation, name, tile, world_id, epoch, stale, generation_known}:
 * the identity of a typed reference is kept as the backend reported it. "tile" is the array
 * [x, y] in this description; the payload sent to the backend names the tile as the canonical
 * object {"x": x, "y": y}, and both shapes are accepted when a reference is read. "stale" marks a reference
 * that belongs to another world or epoch than the current one, and "generation_known" is false
 * for a pooled object (town, station, industry, vehicle, sign) whose generation the backend did
 * not report, because such an id may already belong to another object. Both cases are labelled
 * in the UI ("(stale)" / "(unverified)") and are still sent, the backend resolves them.
 * - "status_window": object {left, top, text, stop} with the position of the status window and
 *   the screen rectangles of its widgets, or null when it is not open.
 *
 * A rectangle is an object {left, top, right, bottom} in screen pixels.
 *
 * @return The description of the agent chat UI.
 */
nlohmann::json DescribeAgentChatUi();

#endif /* AGENT_CHAT_GUI_H */
