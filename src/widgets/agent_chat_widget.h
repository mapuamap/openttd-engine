/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file agent_chat_widget.h Types related to the in-game agent chat widgets. */

#ifndef WIDGETS_AGENT_CHAT_WIDGET_H
#define WIDGETS_AGENT_CHAT_WIDGET_H

/** Widgets of the #AgentChatWindow class. */
enum AgentChatWidgets : WidgetID {
	WID_AC_CAPTION,           ///< Caption of the window.
	WID_AC_CHAT_LIST,         ///< List of chats of the current world.
	WID_AC_CHAT_SCROLL,       ///< Scrollbar of the chat list.
	WID_AC_NEW_CHAT,          ///< Button to create a new chat.
	WID_AC_TRANSCRIPT,        ///< Transcript of the selected chat.
	WID_AC_TRANSCRIPT_SCROLL, ///< Scrollbar of the transcript.
	WID_AC_TRAY,              ///< Attachment tray of the message being composed.
	WID_AC_ADD_POINT,         ///< Button to add a map point attachment.
	WID_AC_ADD_OBJECT,        ///< Button to add a game object attachment.
	WID_AC_ADD_COMPANY,       ///< Button to add a company attachment.
	WID_AC_INPUT,             ///< Multiline message input.
	WID_AC_STATUS,            ///< Status line of the selected chat.
	WID_AC_SEND,              ///< Button to send the composed message.
	WID_AC_STOP,              ///< Button to stop the running task.
};

/** Widgets of the #AgentChatStatusWindow class. */
enum AgentChatStatusWidgets : WidgetID {
	WID_ACS_TEXT, ///< Status text, clicking it opens the chat window.
	WID_ACS_STOP, ///< Button to stop the running task.
};

#endif /* WIDGETS_AGENT_CHAT_WIDGET_H */
