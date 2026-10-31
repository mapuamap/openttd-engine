/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file console_cmds.cpp Implementation of the console hooks. */

#include "stdafx.h"
#include "core/string_consumer.hpp"
#include "console_internal.h"
#include "blueprint.h"
#include "debug.h"
#include "engine_func.h"
#include "landscape.h"
#include "saveload/saveload_func.h"
#include "network/core/network_game_info.h"
#include "network/network.h"
#include "network/network_func.h"
#include "network/network_base.h"
#include "network/network_admin.h"
#include "network/network_client.h"
#include "command_func.h"
#include "settings_func.h"
#include "fios.h"
#include "fileio_func.h"
#include "fontcache.h"
#include "screenshot.h"
#include "genworld.h"
#include "string_func.h"
#include "strings_func.h"
#include "viewport_func.h"
#include "window_func.h"
#include "timer/timer.h"
#include "company_func.h"
#include "gamelog.h"
#include "ai/ai.hpp"
#include "ai/ai_config.hpp"
#include "newgrf.h"
#include "newgrf_profiling.h"
#include "console_func.h"
#include "engine_base.h"
#include "road.h"
#include "rail.h"
#include "game/game.hpp"
#include "3rdparty/fmt/chrono.h"
#include "company_cmd.h"
#include "misc_cmd.h"
#include "industry.h"
#include "town.h"
#include "town_cmd.h"
#include "town_growth.h"

#if defined(WITH_ZLIB)
#include "network/network_content.h"
#endif /* WITH_ZLIB */

#include "table/strings.h"

#include "safeguards.h"

/* scriptfile handling */
static uint _script_current_depth; ///< Depth of scripts running (used to abort execution when #ConReturn is encountered).

/* Scheduled execution handling. */
static std::string _scheduled_monthly_script; ///< Script scheduled to execute by the 'schedule' console command (empty if no script is scheduled).

/** Timer that runs every month of game time for the 'schedule' console command. */
static const IntervalTimer<TimerGameCalendar> _scheduled_monthly_timer = {{TimerGameCalendar::Trigger::Month, TimerGameCalendar::Priority::None}, [](auto) {
	if (_scheduled_monthly_script.empty()) {
		return;
	}

	/* Clear the schedule before rather than after the script to allow the script to itself call
	 * schedule without it getting immediately cleared. */
	const std::string filename = _scheduled_monthly_script;
	_scheduled_monthly_script.clear();

	IConsolePrint(CC_DEFAULT, "Executing scheduled script file '{}'...", filename);
	IConsoleCmdExec(fmt::format("exec {}", filename));
}};

/**
 * Parse an integer using #ParseInteger and convert it to the requested type.
 * @param arg The string to be converted.
 * @tparam T The type to return.
 * @return The number in the given type, or std::nullopt when it could not be parsed.
 */
template <typename T>
static std::optional<T> ParseType(std::string_view arg)
{
	auto i = ParseInteger(arg);
	if (i.has_value()) return static_cast<T>(*i);
	return std::nullopt;
}

/** File list storage for the console, for caching the last 'ls' command. */
class ConsoleFileList : public FileList {
public:
	ConsoleFileList(AbstractFileType abstract_filetype, bool show_dirs) : FileList(), abstract_filetype(abstract_filetype), show_dirs(show_dirs)
	{
	}

	/** Declare the file storage cache as being invalid, also clears all stored files. */
	void InvalidateFileList()
	{
		this->clear();
		this->file_list_valid = false;
	}

	/**
	 * (Re-)validate the file storage cache. Only makes a change if the storage was invalid, or if \a force_reload.
	 * @param force_reload Always reload the file storage cache.
	 */
	void ValidateFileList(bool force_reload = false)
	{
		if (force_reload || !this->file_list_valid) {
			this->BuildFileList(this->abstract_filetype, SaveLoadOperation::Load, this->show_dirs);
			this->file_list_valid = true;
		}
	}

	AbstractFileType abstract_filetype; ///< The abstract file type to list.
	bool show_dirs; ///< Whether to show directories in the file list.
	bool file_list_valid = false; ///< If set, the file list is valid.
};

static ConsoleFileList _console_file_list_savegame{AbstractFileType::Savegame, true}; ///< File storage cache for savegames.
static ConsoleFileList _console_file_list_scenario{AbstractFileType::Scenario, false}; ///< File storage cache for scenarios.
static ConsoleFileList _console_file_list_heightmap{AbstractFileType::Heightmap, false}; ///< File storage cache for heightmaps.

/****************
 * command hooks
 ****************/

/**
 * Check network availability and inform in console about failure of detection.
 * @param echo Whether to print an error message or not.
 * @return \c true iff the network is available.
 */
static inline bool NetworkAvailable(bool echo)
{
	if (!_network_available) {
		if (echo) IConsolePrint(CC_ERROR, "You cannot use this command because there is no network available.");
		return false;
	}
	return true;
}

/**
 * Check whether we are a server.
 * @copydoc IConsoleHook
 */
static ConsoleHookResult ConHookServerOnly(bool echo)
{
	if (!NetworkAvailable(echo)) return ConsoleHookResult::Disallow;

	if (!_network_server) {
		if (echo) IConsolePrint(CC_ERROR, "This command is only available to a network server.");
		return ConsoleHookResult::Disallow;
	}
	return ConsoleHookResult::Allow;
}

/**
 * Check whether we are a client in a network game.
 * @copydoc IConsoleHook
 */
static ConsoleHookResult ConHookClientOnly(bool echo)
{
	if (!NetworkAvailable(echo)) return ConsoleHookResult::Disallow;

	if (_network_server) {
		if (echo) IConsolePrint(CC_ERROR, "This command is not available to a network server.");
		return ConsoleHookResult::Disallow;
	}
	return ConsoleHookResult::Allow;
}

/**
 * Check whether we are in a multiplayer game.
 * @copydoc IConsoleHook
 */
static ConsoleHookResult ConHookNeedNetwork(bool echo)
{
	if (!NetworkAvailable(echo)) return ConsoleHookResult::Disallow;

	if (!_networking || (!_network_server && !MyClient::IsConnected())) {
		if (echo) IConsolePrint(CC_ERROR, "Not connected. This command is only available in multiplayer.");
		return ConsoleHookResult::Disallow;
	}
	return ConsoleHookResult::Allow;
}

/**
 * Check whether we are in a multiplayer game and are playing, i.e. we are not the dedicated server.
 * @copydoc IConsoleHook
 */
static ConsoleHookResult ConHookNeedNonDedicatedNetwork(bool echo)
{
	if (!NetworkAvailable(echo)) return ConsoleHookResult::Disallow;

	if (_network_dedicated) {
		if (echo) IConsolePrint(CC_ERROR, "This command is not available to a dedicated network server.");
		return ConsoleHookResult::Disallow;
	}
	return ConsoleHookResult::Allow;
}

/**
 * Check whether we are in singleplayer mode.
 * @copydoc IConsoleHook
 */
static ConsoleHookResult ConHookNoNetwork(bool echo)
{
	if (_networking) {
		if (echo) IConsolePrint(CC_ERROR, "This command is forbidden in multiplayer.");
		return ConsoleHookResult::Disallow;
	}
	return ConsoleHookResult::Allow;
}

/**
 * Check if are either in singleplayer or a server.
 * @copydoc IConsoleHook
 */
static ConsoleHookResult ConHookServerOrNoNetwork(bool echo)
{
	if (_networking && !_network_server) {
		if (echo) IConsolePrint(CC_ERROR, "This command is only available to a network server.");
		return ConsoleHookResult::Disallow;
	}
	return ConsoleHookResult::Allow;
}

/**
 * Check whether NewGRF developer tools are enabled.
 * @copydoc IConsoleHook
 */
static ConsoleHookResult ConHookNewGRFDeveloperTool(bool echo)
{
	if (_settings_client.gui.newgrf_developer_tools) {
		if (_game_mode == GameMode::Menu) {
			if (echo) IConsolePrint(CC_ERROR, "This command is only available in-game and in the editor.");
			return ConsoleHookResult::Disallow;
		}
		return ConHookNoNetwork(echo);
	}
	return ConsoleHookResult::Hide;
}

/**
 * Reset status of all engines.
 * @copydoc IConsoleCmdProc
 */
static bool ConResetEngines(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Reset status data of all engines. This might solve some issues with 'lost' engines. Usage: 'resetengines'.");
		return true;
	}

	StartupEngines();
	return true;
}

/**
 * Reset status of the engine pool.
 * @note Resetting the pool only succeeds when there are no vehicles ingame.
 * @copydoc IConsoleCmdProc
 */
static bool ConResetEnginePool(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Reset NewGRF allocations of engine slots. This will remove invalid engine definitions, and might make default engines available again.");
		return true;
	}

	if (_game_mode == GameMode::Menu) {
		IConsolePrint(CC_ERROR, "This command is only available in-game and in the editor.");
		return true;
	}

	if (!EngineOverrideManager::ResetToCurrentNewGRFConfig()) {
		IConsolePrint(CC_ERROR, "This can only be done when there are no vehicles in the game.");
		return true;
	}

	return true;
}

#ifdef _DEBUG
/**
 * Reset a tile to bare land in debug mode.
 * @copydoc IConsoleCmdProc
 */
static bool ConResetTile(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Reset a tile to bare land. Usage: 'resettile <tile>'.");
		IConsolePrint(CC_HELP, "Tile can be either decimal (34161) or hexadecimal (0x4a5B).");
		return true;
	}

	if (argv.size() == 2) {
		auto result = ParseInteger(argv[1], 0);
		if (result.has_value() && IsValidTile(*result)) {
			DoClearSquare(TileIndex{*result});
			return true;
		}
	}

	return false;
}
#endif /* _DEBUG */

/**
 * Zoom map to given level.
 * @copydoc IConsoleCmdProc
 */
static bool ConZoomToLevel(std::span<std::string_view> argv)
{
	switch (argv.size()) {
		case 0:
			IConsolePrint(CC_HELP, "Set the current zoom level of the main viewport.");
			IConsolePrint(CC_HELP, "Usage: 'zoomto <level>'.");

			if (ZoomLevel::Min < _settings_client.gui.zoom_min) {
				IConsolePrint(CC_HELP, "The lowest zoom-in level allowed by current client settings is {}.", std::max(ZoomLevel::Min, _settings_client.gui.zoom_min));
			} else {
				IConsolePrint(CC_HELP, "The lowest supported zoom-in level is {}.", std::max(ZoomLevel::Min, _settings_client.gui.zoom_min));
			}

			if (_settings_client.gui.zoom_max < ZoomLevel::Max) {
				IConsolePrint(CC_HELP, "The highest zoom-out level allowed by current client settings is {}.", std::min(_settings_client.gui.zoom_max, ZoomLevel::Max));
			} else {
				IConsolePrint(CC_HELP, "The highest supported zoom-out level is {}.", std::min(_settings_client.gui.zoom_max, ZoomLevel::Max));
			}
			return true;

		case 2: {
			auto level = ParseInteger<std::underlying_type_t<ZoomLevel>>(argv[1]);
			if (level.has_value()) {
				auto zoom_lvl = static_cast<ZoomLevel>(*level);
				if (!IsInsideMM(zoom_lvl, ZoomLevel::Begin, ZoomLevel::End)) {
					IConsolePrint(CC_ERROR, "Invalid zoom level. Valid range is {} to {}.", ZoomLevel::Min, ZoomLevel::Max);
				} else if (!IsInsideMM(zoom_lvl, _settings_client.gui.zoom_min, _settings_client.gui.zoom_max + 1)) {
					IConsolePrint(CC_ERROR, "Current client settings limit zoom levels to range {} to {}.", _settings_client.gui.zoom_min, _settings_client.gui.zoom_max);
				} else {
					Window *w = GetMainWindow();
					Viewport &vp = *w->viewport;
					while (vp.zoom > zoom_lvl) DoZoomInOutWindow(ZOOM_IN, w);
					while (vp.zoom < zoom_lvl) DoZoomInOutWindow(ZOOM_OUT, w);
				}
				return true;
			}
			break;
		}
	}

	return false;
}

/**
 * Scroll to a tile on the map.
 * @copydoc IConsoleCmdProc
 */
static bool ConScrollToTile(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Center the screen on a given tile.");
		IConsolePrint(CC_HELP, "Usage: 'scrollto [instant] <tile>' or 'scrollto [instant] <x> <y>'.");
		IConsolePrint(CC_HELP, "Numbers can be either decimal (34161) or hexadecimal (0x4a5B).");
		IConsolePrint(CC_HELP, "'instant' will immediately move and redraw viewport without smooth scrolling.");
		return true;
	}
	if (argv.size() < 2) return false;

	uint32_t arg_index = 1;
	bool instant = false;
	if (argv[arg_index] == "instant") {
		++arg_index;
		instant = true;
	}

	switch (argv.size() - arg_index) {
		case 1: {
			auto result = ParseInteger(argv[arg_index], 0);
			if (result.has_value()) {
				if (*result >= Map::Size()) {
					IConsolePrint(CC_ERROR, "Tile does not exist.");
					return true;
				}
				ScrollMainWindowToTile(TileIndex{*result}, instant);
				return true;
			}
			break;
		}

		case 2: {
			auto x = ParseInteger(argv[arg_index], 0);
			auto y = ParseInteger(argv[arg_index + 1], 0);
			if (x.has_value() && y.has_value()) {
				if (*x >= Map::SizeX() || *y >= Map::SizeY()) {
					IConsolePrint(CC_ERROR, "Tile does not exist.");
					return true;
				}
				ScrollMainWindowToTile(TileXY(*x, *y), instant);
				return true;
			}
			break;
		}
	}

	return false;
}

/**
 * Save the map to a file.
 * @copydoc IConsoleCmdProc
 */
static bool ConSave(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Save the current game. Usage: 'save <filename>'.");
		return true;
	}

	if (argv.size() == 2) {
		std::string filename = fmt::format("{}.sav", argv[1]);
		IConsolePrint(CC_DEFAULT, "Saving map...");

		if (SaveOrLoad(filename, SaveLoadOperation::Save, DetailedFileType::GameFile, Subdirectory::Save) != SaveLoadResult::Ok) {
			IConsolePrint(CC_ERROR, "Saving map failed.");
		} else {
			IConsolePrint(CC_INFO, "Map successfully saved to '{}'.", filename);
		}
		return true;
	}

	return false;
}

/**
 * Explicitly save the configuration.
 * @copydoc IConsoleCmdProc
 */
static bool ConSaveConfig(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Saves the configuration for new games to the configuration file, typically 'openttd.cfg'.");
		IConsolePrint(CC_HELP, "It does not save the configuration of the current game to the configuration file.");
		return true;
	}

	SaveToConfig();
	IConsolePrint(CC_DEFAULT, "Saved config.");
	return true;
}

/** Load a savegame. @copydoc IConsoleCmdProc */
static bool ConLoad(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Load a game by name or index. Usage: 'load <file | number>'.");
		return true;
	}

	if (argv.size() != 2) return false;

	std::string_view file = argv[1];
	_console_file_list_savegame.ValidateFileList();
	const FiosItem *item = _console_file_list_savegame.FindItem(file);
	if (item != nullptr) {
		if (item->type.abstract == AbstractFileType::Savegame) {
			_switch_mode = SwitchMode::LoadGame;
			_file_to_saveload.Set(*item);
		} else {
			IConsolePrint(CC_ERROR, "'{}' is not a savegame.", file);
		}
	} else {
		IConsolePrint(CC_ERROR, "'{}' cannot be found.", file);
	}

	return true;
}

/** Load a scenario. @copydoc IConsoleCmdProc */
static bool ConLoadScenario(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Load a scenario by name or index. Usage: 'load_scenario <file | number>'.");
		return true;
	}

	if (argv.size() != 2) return false;

	std::string_view file = argv[1];
	_console_file_list_scenario.ValidateFileList();
	const FiosItem *item = _console_file_list_scenario.FindItem(file);
	if (item != nullptr) {
		if (item->type.abstract == AbstractFileType::Scenario) {
			_switch_mode = SwitchMode::LoadGame;
			_file_to_saveload.Set(*item);
		} else {
			IConsolePrint(CC_ERROR, "'{}' is not a scenario.", file);
		}
	} else {
		IConsolePrint(CC_ERROR, "'{}' cannot be found.", file);
	}

	return true;
}

/** Load a heightmap. @copydoc IConsoleCmdProc */
static bool ConLoadHeightmap(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Load a heightmap by name or index. Usage: 'load_heightmap <file | number>'.");
		return true;
	}

	if (argv.size() != 2) return false;

	std::string_view file = argv[1];
	_console_file_list_heightmap.ValidateFileList();
	const FiosItem *item = _console_file_list_heightmap.FindItem(file);
	if (item != nullptr) {
		if (item->type.abstract == AbstractFileType::Heightmap) {
			_switch_mode = SwitchMode::StartHeightmap;
			_file_to_saveload.Set(*item);
		} else {
			IConsolePrint(CC_ERROR, "'{}' is not a heightmap.", file);
		}
	} else {
		IConsolePrint(CC_ERROR, "'{}' cannot be found.", file);
	}

	return true;
}

/** Remove a savegame file from disk. @copydoc IConsoleCmdProc */
static bool ConRemove(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Remove a savegame by name or index. Usage: 'rm <file | number>'.");
		return true;
	}

	if (argv.size() != 2) return false;

	std::string_view file = argv[1];
	_console_file_list_savegame.ValidateFileList();
	const FiosItem *item = _console_file_list_savegame.FindItem(file);
	if (item != nullptr) {
		if (item->type.abstract == AbstractFileType::Savegame) {
			if (!FioRemove(item->name)) {
				IConsolePrint(CC_ERROR, "Failed to delete '{}'.", item->name);
			}
		} else {
			IConsolePrint(CC_ERROR, "'{}' is not a savegame.", file);
		}
	} else {
		IConsolePrint(CC_ERROR, "'{}' could not be found.", file);
	}

	_console_file_list_savegame.InvalidateFileList();
	return true;
}


/** List all the files in the current dir via console. @copydoc IConsoleCmdProc */
static bool ConListFiles(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "List all loadable savegames and directories in the current dir via console. Usage: 'ls | dir'.");
		return true;
	}

	_console_file_list_savegame.ValidateFileList(true);
	for (uint i = 0; i < _console_file_list_savegame.size(); i++) {
		IConsolePrint(CC_DEFAULT, "{}) {}", i, _console_file_list_savegame[i].title.GetDecodedString());
	}

	return true;
}

/** List all the scenarios. @copydoc IConsoleCmdProc */
static bool ConListScenarios(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "List all loadable scenarios. Usage: 'list_scenarios'.");
		return true;
	}

	_console_file_list_scenario.ValidateFileList(true);
	for (uint i = 0; i < _console_file_list_scenario.size(); i++) {
		IConsolePrint(CC_DEFAULT, "{}) {}", i, _console_file_list_scenario[i].title.GetDecodedString());
	}

	return true;
}

/** List all the heightmaps. @copydoc IConsoleCmdProc */
static bool ConListHeightmaps(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "List all loadable heightmaps. Usage: 'list_heightmaps'.");
		return true;
	}

	_console_file_list_heightmap.ValidateFileList(true);
	for (uint i = 0; i < _console_file_list_heightmap.size(); i++) {
		IConsolePrint(CC_DEFAULT, "{}) {}", i, _console_file_list_heightmap[i].title.GetDecodedString());
	}

	return true;
}

/** Change the dir via console. @copydoc IConsoleCmdProc */
static bool ConChangeDirectory(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Change the dir via console. Usage: 'cd <directory | number>'.");
		return true;
	}

	if (argv.size() != 2) return false;

	std::string_view file = argv[1];
	_console_file_list_savegame.ValidateFileList(true);
	const FiosItem *item = _console_file_list_savegame.FindItem(file);
	if (item != nullptr) {
		switch (item->type.detailed) {
			case DetailedFileType::FiosDirectory:
			case DetailedFileType::FiosDrive:
			case DetailedFileType::FiosParent:
				FiosBrowseTo(item);
				break;
			default: IConsolePrint(CC_ERROR, "{}: Not a directory.", file);
		}
	} else {
		IConsolePrint(CC_ERROR, "{}: No such file or directory.", file);
	}

	_console_file_list_savegame.InvalidateFileList();
	return true;
}

/** Print the current working directory. @copydoc IConsoleCmdProc */
static bool ConPrintWorkingDirectory(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Print out the current working directory. Usage: 'pwd'.");
		return true;
	}

	/* XXX - Workaround for broken file handling */
	_console_file_list_savegame.ValidateFileList(true);
	_console_file_list_savegame.InvalidateFileList();

	IConsolePrint(CC_DEFAULT, FiosGetCurrentPath());
	return true;
}

/** Clear the console's buffer. @copydoc IConsoleCmdProc */
static bool ConClearBuffer(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Clear the console buffer. Usage: 'clear'.");
		return true;
	}

	IConsoleClearBuffer();
	SetWindowDirty(WindowClass::Console, 0);
	return true;
}


/**********************************
 * Network Core Console Commands
 **********************************/

/**
 * Helper to kick or ban a user.
 * @param arg The client id or IP address.
 * @param ban Whether to ban, when \c false only a kick is performed.
 * @param reason The reason for this action.
 * @return \c true iff the command is handled correctly, i.e. \c false to show a help message.
 */
static bool ConKickOrBan(std::string_view arg, bool ban, std::string_view reason)
{
	uint n;

	if (arg.find_first_of(".:") == std::string::npos) { // banning with ID
		auto client_id = ParseType<ClientID>(arg);
		if (!client_id.has_value()) {
			IConsolePrint(CC_ERROR, "The given client-id is not a valid number.");
			return true;
		}

		/* Don't kill the server, or the client doing the rcon. The latter can't be kicked because
		 * kicking frees closes and subsequently free the connection related instances, which we
		 * would be reading from and writing to after returning. So we would read or write data
		 * from freed memory up till the segfault triggers. */
		if (*client_id == ClientID::Server || *client_id == _redirect_console_to_client) {
			IConsolePrint(CC_ERROR, "You can not {} yourself!", ban ? "ban" : "kick");
			return true;
		}

		NetworkClientInfo *ci = NetworkClientInfo::GetByClientID(*client_id);
		if (ci == nullptr) {
			IConsolePrint(CC_ERROR, "Invalid client-id.");
			return true;
		}

		if (!ban) {
			/* Kick only this client, not all clients with that IP */
			NetworkServerKickClient(*client_id, reason);
			return true;
		}

		/* When banning, kick+ban all clients with that IP */
		n = NetworkServerKickOrBanIP(*client_id, ban, reason);
	} else {
		n = NetworkServerKickOrBanIP(arg, ban, reason);
	}

	if (n == 0) {
		IConsolePrint(CC_DEFAULT, ban ? "Client not online, address added to banlist." : "Client not found.");
	} else {
		IConsolePrint(CC_DEFAULT, "{}ed {} client(s).", ban ? "Bann" : "Kick", n);
	}

	return true;
}

/** Kick a user from a network game. @copydoc IConsoleCmdProc */
static bool ConKick(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Kick a client from a network game. Usage: 'kick <ip | client-id> [<kick-reason>]'.");
		IConsolePrint(CC_HELP, "For client-id's, see the command 'clients'.");
		return true;
	}

	if (argv.size() != 2 && argv.size() != 3) return false;

	/* No reason supplied for kicking */
	if (argv.size() == 2) return ConKickOrBan(argv[1], false, {});

	/* Reason for kicking supplied */
	size_t kick_message_length = argv[2].size();
	if (kick_message_length >= 255) {
		IConsolePrint(CC_ERROR, "Maximum kick message length is 254 characters. You entered {} characters.", kick_message_length);
		return false;
	} else {
		return ConKickOrBan(argv[1], false, argv[2]);
	}
}

/** Ban a user from a network game. @copydoc IConsoleCmdProc */
static bool ConBan(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Ban a client from a network game. Usage: 'ban <ip | client-id> [<ban-reason>]'.");
		IConsolePrint(CC_HELP, "For client-id's, see the command 'clients'.");
		IConsolePrint(CC_HELP, "If the client is no longer online, you can still ban their IP.");
		return true;
	}

	if (argv.size() != 2 && argv.size() != 3) return false;

	/* No reason supplied for kicking */
	if (argv.size() == 2) return ConKickOrBan(argv[1], true, {});

	/* Reason for kicking supplied */
	size_t kick_message_length = argv[2].size();
	if (kick_message_length >= 255) {
		IConsolePrint(CC_ERROR, "Maximum kick message length is 254 characters. You entered {} characters.", kick_message_length);
		return false;
	} else {
		return ConKickOrBan(argv[1], true, argv[2]);
	}
}

/** Unban a user from a network game. @copydoc IConsoleCmdProc */
static bool ConUnBan(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Unban a client from a network game. Usage: 'unban <ip | banlist-index>'.");
		IConsolePrint(CC_HELP, "For a list of banned IP's, see the command 'banlist'.");
		return true;
	}

	if (argv.size() != 2) return false;

	/* Try by IP. */
	uint index;
	for (index = 0; index < _network_ban_list.size(); index++) {
		if (_network_ban_list[index] == argv[1]) break;
	}

	/* Try by index. */
	if (index >= _network_ban_list.size()) {
		index = ParseInteger(argv[1]).value_or(0) - 1U; // let it wrap
	}

	if (index < _network_ban_list.size()) {
		IConsolePrint(CC_DEFAULT, "Unbanned {}.", _network_ban_list[index]);
		_network_ban_list.erase(_network_ban_list.begin() + index);
	} else {
		IConsolePrint(CC_DEFAULT, "Invalid list index or IP not in ban-list.");
		IConsolePrint(CC_DEFAULT, "For a list of banned IP's, see the command 'banlist'.");
	}

	return true;
}

/** Show the list of banned clients. @copydoc IConsoleCmdProc */
static bool ConBanList(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "List the IP's of banned clients: Usage 'banlist'.");
		return true;
	}

	IConsolePrint(CC_DEFAULT, "Banlist:");

	uint i = 1;
	for (const auto &entry : _network_ban_list) {
		IConsolePrint(CC_DEFAULT, "  {}) {}", i, entry);
		i++;
	}

	return true;
}

/** Manually pause the game. @copydoc IConsoleCmdProc */
static bool ConPauseGame(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Pause a network game. Usage: 'pause'.");
		return true;
	}

	if (_game_mode == GameMode::Menu) {
		IConsolePrint(CC_ERROR, "This command is only available in-game and in the editor.");
		return true;
	}

	if (!_pause_mode.Test(PauseMode::Normal)) {
		Command<Commands::Pause>::Post(PauseMode::Normal, true);
		if (!_networking) IConsolePrint(CC_DEFAULT, "Game paused.");
	} else {
		IConsolePrint(CC_DEFAULT, "Game is already paused.");
	}

	return true;
}

/** Manually unpause the game. @copydoc IConsoleCmdProc */
static bool ConUnpauseGame(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Unpause a network game. Usage: 'unpause'.");
		return true;
	}

	if (_game_mode == GameMode::Menu) {
		IConsolePrint(CC_ERROR, "This command is only available in-game and in the editor.");
		return true;
	}

	if (_pause_mode.Test(PauseMode::Normal)) {
		Command<Commands::Pause>::Post(PauseMode::Normal, false);
		if (!_networking) IConsolePrint(CC_DEFAULT, "Game unpaused.");
	} else if (_pause_mode.Test(PauseMode::Error)) {
		IConsolePrint(CC_DEFAULT, "Game is in error state and cannot be unpaused via console.");
	} else if (_pause_mode.Any()) {
		IConsolePrint(CC_DEFAULT, "Game cannot be unpaused manually; disable pause_on_join/min_active_clients.");
	} else {
		IConsolePrint(CC_DEFAULT, "Game is already unpaused.");
	}

	return true;
}

/** Run a console command on the server. @copydoc IConsoleCmdProc */
static bool ConRcon(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Remote control the server from another client. Usage: 'rcon <password> <command>'.");
		IConsolePrint(CC_HELP, "Remember to enclose the command in quotes, otherwise only the first parameter is sent.");
		IConsolePrint(CC_HELP, "When your client's public key is in the 'authorized keys' for 'rcon', the password is not checked and may be '*'.");
		return true;
	}

	if (argv.size() < 3) return false;

	if (_network_server) {
		IConsoleCmdExec(argv[2]);
	} else {
		NetworkClientSendRcon(argv[1], argv[2]);
	}
	return true;
}

/** Get the status of connected clients. @copydoc IConsoleCmdProc */
static bool ConStatus(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "List the status of all clients connected to the server. Usage 'status'.");
		return true;
	}

	NetworkServerShowStatusToConsole();
	return true;
}

/** Get information like client/company count/limits for the server. @copydoc IConsoleCmdProc */
static bool ConServerInfo(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "List current and maximum client/company limits. Usage 'server_info'.");
		IConsolePrint(CC_HELP, "You can change these values by modifying settings 'network.max_clients' and 'network.max_companies'.");
		return true;
	}

	IConsolePrint(CC_DEFAULT, "Invite code:                {}", _network_server_invite_code);
	IConsolePrint(CC_DEFAULT, "Current/maximum clients:    {:3d}/{:3d}", _network_game_info.clients_on, _settings_client.network.max_clients);
	IConsolePrint(CC_DEFAULT, "Current/maximum companies:  {:3d}/{:3d}", Company::GetNumItems(), _settings_client.network.max_companies);
	IConsolePrint(CC_DEFAULT, "Current spectators:         {:3d}", NetworkSpectatorCount());

	return true;
}

/** Change the name of a client. @copydoc IConsoleCmdProc */
static bool ConClientNickChange(std::span<std::string_view> argv)
{
	if (argv.size() != 3) {
		IConsolePrint(CC_HELP, "Change the nickname of a connected client. Usage: 'client_name <client-id> <new-name>'.");
		IConsolePrint(CC_HELP, "For client-id's, see the command 'clients'.");
		return true;
	}

	auto client_id = ParseType<ClientID>(argv[1]);
	if (!client_id.has_value()) {
		IConsolePrint(CC_ERROR, "The given client-id is not a valid number.");
		return true;
	}

	if (*client_id == ClientID::Server) {
		IConsolePrint(CC_ERROR, "Please use the command 'name' to change your own name!");
		return true;
	}

	if (NetworkClientInfo::GetByClientID(*client_id) == nullptr) {
		IConsolePrint(CC_ERROR, "Invalid client-id.");
		return true;
	}

	std::string client_name{StrTrimView(argv[2], StringConsumer::WHITESPACE_NO_NEWLINE)};
	if (!NetworkIsValidClientName(client_name)) {
		IConsolePrint(CC_ERROR, "Cannot give a client an empty name.");
		return true;
	}

	if (!NetworkServerChangeClientName(*client_id, client_name)) {
		IConsolePrint(CC_ERROR, "Cannot give a client a duplicate name.");
	}

	return true;
}

/**
 * Helper to parse a company ID. Note that 'Company #1' has ID 0.
 * @param arg The string to get the company ID from.
 * @return The company's ID, or std::nullopt when no valid ID was found.
 */
static std::optional<CompanyID> ParseCompanyID(std::string_view arg)
{
	auto company_id = ParseType<CompanyID>(arg);
	if (company_id.has_value() && *company_id <= MAX_COMPANIES) return static_cast<CompanyID>(*company_id - 1);
	return company_id;
}

/** As client, join a company. @copydoc IConsoleCmdProc */
static bool ConJoinCompany(std::span<std::string_view> argv)
{
	if (argv.size() < 2) {
		IConsolePrint(CC_HELP, "Request joining another company. Usage: 'join <company-id>'.");
		IConsolePrint(CC_HELP, "For valid company-id see company list, use 255 for spectator.");
		return true;
	}

	auto company_id = ParseCompanyID(argv[1]);
	if (!company_id.has_value()) {
		IConsolePrint(CC_ERROR, "The given company-id is not a valid number.");
		return true;
	}

	const NetworkClientInfo *info = NetworkClientInfo::GetByClientID(_network_own_client_id);
	if (info == nullptr) {
		IConsolePrint(CC_ERROR, "You have not joined the game yet!");
		return true;
	}

	/* Check we have a valid company id! */
	if (!Company::IsValidID(*company_id) && *company_id != COMPANY_SPECTATOR) {
		IConsolePrint(CC_ERROR, "Company does not exist. Company-id must be between 1 and {}.", MAX_COMPANIES);
		return true;
	}

	if (info->client_playas == *company_id) {
		IConsolePrint(CC_ERROR, "You are already there!");
		return true;
	}

	if (*company_id != COMPANY_SPECTATOR && !Company::IsHumanID(*company_id)) {
		IConsolePrint(CC_ERROR, "Cannot join AI company.");
		return true;
	}

	if (!info->CanJoinCompany(*company_id)) {
		IConsolePrint(CC_ERROR, "You are not allowed to join this company.");
		return true;
	}

	/* non-dedicated server may just do the move! */
	if (_network_server) {
		NetworkServerDoMove(ClientID::Server, *company_id);
	} else {
		NetworkClientRequestMove(*company_id);
	}

	return true;
}

/** Move a client to a specific company. @copydoc IConsoleCmdProc */
static bool ConMoveClient(std::span<std::string_view> argv)
{
	if (argv.size() < 3) {
		IConsolePrint(CC_HELP, "Move a client to another company. Usage: 'move <client-id> <company-id>'.");
		IConsolePrint(CC_HELP, "For valid client-id see 'clients', for valid company-id see 'companies', use 255 for moving to spectators.");
		return true;
	}

	auto client_id = ParseType<ClientID>(argv[1]);
	if (!client_id.has_value()) {
		IConsolePrint(CC_ERROR, "The given client-id is not a valid number.");
		return true;
	}
	const NetworkClientInfo *ci = NetworkClientInfo::GetByClientID(*client_id);

	auto company_id = ParseCompanyID(argv[2]);
	if (!company_id.has_value()) {
		IConsolePrint(CC_ERROR, "The given company-id is not a valid number.");
		return true;
	}

	/* check the client exists */
	if (ci == nullptr) {
		IConsolePrint(CC_ERROR, "Invalid client-id, check the command 'clients' for valid client-id's.");
		return true;
	}

	if (!Company::IsValidID(*company_id) && *company_id != COMPANY_SPECTATOR) {
		IConsolePrint(CC_ERROR, "Company does not exist. Company-id must be between 1 and {}.", MAX_COMPANIES);
		return true;
	}

	if (*company_id != COMPANY_SPECTATOR && !Company::IsHumanID(*company_id)) {
		IConsolePrint(CC_ERROR, "You cannot move clients to AI companies.");
		return true;
	}

	if (ci->client_id == ClientID::Server && _network_dedicated) {
		IConsolePrint(CC_ERROR, "You cannot move the server!");
		return true;
	}

	if (ci->client_playas == *company_id) {
		IConsolePrint(CC_ERROR, "You cannot move someone to where they already are!");
		return true;
	}

	/* we are the server, so force the update */
	NetworkServerDoMove(ci->client_id, *company_id);

	return true;
}

/** Remove a company from the game. @copydoc IConsoleCmdProc */
static bool ConResetCompany(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Remove an idle company from the game. Usage: 'reset_company <company-id>'.");
		IConsolePrint(CC_HELP, "For company-id's, see the list of companies from the dropdown menu. Company 1 is 1, etc.");
		return true;
	}

	if (argv.size() != 2) return false;

	auto index = ParseCompanyID(argv[1]);
	if (!index.has_value()) {
		IConsolePrint(CC_ERROR, "The given company-id is not a valid number.");
		return true;
	}

	/* Check valid range */
	if (!Company::IsValidID(*index)) {
		IConsolePrint(CC_ERROR, "Company does not exist. company-id must be between 1 and {}.", MAX_COMPANIES);
		return true;
	}

	if (!Company::IsHumanID(*index)) {
		IConsolePrint(CC_ERROR, "Company is owned by an AI.");
		return true;
	}

	if (NetworkCompanyHasClients(*index)) {
		IConsolePrint(CC_ERROR, "Cannot remove company: a client is connected to that company.");
		return false;
	}
	const NetworkClientInfo *ci = NetworkClientInfo::GetByClientID(ClientID::Server);
	assert(ci != nullptr);
	if (ci->client_playas == *index) {
		IConsolePrint(CC_ERROR, "Cannot remove company: the server is connected to that company.");
		return true;
	}

	/* It is safe to remove this company */
	Command<Commands::CompanyControl>::Post(CompanyCtrlAction::Delete, *index, CompanyRemoveReason::Manual, ClientID::Invalid);
	IConsolePrint(CC_DEFAULT, "Company deleted.");

	return true;
}

/** List the clients. @copydoc IConsoleCmdProc */
static bool ConNetworkClients(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Get a list of connected clients including their ID, name, company-id, and IP. Usage: 'clients'.");
		return true;
	}

	NetworkPrintClients();

	return true;
}

/** Connect to the last client you were connected to. @copydoc IConsoleCmdProc */
static bool ConNetworkReconnect(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Reconnect to server to which you were connected last time. Usage: 'reconnect [<company-id>]'.");
		IConsolePrint(CC_HELP, "Company 255 is spectator (default, if not specified), 254 means creating new company.");
		IConsolePrint(CC_HELP, "All others are a certain company with Company 1 being #1.");
		return true;
	}

	CompanyID playas = COMPANY_SPECTATOR;
	if (argv.size() >= 2) {
		auto company_id = ParseCompanyID(argv[1]);
		if (!company_id.has_value()) {
			IConsolePrint(CC_ERROR, "The given company-id is not a valid number.");
			return true;
		}
		if (*company_id >= MAX_COMPANIES && *company_id != COMPANY_NEW_COMPANY && *company_id != COMPANY_SPECTATOR) return false;
		playas = *company_id;
	}

	if (_settings_client.network.last_joined.empty()) {
		IConsolePrint(CC_DEFAULT, "No server for reconnecting.");
		return true;
	}

	/* Don't resolve the address first, just print it directly as it comes from the config file. */
	IConsolePrint(CC_DEFAULT, "Reconnecting to {} ...", _settings_client.network.last_joined);

	return NetworkClientConnectGame(_settings_client.network.last_joined, playas);
}

/** Connect to a specific server. @copydoc IConsoleCmdProc */
static bool ConNetworkConnect(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Connect to a remote OTTD server and join the game. Usage: 'connect <ip>'.");
		IConsolePrint(CC_HELP, "IP can contain port and company: 'IP[:Port][#Company]', eg: 'server.ottd.org:443#2'.");
		IConsolePrint(CC_HELP, "Company #255 is spectator all others are a certain company with Company 1 being #1.");
		return true;
	}

	if (argv.size() < 2) return false;

	return NetworkClientConnectGame(argv[1], COMPANY_NEW_COMPANY);
}

/*********************************
 *  script file console commands
 *********************************/

/** Run a local script file. @copydoc IConsoleCmdProc */
static bool ConExec(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Execute a local script file. Usage: 'exec <script> [0]'.");
		IConsolePrint(CC_HELP, "By passing '0' after the script name, no warning about a missing script file will be shown.");
		return true;
	}

	if (argv.size() < 2) return false;

	auto script_file = FioFOpenFile(argv[1], "r", Subdirectory::Base);

	if (!script_file.has_value()) {
		if (argv.size() == 2 || argv[2] != "0") IConsolePrint(CC_ERROR, "Script file '{}' not found.", argv[1]);
		return true;
	}

	if (_script_current_depth == 11) {
		IConsolePrint(CC_ERROR, "Maximum 'exec' depth reached; script A is calling script B is calling script C ... more than 10 times.");
		return true;
	}

	_script_current_depth++;
	uint script_depth = _script_current_depth;

	char buffer[ICON_CMDLN_SIZE];
	while (fgets(buffer, sizeof(buffer), *script_file) != nullptr) {
		/* Remove newline characters from the executing script */
		std::string_view cmdline{buffer};
		auto last_non_newline = cmdline.find_last_not_of("\r\n");
		if (last_non_newline != std::string_view::npos) cmdline = cmdline.substr(0, last_non_newline + 1);

		IConsoleCmdExec(cmdline);
		/* Ensure that we are still on the same depth or that we returned via 'return'. */
		assert(_script_current_depth == script_depth || _script_current_depth == script_depth - 1);

		/* The 'return' command was executed. */
		if (_script_current_depth == script_depth - 1) break;
	}

	if (ferror(*script_file) != 0) {
		IConsolePrint(CC_ERROR, "Encountered error while trying to read from script file '{}'.", argv[1]);
	}

	if (_script_current_depth == script_depth) _script_current_depth--;
	return true;
}

/** Schedule the execution of a script. @copydoc IConsoleCmdProc */
static bool ConSchedule(std::span<std::string_view> argv)
{
	if (argv.size() < 3 || std::string_view(argv[1]) != "on-next-calendar-month") {
		IConsolePrint(CC_HELP, "Schedule a local script to execute later. Usage: 'schedule on-next-calendar-month <script>'.");
		return true;
	}

	/* Check if the file exists. It might still go away later, but helpful to show an error now. */
	if (!FioCheckFileExists(argv[2], Subdirectory::Base)) {
		IConsolePrint(CC_ERROR, "Script file '{}' not found.", argv[2]);
		return true;
	}

	/* We only support a single script scheduled, so we tell the user what's happening if there was already one. */
	std::string_view filename = std::string_view(argv[2]);
	if (!_scheduled_monthly_script.empty() && filename == _scheduled_monthly_script) {
		IConsolePrint(CC_INFO, "Script file '{}' was already scheduled to execute at the start of next calendar month.", filename);
	} else if (!_scheduled_monthly_script.empty() && filename != _scheduled_monthly_script) {
		IConsolePrint(CC_INFO, "Script file '{}' scheduled to execute at the start of next calendar month, replacing the previously scheduled script file '{}'.", filename, _scheduled_monthly_script);
	} else {
		IConsolePrint(CC_INFO, "Script file '{}' scheduled to execute at the start of next calendar month.", filename);
	}

	/* Store the filename to be used by _schedule_timer on the start of next calendar month. */
	_scheduled_monthly_script = filename;

	return true;
}

/** End the execution of the current script. @copydoc IConsoleCmdProc */
static bool ConReturn(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Stop executing a running script. Usage: 'return'.");
		return true;
	}

	_script_current_depth--;
	return true;
}

/*****************************
 *  default console commands
 ******************************/
extern bool CloseConsoleLogIfActive();
extern std::span<const GRFFile> GetAllGRFFiles();
extern void ConPrintFramerate(); // framerate_gui.cpp
extern void ShowFramerateWindow();

/** Enable or disable logging of console output. @copydoc IConsoleCmdProc */
static bool ConScript(std::span<std::string_view> argv)
{
	extern std::optional<FileHandle> _iconsole_output_file;

	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Start or stop logging console output to a file. Usage: 'script <filename>'.");
		IConsolePrint(CC_HELP, "If filename is omitted, a running log is stopped if it is active.");
		return true;
	}

	if (!CloseConsoleLogIfActive()) {
		if (argv.size() < 2) return false;

		_iconsole_output_file = FileHandle::Open(argv[1], "ab");
		if (!_iconsole_output_file.has_value()) {
			IConsolePrint(CC_ERROR, "Could not open console log file '{}'.", argv[1]);
		} else {
			IConsolePrint(CC_INFO, "Console log output started to '{}'.", argv[1]);
		}
	}

	return true;
}

/** Simply print the arguments. @copydoc IConsoleCmdProc */
static bool ConEcho(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Print back the first argument to the console. Usage: 'echo <arg>'.");
		return true;
	}

	if (argv.size() < 2) return false;
	IConsolePrint(CC_DEFAULT, "{}", argv[1]);
	return true;
}

/** Print the arguments in a particular colour. @copydoc IConsoleCmdProc */
static bool ConEchoC(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Print back the first argument to the console in a given colour. Usage: 'echoc <colour> <arg2>'.");
		return true;
	}

	if (argv.size() < 3) return false;

	auto colour = ParseInteger(argv[1]);
	if (!colour.has_value() || !IsInsideMM(*colour, to_underlying(TextColour::Begin), to_underlying(TextColour::End))) {
		IConsolePrint(CC_ERROR, "The colour must be a number between {} and {}.", TextColour::Begin, to_underlying(TextColour::End) - 1);
		return true;
	}

	IConsolePrint(static_cast<TextColour>(*colour), "{}", argv[2]);
	return true;
}

/** Start/create a new game. @copydoc IConsoleCmdProc */
static bool ConNewGame(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Start a new game. Usage: 'newgame [seed]'.");
		IConsolePrint(CC_HELP, "The server can force a new game using 'newgame'; any client joined will rejoin after the server is done generating the new game.");
		return true;
	}

	uint32_t seed = GENERATE_NEW_SEED;
	if (argv.size() >= 2) {
		auto param = ParseInteger(argv[1]);
		if (!param.has_value()) {
			IConsolePrint(CC_ERROR, "The given seed must be a valid number.");
			return true;
		}
		seed = *param;
	}

	StartNewGameWithoutGUI(seed);
	return true;
}

/** Restart the game. @copydoc IConsoleCmdProc */
static bool ConRestart(std::span<std::string_view> argv)
{
	if (argv.empty() || argv.size() > 2) {
		IConsolePrint(CC_HELP, "Restart game. Usage: 'restart [current|newgame]'.");
		IConsolePrint(CC_HELP, "Restarts a game, using either the current or newgame (default) settings.");
		IConsolePrint(CC_HELP, " * if you started from a new game, and your current/newgame settings haven't changed, the game will be identical to when you started it.");
		IConsolePrint(CC_HELP, " * if you started from a savegame / scenario / heightmap, the game might be different, because the current/newgame settings might differ.");
		return true;
	}

	if (argv.size() == 1 || std::string_view(argv[1]) == "newgame") {
		StartNewGameWithoutGUI(_settings_game.game_creation.generation_seed);
	} else {
		_settings_game.game_creation.map_x = Map::LogX();
		_settings_game.game_creation.map_y = Map::LogY();
		_switch_mode = SwitchMode::RestartGame;
	}

	return true;
}

/** Reload a game from the loaded savegame/scenario/heightmap. @copydoc IConsoleCmdProc */
static bool ConReload(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Reload game. Usage: 'reload'.");
		IConsolePrint(CC_HELP, "Reloads a game if loaded via savegame / scenario / heightmap.");
		return true;
	}

	if (_file_to_saveload.ftype.abstract == AbstractFileType::None || _file_to_saveload.ftype.abstract == AbstractFileType::Invalid) {
		IConsolePrint(CC_ERROR, "No game loaded to reload.");
		return true;
	}

	/* Use a switch-mode to prevent copying over newgame settings to active settings. */
	_settings_game.game_creation.map_x = Map::LogX();
	_settings_game.game_creation.map_y = Map::LogY();
	_switch_mode = SwitchMode::ReloadGame;
	return true;
}

/**
 * Print a text buffer line by line to the console. Lines are separated by '\n'.
 * @param full_string The multi-line string to print.
 */
static void PrintLineByLine(const std::string &full_string)
{
	std::istringstream in(full_string);
	std::string line;
	while (std::getline(in, line)) {
		IConsolePrint(CC_DEFAULT, line);
	}
}

/**
 * Helper to print a list to the console.
 * @param list_function The function that gets the list.
 * @param args The arguments for the list function.
 * @return \c true, to ease the use in @see IConsoleCmdProc.
 */
template <typename F, typename ... Args>
bool PrintList(F list_function, Args... args)
{
	std::string output_str;
	auto inserter = std::back_inserter(output_str);
	list_function(inserter, args...);
	PrintLineByLine(output_str);

	return true;
}

/** List all AI libraries. @copydoc IConsoleCmdProc */
static bool ConListAILibs(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "List installed AI libraries. Usage: 'list_ai_libs'.");
		return true;
	}

	return PrintList(AI::GetConsoleLibraryList, true);
}

/** List all AI scripts. @copydoc IConsoleCmdProc */
static bool ConListAI(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "List installed AIs. Usage: 'list_ai'.");
		return true;
	}

	return PrintList(AI::GetConsoleList, false);
}

/** List all game script libraries. @copydoc IConsoleCmdProc */
static bool ConListGameLibs(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "List installed Game Script libraries. Usage: 'list_game_libs'.");
		return true;
	}

	return PrintList(Game::GetConsoleLibraryList, true);
}

/** List all game scripts. @copydoc IConsoleCmdProc */
static bool ConListGame(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "List installed Game Scripts. Usage: 'list_game'.");
		return true;
	}

	return PrintList(Game::GetConsoleList, false);
}

/** Start a new AI. @copydoc IConsoleCmdProc */
static bool ConStartAI(std::span<std::string_view> argv)
{
	if (argv.empty() || argv.size() > 3) {
		IConsolePrint(CC_HELP, "Start a new AI. Usage: 'start_ai [<AI>] [<settings>]'.");
		IConsolePrint(CC_HELP, "Start a new AI. If <AI> is given, it starts that specific AI (if found).");
		IConsolePrint(CC_HELP, "If <settings> is given, it is parsed and the AI settings are set to that.");
		return true;
	}

	if (_game_mode != GameMode::Normal) {
		IConsolePrint(CC_ERROR, "AIs can only be managed in a game.");
		return true;
	}

	if (Company::GetNumItems() == CompanyPool::MAX_SIZE) {
		IConsolePrint(CC_ERROR, "Can't start a new AI (no more free slots).");
		return true;
	}
	if (_networking && !_network_server) {
		IConsolePrint(CC_ERROR, "Only the server can start a new AI.");
		return true;
	}
	if (_networking && !_settings_game.ai.ai_in_multiplayer) {
		IConsolePrint(CC_ERROR, "AIs are not allowed in multiplayer by configuration.");
		IConsolePrint(CC_ERROR, "Switch AI -> AI in multiplayer to True.");
		return true;
	}
	if (!AI::CanStartNew()) {
		IConsolePrint(CC_ERROR, "Can't start a new AI.");
		return true;
	}

	int n = 0;
	/* Find the next free slot */
	for (const Company *c : Company::Iterate()) {
		if (c->index != n) break;
		n++;
	}

	AIConfig *config = AIConfig::GetConfig((CompanyID)n);
	if (argv.size() >= 2) {
		config->Change(argv[1], -1, false);

		/* If the name is not found, and there is a dot in the name,
		 * try again with the assumption everything right of the dot is
		 * the version the user wants to load. */
		if (!config->HasScript()) {
			StringConsumer consumer{std::string_view{argv[1]}};
			auto name = consumer.ReadUntilChar('.', StringConsumer::SKIP_ONE_SEPARATOR);
			if (consumer.AnyBytesLeft()) {
				auto version = consumer.TryReadIntegerBase<uint32_t>(10);
				if (!version.has_value()) {
					IConsolePrint(CC_ERROR, "The version is not a valid number.");
					return true;
				}
				config->Change(name, *version, true);
			}
		}

		if (!config->HasScript()) {
			IConsolePrint(CC_ERROR, "Failed to load the specified AI.");
			return true;
		}
		if (argv.size() == 3) {
			config->StringToSettings(argv[2]);
		}
	}

	/* Start a new AI company */
	Command<Commands::CompanyControl>::Post(CompanyCtrlAction::NewAI, CompanyID::Invalid(), CompanyRemoveReason::None, ClientID::Invalid);

	return true;
}

/** Reload/restart an AI. @copydoc IConsoleCmdProc */
static bool ConReloadAI(std::span<std::string_view> argv)
{
	if (argv.size() != 2) {
		IConsolePrint(CC_HELP, "Reload an AI. Usage: 'reload_ai <company-id>'.");
		IConsolePrint(CC_HELP, "Reload the AI with the given company id. For company-id's, see the list of companies from the dropdown menu. Company 1 is 1, etc.");
		return true;
	}

	if (_game_mode != GameMode::Normal) {
		IConsolePrint(CC_ERROR, "AIs can only be managed in a game.");
		return true;
	}

	if (_networking && !_network_server) {
		IConsolePrint(CC_ERROR, "Only the server can reload an AI.");
		return true;
	}

	auto company_id = ParseCompanyID(argv[1]);
	if (!company_id.has_value()) {
		IConsolePrint(CC_ERROR, "The given company-id is not a valid number.");
		return true;
	}

	if (!Company::IsValidID(*company_id)) {
		IConsolePrint(CC_ERROR, "Unknown company. Company range is between 1 and {}.", MAX_COMPANIES);
		return true;
	}

	/* In singleplayer mode the player can be in an AI company, after cheating or loading network save with an AI in first slot. */
	if (Company::IsHumanID(*company_id) || *company_id == _local_company) {
		IConsolePrint(CC_ERROR, "Company is not controlled by an AI.");
		return true;
	}

	/* First kill the company of the AI, then start a new one. This should start the current AI again */
	Command<Commands::CompanyControl>::Post(CompanyCtrlAction::Delete, *company_id, CompanyRemoveReason::Manual, ClientID::Invalid);
	Command<Commands::CompanyControl>::Post(CompanyCtrlAction::NewAI, *company_id, CompanyRemoveReason::None, ClientID::Invalid);
	IConsolePrint(CC_DEFAULT, "AI reloaded.");

	return true;
}

/** Stop a currently running AI. @copydoc IConsoleCmdProc */
static bool ConStopAI(std::span<std::string_view> argv)
{
	if (argv.size() != 2) {
		IConsolePrint(CC_HELP, "Stop an AI. Usage: 'stop_ai <company-id>'.");
		IConsolePrint(CC_HELP, "Stop the AI with the given company id. For company-id's, see the list of companies from the dropdown menu. Company 1 is 1, etc.");
		return true;
	}

	if (_game_mode != GameMode::Normal) {
		IConsolePrint(CC_ERROR, "AIs can only be managed in a game.");
		return true;
	}

	if (_networking && !_network_server) {
		IConsolePrint(CC_ERROR, "Only the server can stop an AI.");
		return true;
	}

	auto company_id = ParseCompanyID(argv[1]);
	if (!company_id.has_value()) {
		IConsolePrint(CC_ERROR, "The given company-id is not a valid number.");
		return true;
	}

	if (!Company::IsValidID(*company_id)) {
		IConsolePrint(CC_ERROR, "Unknown company. Company range is between 1 and {}.", MAX_COMPANIES);
		return true;
	}

	/* In singleplayer mode the player can be in an AI company, after cheating or loading network save with an AI in first slot. */
	if (Company::IsHumanID(*company_id) || *company_id == _local_company) {
		IConsolePrint(CC_ERROR, "Company is not controlled by an AI.");
		return true;
	}

	/* Now kill the company of the AI. */
	Command<Commands::CompanyControl>::Post(CompanyCtrlAction::Delete, *company_id, CompanyRemoveReason::Manual, ClientID::Invalid);
	IConsolePrint(CC_DEFAULT, "AI stopped, company deleted.");

	return true;
}

/** Rescan the folder structure for new/changed AIs and libraries. @copydoc IConsoleCmdProc */
static bool ConRescanAI(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Rescan the AI dir for scripts. Usage: 'rescan_ai'.");
		return true;
	}

	if (_networking && !_network_server) {
		IConsolePrint(CC_ERROR, "Only the server can rescan the AI dir for scripts.");
		return true;
	}

	AI::Rescan();

	return true;
}

/** Rescan the folder structure for new/changed game scripts and libraries. @copydoc IConsoleCmdProc */
static bool ConRescanGame(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Rescan the Game Script dir for scripts. Usage: 'rescan_game'.");
		return true;
	}

	if (_networking && !_network_server) {
		IConsolePrint(CC_ERROR, "Only the server can rescan the Game Script dir for scripts.");
		return true;
	}

	Game::Rescan();

	return true;
}

/** Rescan the folder structure for new/changed NewGRFs. @copydoc IConsoleCmdProc */
static bool ConRescanNewGRF(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Rescan the data dir for NewGRFs. Usage: 'rescan_newgrf'.");
		return true;
	}

	if (!RequestNewGRFScan()) {
		IConsolePrint(CC_ERROR, "NewGRF scanning is already running. Please wait until completed to run again.");
	}

	return true;
}

/** Get the seed that was used to create this game. @copydoc IConsoleCmdProc */
static bool ConGetSeed(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Returns the seed used to create this game. Usage: 'getseed'.");
		IConsolePrint(CC_HELP, "The seed can be used to reproduce the exact same map as the game started with.");
		return true;
	}

	IConsolePrint(CC_DEFAULT, "Generation Seed: {}", _settings_game.game_creation.generation_seed);
	return true;
}

/** Get the current game date. @copydoc IConsoleCmdProc */
static bool ConGetDate(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Returns the current date (year-month-day) of the game. Usage: 'getdate'.");
		return true;
	}

	TimerGameCalendar::YearMonthDay ymd = TimerGameCalendar::ConvertDateToYMD(TimerGameCalendar::date);
	IConsolePrint(CC_DEFAULT, "Date: {:04d}-{:02d}-{:02d}", ymd.year, ymd.month + 1, ymd.day);
	return true;
}

/** Get the current system date. @copydoc IConsoleCmdProc */
static bool ConGetSysDate(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Returns the current date (year-month-day) of your system. Usage: 'getsysdate'.");
		return true;
	}

	IConsolePrint(CC_DEFAULT, "System Date: {:%Y-%m-%d %H:%M:%S}", fmt::localtime(time(nullptr)));
	return true;
}

/** Create an alias for a command. @copydoc IConsoleCmdProc */
static bool ConAlias(std::span<std::string_view> argv)
{
	IConsoleAlias *alias;

	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Add a new alias, or redefine the behaviour of an existing alias . Usage: 'alias <name> <command>'.");
		return true;
	}

	if (argv.size() < 3) return false;

	alias = IConsole::AliasGet(std::string(argv[1]));
	if (alias == nullptr) {
		IConsole::AliasRegister(std::string(argv[1]), argv[2]);
	} else {
		alias->cmdline = argv[2];
	}
	return true;
}

/** Make a screenshot. @copydoc IConsoleCmdProc */
static bool ConScreenShot(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Create a screenshot of the game. Usage: 'screenshot [viewport | normal | big | giant | heightmap | minimap] [no_con] [size <width> <height>] [<filename>]'.");
		IConsolePrint(CC_HELP, "  'viewport' (default) makes a screenshot of the current viewport (including menus, windows).");
		IConsolePrint(CC_HELP, "  'normal' makes a screenshot of the visible area.");
		IConsolePrint(CC_HELP, "  'big' makes a zoomed-in screenshot of the visible area.");
		IConsolePrint(CC_HELP, "  'giant' makes a screenshot of the whole map.");
		IConsolePrint(CC_HELP, "  'heightmap' makes a heightmap screenshot of the map that can be loaded in as heightmap.");
		IConsolePrint(CC_HELP, "  'minimap' makes a top-viewed minimap screenshot of the whole world which represents one tile by one pixel.");
		IConsolePrint(CC_HELP, "  'no_con' hides the console to create the screenshot (only useful in combination with 'viewport').");
		IConsolePrint(CC_HELP, "  'size' sets the width and height of the viewport to make a screenshot of (only useful in combination with 'normal' or 'big').");
		IConsolePrint(CC_HELP, "  A filename ending in # will prevent overwriting existing files and will number files counting upwards.");
		return true;
	}

	if (argv.size() > 7) return false;

	ScreenshotType type = SC_VIEWPORT;
	uint32_t width = 0;
	uint32_t height = 0;
	std::string name{};
	uint32_t arg_index = 1;

	if (argv.size() > arg_index) {
		if (argv[arg_index] == "viewport") {
			type = SC_VIEWPORT;
			arg_index += 1;
		} else if (argv[arg_index] == "normal") {
			type = SC_DEFAULTZOOM;
			arg_index += 1;
		} else if (argv[arg_index] == "big") {
			type = SC_ZOOMEDIN;
			arg_index += 1;
		} else if (argv[arg_index] == "giant") {
			type = SC_WORLD;
			arg_index += 1;
		} else if (argv[arg_index] == "heightmap") {
			type = SC_HEIGHTMAP;
			arg_index += 1;
		} else if (argv[arg_index] == "minimap") {
			type = SC_MINIMAP;
			arg_index += 1;
		}
	}

	if (argv.size() > arg_index && argv[arg_index] == "no_con") {
		if (type != SC_VIEWPORT) {
			IConsolePrint(CC_ERROR, "'no_con' can only be used in combination with 'viewport'.");
			return true;
		}
		IConsoleClose();
		arg_index += 1;
	}

	if (argv.size() > arg_index + 2 && argv[arg_index] == "size") {
		/* size <width> <height> */
		if (type != SC_DEFAULTZOOM && type != SC_ZOOMEDIN) {
			IConsolePrint(CC_ERROR, "'size' can only be used in combination with 'normal' or 'big'.");
			return true;
		}
		auto t = ParseInteger(argv[arg_index + 1]);
		if (!t.has_value()) {
			IConsolePrint(CC_ERROR, "Invalid width '{}'", argv[arg_index + 1]);
			return true;
		}
		width = *t;

		t = ParseInteger(argv[arg_index + 2]);
		if (!t.has_value()) {
			IConsolePrint(CC_ERROR, "Invalid height '{}'", argv[arg_index + 2]);
			return true;
		}
		height = *t;
		arg_index += 3;
	}

	if (argv.size() > arg_index) {
		/* Last parameter that was not one of the keywords must be the filename. */
		name = argv[arg_index];
		arg_index += 1;
	}

	if (argv.size() > arg_index) {
		/* We have parameters we did not process; means we misunderstood any of the above. */
		return false;
	}

	MakeScreenshot(type, std::move(name), width, height);
	return true;
}

/** Get debug information about a command. @copydoc IConsoleCmdProc */
static bool ConInfoCmd(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Print out debugging information about a command. Usage: 'info_cmd <cmd>'.");
		return true;
	}

	if (argv.size() < 2) return false;

	const IConsoleCmd *cmd = IConsole::CmdGet(std::string(argv[1]));
	if (cmd == nullptr) {
		IConsolePrint(CC_ERROR, "The given command was not found.");
		return true;
	}

	IConsolePrint(CC_DEFAULT, "Command name: '{}'", cmd->name);

	if (cmd->hook != nullptr) IConsolePrint(CC_DEFAULT, "Command is hooked.");

	return true;
}

/** Change the debug levels of the game. @copydoc IConsoleCmdProc */
static bool ConDebugLevel(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Get/set the default debugging level for the game. Usage: 'debug_level [<level>]'.");
		IConsolePrint(CC_HELP, "Level can be any combination of names, levels. Eg 'net=5 ms=4'. Remember to enclose it in \"'\"s.");
		return true;
	}

	if (argv.size() > 2) return false;

	if (argv.size() == 1) {
		IConsolePrint(CC_DEFAULT, "Current debug-level: '{}'", GetDebugString());
	} else {
		SetDebugString(argv[1], [](std::string_view err) { IConsolePrint(CC_ERROR, "{}", err); });
	}

	return true;
}

/** Exit the game, i.e. exit the complete application. @copydoc IConsoleCmdProc */
static bool ConExit(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Exit the game. Usage: 'exit'.");
		return true;
	}

	if (_game_mode == GameMode::Normal && _settings_client.gui.autosave_on_exit) DoExitSave();

	_exit_game = true;
	return true;
}

/** Part the game, i.e. go back to the main menu. @copydoc IConsoleCmdProc */
static bool ConPart(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Leave the currently joined/running game (only ingame). Usage: 'part'.");
		return true;
	}

	if (_game_mode != GameMode::Normal) return false;

	if (_network_dedicated) {
		IConsolePrint(CC_ERROR, "A dedicated server can not leave the game.");
		return false;
	}

	_switch_mode = SwitchMode::Menu;
	return true;
}

/** Show generic help and specific help for commands. @copydoc IConsoleCmdProc */
static bool ConHelp(std::span<std::string_view> argv)
{
	if (argv.size() == 2) {
		const IConsoleCmd *cmd;
		const IConsoleAlias *alias;

		cmd = IConsole::CmdGet(std::string(argv[1]));
		if (cmd != nullptr) {
			cmd->proc({});
			return true;
		}

		alias = IConsole::AliasGet(std::string(argv[1]));
		if (alias != nullptr) {
			cmd = IConsole::CmdGet(alias->cmdline);
			if (cmd != nullptr) {
				cmd->proc({});
				return true;
			}
			IConsolePrint(CC_ERROR, "Alias is of special type, please see its execution-line: '{}'.", alias->cmdline);
			return true;
		}

		IConsolePrint(CC_ERROR, "Command not found.");
		return true;
	}

	IConsolePrint(TextColour::LightBlue, " ---- OpenTTD Console Help ---- ");
	IConsolePrint(CC_DEFAULT, " - commands: the command to list all commands is 'list_cmds'.");
	IConsolePrint(CC_DEFAULT, " call commands with '<command> <arg2> <arg3>...'");
	IConsolePrint(CC_DEFAULT, " - to assign strings, or use them as arguments, enclose it within quotes.");
	IConsolePrint(CC_DEFAULT, " like this: '<command> \"string argument with spaces\"'.");
	IConsolePrint(CC_DEFAULT, " - use 'help <command>' to get specific information.");
	IConsolePrint(CC_DEFAULT, " - scroll console output with shift + (up | down | pageup | pagedown).");
	IConsolePrint(CC_DEFAULT, " - scroll console input history with the up or down arrows.");
	IConsolePrint(CC_DEFAULT, "");
	return true;
}

/** List all registered commands that are not hidden. @copydoc IConsoleCmdProc */
static bool ConListCommands(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "List all registered commands. Usage: 'list_cmds [<pre-filter>]'.");
		return true;
	}

	for (auto &it : IConsole::Commands()) {
		const IConsoleCmd *cmd = &it.second;
		if (argv.size() <= 1|| cmd->name.find(argv[1]) != std::string::npos) {
			if (cmd->hook == nullptr || cmd->hook(false) != ConsoleHookResult::Hide) IConsolePrint(CC_DEFAULT, cmd->name);
		}
	}

	return true;
}

/** List all registered aliases. @copydoc IConsoleCmdProc */
static bool ConListAliases(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "List all registered aliases. Usage: 'list_aliases [<pre-filter>]'.");
		return true;
	}

	for (auto &it : IConsole::Aliases()) {
		const IConsoleAlias *alias = &it.second;
		if (argv.size() <= 1 || alias->name.find(argv[1]) != std::string::npos) {
			IConsolePrint(CC_DEFAULT, "{} => {}", alias->name, alias->cmdline);
		}
	}

	return true;
}

/** List all companies. @copydoc IConsoleCmdProc */
static bool ConCompanies(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "List the details of all companies in the game. Usage 'companies'.");
		return true;
	}

	for (const Company *c : Company::Iterate()) {
		/* Grab the company name */
		std::string company_name = GetString(STR_COMPANY_NAME, c->index);

		std::string colour = GetString(STR_COLOUR_DARK_BLUE + to_underlying(_company_colours[c->index]));
		IConsolePrint(CC_INFO, "#:{}({}) Company Name: '{}'  Year Founded: {}  Money: {}  Loan: {}  Value: {}  (T:{}, R:{}, P:{}, S:{}) {}",
			c->index + 1, colour, company_name,
			c->inaugurated_year, (int64_t)c->money, (int64_t)c->current_loan, (int64_t)CalculateCompanyValue(c),
			c->group_all[VehicleType::Train].num_vehicle,
			c->group_all[VehicleType::Road].num_vehicle,
			c->group_all[VehicleType::Aircraft].num_vehicle,
			c->group_all[VehicleType::Ship].num_vehicle,
			c->is_ai ? "AI" : "");
	}

	return true;
}

/* Supply-driven town growth: inspect needs and the power grid, force need levels, add fuel. */

/**
 * Get a short name of the growth status of a town for the console.
 * @param t The town.
 * @return The name of the status.
 */
static std::string GetTownGrowthStatusText(const Town *t)
{
	switch (t->growth.status) {
		case TownGrowthStatus::Growing: return "growing";
		case TownGrowthStatus::Blocked:
			if (t->growth.blocking_need < TownNeed::End) return fmt::format("blocked:{}", GetString(GetTownNeedName(t->growth.blocking_need)));
			return "blocked";
		case TownGrowthStatus::NoSpace: return "no space";
		case TownGrowthStatus::Disabled: return "disabled";
		case TownGrowthStatus::Custom: return "custom (GS)";
		default: return fmt::format("status {}", to_underlying(t->growth.status));
	}
}

/**
 * Format a value per mille as a decimal multiplier, e.g. 6554 as "6.554".
 * @param per_mille The value per mille.
 * @return The formatted value.
 */
static std::string FormatPerMille(uint32_t per_mille)
{
	return fmt::format("{}.{:03}", per_mille / 1000, per_mille % 1000);
}

/**
 * Print the needs of one town in detail.
 * @param t The town.
 */
static void PrintTownNeedsDetails(const Town *t)
{
	const TownGrowthState &g = t->growth;
	IConsolePrint(CC_INFO, "Town {} '{}': population {}, houses {}, status {}",
		t->index, GetString(STR_TOWN_NAME, t->index), t->cache.population, t->cache.num_houses, GetTownGrowthStatusText(t));
	IConsolePrint(CC_INFO, "  multiplier x{}, growth {} residents/year, budget {} residents",
		FormatPerMille(g.multiplier), g.yearly_growth, g.budget / 1000);
	IConsolePrint(CC_INFO, "  {:<12} {:>8} {:>5} {:>4} {:>6} {:>9} {:>9} {:>9}  {}",
		"need", "unlocked", "level", "raw", "forced", "coverage", "supplied", "demand", "coverage history");
	for (TownNeed n : EnumRange(TownNeed::End)) {
		const TownNeedState &s = g.needs[n];
		IConsolePrint(CC_INFO, "  {:<12} {:>8} {:>5} {:>4} {:>6} {:>8}% {:>9} {:>9}  {}/{}/{}%",
			GetString(GetTownNeedName(n)),
			IsTownNeedUnlocked(t, n) ? "yes" : fmt::format("at {}", GetTownNeedUnlockPopulation(n)),
			GetTownNeedEffectiveLevel(t, n), s.level,
			s.forced == TOWN_NEED_NOT_FORCED ? std::string("-") : fmt::format("{}", s.forced),
			s.coverage, s.supplied, s.demand,
			s.coverage_history[0], s.coverage_history[1], s.coverage_history[2]);
	}
	if (g.power_links.empty()) {
		IConsolePrint(CC_INFO, "  power plants: none linked");
	} else {
		for (const TownPowerLink &link : g.power_links) {
			const Industry *i = Industry::GetIfValid(link.plant);
			IConsolePrint(CC_INFO, "  power plant {} '{}': delivered {} last month", link.plant,
				i == nullptr ? std::string("?") : GetString(STR_INDUSTRY_NAME, link.plant), link.delivered);
		}
	}
}

/** Show the needs and growth of towns. @copydoc IConsoleCmdProc */
static bool ConTownNeeds(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Show the needs and growth of all towns, or the details of one town. Usage: 'town_needs [<town id>]'.");
		IConsolePrint(CC_HELP, "Levels are shown as effective/raw; '*' marks a forced level, '-' a need the town is too small for.");
		return true;
	}

	if (argv.size() > 2) return false;

	if (argv.size() == 2) {
		auto town_id = ParseType<TownID>(argv[1]);
		if (!town_id.has_value() || !Town::IsValidID(*town_id)) {
			IConsolePrint(CC_ERROR, "Unknown town '{}'.", argv[1]);
			return true;
		}
		PrintTownNeedsDetails(Town::Get(*town_id));
		return true;
	}

	std::vector<const Town *> towns;
	for (const Town *t : Town::Iterate()) towns.push_back(t);
	std::ranges::sort(towns, [](const Town *a, const Town *b) {
		if (a->cache.population != b->cache.population) return a->cache.population > b->cache.population;
		return a->index < b->index;
	});

	IConsolePrint(CC_INFO, "{:>5} {:>10} {:>5} {:>5} {:>5} {:>5} {:>5} {:>20} {:>7} {:>8} {:>8}  {:<16} {}",
		"id", "population", "pass", "mail", "goods", "elec", "valu", "coverage %", "mult", "growth/y", "budget", "status", "name");
	for (const Town *t : towns) {
		const TownGrowthState &g = t->growth;
		std::array<std::string, to_underlying(TownNeed::End)> levels;
		std::string coverage;
		for (TownNeed n : EnumRange(TownNeed::End)) {
			const TownNeedState &s = g.needs[n];
			const char *mark = s.forced != TOWN_NEED_NOT_FORCED ? "*" : (IsTownNeedUnlocked(t, n) ? "" : "-");
			levels[to_underlying(n)] = fmt::format("{}/{}{}", GetTownNeedEffectiveLevel(t, n), s.level, mark);
			format_append(coverage, "{}{}", n == TownNeed::Begin ? "" : "/", s.coverage);
		}
		IConsolePrint(CC_INFO, "{:>5} {:>10} {:>5} {:>5} {:>5} {:>5} {:>5} {:>20} {:>7} {:>8} {:>8}  {:<16} {}",
			t->index, t->cache.population, levels[0], levels[1], levels[2], levels[3], levels[4], coverage,
			FormatPerMille(g.multiplier), g.yearly_growth, g.budget / 1000, GetTownGrowthStatusText(t),
			GetString(STR_TOWN_NAME, t->index));
	}
	return true;
}

/** Force the level of town needs, or let them follow the supply again. @copydoc IConsoleCmdProc */
static bool ConTownForceNeed(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Force the level of a town need, or let it follow the supply again. Usage: 'town_force_need <town id|all> <p|m|g|e|all> <0..5|auto>'.");
		IConsolePrint(CC_HELP, "Needs: p = passengers, m = mail, g = goods, e = electricity. A forced level ignores the unlock and small town rules.");
		return true;
	}

	if (argv.size() != 4) return false;

	if (_game_mode == GameMode::Menu) {
		IConsolePrint(CC_ERROR, "There is no game running.");
		return true;
	}

	TownID town_id = TownID::Invalid();
	if (argv[1] != "all") {
		auto id = ParseType<TownID>(argv[1]);
		if (!id.has_value() || !Town::IsValidID(*id)) {
			IConsolePrint(CC_ERROR, "Unknown town '{}'.", argv[1]);
			return true;
		}
		town_id = *id;
	}

	TownNeed need;
	if (argv[2] == "p") {
		need = TownNeed::Passengers;
	} else if (argv[2] == "m") {
		need = TownNeed::Mail;
	} else if (argv[2] == "g") {
		need = TownNeed::Goods;
	} else if (argv[2] == "e") {
		need = TownNeed::Electricity;
	} else if (argv[2] == "v") {
		need = TownNeed::Valuables;
	} else if (argv[2] == "all") {
		need = TownNeed::End;
	} else {
		IConsolePrint(CC_ERROR, "Unknown need '{}', use p, m, g, e, v or all.", argv[2]);
		return true;
	}

	uint8_t level = TOWN_NEED_NOT_FORCED;
	if (argv[3] != "auto") {
		auto value = ParseInteger(argv[3]);
		if (!value.has_value() || *value > TOWN_NEED_LEVEL_MAX) {
			IConsolePrint(CC_ERROR, "Level must be 0..{} or 'auto'.", TOWN_NEED_LEVEL_MAX);
			return true;
		}
		level = static_cast<uint8_t>(*value);
	}

	if (!Command<Commands::TownForceNeedLevel>::Post(town_id, need, level)) {
		IConsolePrint(CC_ERROR, "Forcing the need level failed.");
	} else if (!_networking) {
		IConsolePrint(CC_INFO, "Need level set; the growth multiplier follows at the next monthly update.");
	}
	return true;
}

/** List the power plants with their fuel stock, output and linked towns. @copydoc IConsoleCmdProc */
static bool ConPowerGrid(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "List the power plants: fuel stock, electricity delivered and asked last month, linked towns. Usage: 'power_grid'.");
		return true;
	}

	std::map<IndustryID, std::vector<TownID>> linked_towns;
	for (const Town *t : Town::Iterate()) {
		for (const TownPowerLink &link : t->growth.power_links) linked_towns[link.plant].push_back(t->index);
	}

	uint count = 0;
	IConsolePrint(CC_INFO, "{:>5} {:>7} {:>7} {:>7}  {:<32} {}", "id", "fuel", "output", "demand", "name", "linked towns");
	for (const Industry *i : Industry::Iterate()) {
		if (!IsPowerPlant(i)) continue;
		count++;

		std::string towns;
		for (TownID town : linked_towns[i->index]) format_append(towns, "{}{}", towns.empty() ? "" : ",", town);
		IConsolePrint(CC_INFO, "{:>5} {:>7} {:>7} {:>7}  {:<32} {}", i->index, GetPowerPlantFuelStock(i), i->power_output, i->power_demand,
			GetString(STR_INDUSTRY_NAME, i->index), towns.empty() ? std::string("-") : towns);
	}
	IConsolePrint(CC_INFO, "{} power plant(s).", count);
	return true;
}

/** Add fuel to the stock of power plants. @copydoc IConsoleCmdProc */
static bool ConPowerAddFuel(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Add fuel to the stock of a power plant, or of all of them. Usage: 'power_add_fuel <industry id|all> <amount>'.");
		IConsolePrint(CC_HELP, "The stock is capped at {} units.", UINT16_MAX);
		return true;
	}

	if (argv.size() != 3) return false;

	if (_game_mode == GameMode::Menu) {
		IConsolePrint(CC_ERROR, "There is no game running.");
		return true;
	}

	IndustryID industry_id = IndustryID::Invalid();
	if (argv[1] != "all") {
		auto id = ParseType<IndustryID>(argv[1]);
		const Industry *i = id.has_value() ? Industry::GetIfValid(*id) : nullptr;
		if (i == nullptr || !IsPowerPlant(i)) {
			IConsolePrint(CC_ERROR, "Industry '{}' is not a power plant.", argv[1]);
			return true;
		}
		industry_id = *id;
	}

	auto amount = ParseInteger(argv[2], 10, true);
	if (!amount.has_value() || *amount == 0) {
		IConsolePrint(CC_ERROR, "Amount must be a positive number.");
		return true;
	}

	if (!Command<Commands::PowerPlantAddFuel>::Post(industry_id, static_cast<uint16_t>(std::min<uint32_t>(*amount, UINT16_MAX)))) {
		IConsolePrint(CC_ERROR, "Adding fuel failed.");
	}
	return true;
}

/** Write the simulation line of every town to the debug output now. @copydoc IConsoleCmdProc */
static bool ConTownSimLog(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Write the machine readable [townsim] line of every town to the debug output now. Usage: 'townsim_log'.");
		IConsolePrint(CC_HELP, "With 'debug_level town=1' these lines are also written at the start of every year.");
		return true;
	}

	LogTownGrowthSimulation(true);
	return true;
}

/** Say something to all clients in a network game. @copydoc IConsoleCmdProc */
static bool ConSay(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Chat to your fellow players in a multiplayer game. Usage: 'say \"<msg>\"'.");
		return true;
	}

	if (argv.size() != 2) return false;

	if (!_network_server) {
		NetworkClientSendChat(NetworkAction::ChatBroadcast, NetworkChatDestinationType::Broadcast, 0 /* param does not matter */, argv[1]);
	} else {
		bool from_admin = (_redirect_console_to_admin < AdminID::Invalid());
		NetworkServerSendChat(NetworkAction::ChatBroadcast, NetworkChatDestinationType::Broadcast, 0, argv[1], ClientID::Server, from_admin);
	}

	return true;
}

/** Say something to all clients in your company in a network game. @copydoc IConsoleCmdProc */
static bool ConSayCompany(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Chat to a certain company in a multiplayer game. Usage: 'say_company <company-no> \"<msg>\"'.");
		IConsolePrint(CC_HELP, "CompanyNo is the company that plays as company <companyno>, 1 through max_companies.");
		return true;
	}

	if (argv.size() != 3) return false;

	auto company_id = ParseCompanyID(argv[1]);
	if (!company_id.has_value()) {
		IConsolePrint(CC_ERROR, "The given company-id is not a valid number.");
		return true;
	}

	if (!Company::IsValidID(*company_id)) {
		IConsolePrint(CC_DEFAULT, "Unknown company. Company range is between 1 and {}.", MAX_COMPANIES);
		return true;
	}

	if (!_network_server) {
		NetworkClientSendChat(NetworkAction::ChatTeam, NetworkChatDestinationType::Team, company_id->base(), argv[2]);
	} else {
		bool from_admin = (_redirect_console_to_admin < AdminID::Invalid());
		NetworkServerSendChat(NetworkAction::ChatTeam, NetworkChatDestinationType::Team, company_id->base(), argv[2], ClientID::Server, from_admin);
	}

	return true;
}

/** Say something to a specific client in a network game. @copydoc IConsoleCmdProc */
static bool ConSayClient(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Chat to a certain client in a multiplayer game. Usage: 'say_client <client-id> \"<msg>\"'.");
		IConsolePrint(CC_HELP, "For client-id's, see the command 'clients'.");
		return true;
	}

	if (argv.size() != 3) return false;

	auto client_id = ParseType<ClientID>(argv[1]);
	if (!client_id.has_value()) {
		IConsolePrint(CC_ERROR, "The given client-id is not a valid number.");
		return true;
	}

	if (!_network_server) {
		NetworkClientSendChat(NetworkAction::ChatClient, NetworkChatDestinationType::Client, to_underlying(*client_id), argv[2]);
	} else {
		bool from_admin = (_redirect_console_to_admin < AdminID::Invalid());
		NetworkServerSendChat(NetworkAction::ChatClient, NetworkChatDestinationType::Client, to_underlying(*client_id), argv[2], ClientID::Server, from_admin);
	}

	return true;
}

/** All the known authorized keys with their name. */
static const std::initializer_list<std::pair<std::string_view, NetworkAuthorizedKeys *>> _console_cmd_authorized_keys{
	{ "admin", &_settings_client.network.admin_authorized_keys },
	{ "rcon", &_settings_client.network.rcon_authorized_keys },
	{ "server", &_settings_client.network.server_authorized_keys },
};

/** Actions that can be performed on authorized keys from the console. */
enum class ConNetworkAuthorizedKeyAction : uint8_t {
	List, ///< List all authorized keys.
	Add, ///< Add an authorized key.
	Remove, ///< Remove an authorized key.
};

static void PerformNetworkAuthorizedKeyAction(std::string_view name, NetworkAuthorizedKeys *authorized_keys, ConNetworkAuthorizedKeyAction action, const std::string &authorized_key, CompanyID company = CompanyID::Invalid())
{
	switch (action) {
		case ConNetworkAuthorizedKeyAction::List:
			IConsolePrint(CC_WHITE, "The authorized keys for {} are:", name);
			for (auto &ak : *authorized_keys) IConsolePrint(CC_INFO, "  {}", ak);
			return;

		case ConNetworkAuthorizedKeyAction::Add:
			if (authorized_keys->Contains(authorized_key)) {
				IConsolePrint(CC_WARNING, "Not added {} to {} as it already exists.", authorized_key, name);
				return;
			}

			if (company == CompanyID::Invalid()) {
				authorized_keys->Add(authorized_key);
			} else {
				AutoRestoreBackup backup(_current_company, company);
				Command<Commands::CompanyAllowListControl>::Post(CompanyAllowListCtrlAction::AddKey, authorized_key);
			}
			IConsolePrint(CC_INFO, "Added {} to {}.", authorized_key, name);
			return;

		case ConNetworkAuthorizedKeyAction::Remove:
			if (!authorized_keys->Contains(authorized_key)) {
				IConsolePrint(CC_WARNING, "Not removed {} from {} as it does not exist.", authorized_key, name);
				return;
			}

			if (company == CompanyID::Invalid()) {
				authorized_keys->Remove(authorized_key);
			} else {
				AutoRestoreBackup backup(_current_company, company);
				Command<Commands::CompanyAllowListControl>::Post(CompanyAllowListCtrlAction::RemoveKey, authorized_key);
			}
			IConsolePrint(CC_INFO, "Removed {} from {}.", authorized_key, name);
			return;
	}
}

/** Management of authorized keys. @copydoc IConsoleCmdProc */
static bool ConNetworkAuthorizedKey(std::span<std::string_view> argv)
{
	if (argv.size() <= 2) {
		IConsolePrint(CC_HELP, "List and update authorized keys. Usage: 'authorized_key list [type]|add [type] [key]|remove [type] [key]'.");
		IConsolePrint(CC_HELP, "  list: list all the authorized keys of the given type.");
		IConsolePrint(CC_HELP, "  add: add the given key to the authorized keys of the given type.");
		IConsolePrint(CC_HELP, "  remove: remove the given key from the authorized keys of the given type; use 'all' to remove all authorized keys.");
		IConsolePrint(CC_HELP, "Instead of a key, use 'client:<id>' to add/remove the key of that given client.");

		std::string buffer;
		for (auto [name, _] : _console_cmd_authorized_keys) format_append(buffer, ", {}", name);
		IConsolePrint(CC_HELP, "The supported types are: all{} and company:<id>.", buffer);
		return true;
	}

	ConNetworkAuthorizedKeyAction action;
	std::string_view action_string = argv[1];
	if (StrEqualsIgnoreCase(action_string, "list")) {
		action = ConNetworkAuthorizedKeyAction::List;
	} else if (StrEqualsIgnoreCase(action_string, "add")) {
		action = ConNetworkAuthorizedKeyAction::Add;
	} else if (StrEqualsIgnoreCase(action_string, "remove") || StrEqualsIgnoreCase(action_string, "delete")) {
		action = ConNetworkAuthorizedKeyAction::Remove;
	} else {
		IConsolePrint(CC_WARNING, "No valid action was given.");
		return false;
	}

	std::string authorized_key;
	if (action != ConNetworkAuthorizedKeyAction::List) {
		if (argv.size() <= 3) {
			IConsolePrint(CC_ERROR, "You must enter the key.");
			return false;
		}

		authorized_key = argv[3];
		if (StrStartsWithIgnoreCase(authorized_key, "client:")) {
			auto value = ParseInteger<uint32_t>(authorized_key.substr(7));
			if (value.has_value()) authorized_key = NetworkGetPublicKeyOfClient(static_cast<ClientID>(*value));
			if (!value.has_value() || authorized_key.empty()) {
				IConsolePrint(CC_ERROR, "You must enter a valid client id; see 'clients'.");
				return false;
			}
		}

		if (authorized_key.size() != NETWORK_PUBLIC_KEY_LENGTH - 1) {
			IConsolePrint(CC_ERROR, "You must enter a valid authorized key.");
			return false;
		}
	}

	std::string_view type = argv[2];
	if (StrEqualsIgnoreCase(type, "all")) {
		for (auto [name, authorized_keys] : _console_cmd_authorized_keys) PerformNetworkAuthorizedKeyAction(name, authorized_keys, action, authorized_key);
		for (Company *c : Company::Iterate()) PerformNetworkAuthorizedKeyAction(fmt::format("company:{}", c->index + 1), &c->allow_list, action, authorized_key, c->index);
		return true;
	}

	if (StrStartsWithIgnoreCase(type, "company:")) {
		auto value = ParseInteger<uint32_t>(type.substr(8));
		Company *c = value.has_value() ? Company::GetIfValid(*value - 1) : nullptr;
		if (c == nullptr) {
			IConsolePrint(CC_ERROR, "You must enter a valid company id; see 'companies'.");
			return false;
		}

		PerformNetworkAuthorizedKeyAction(type, &c->allow_list, action, authorized_key, c->index);
		return true;
	}

	for (auto [name, authorized_keys] : _console_cmd_authorized_keys) {
		if (!StrEqualsIgnoreCase(type, name)) continue;

		PerformNetworkAuthorizedKeyAction(name, authorized_keys, action, authorized_key);
		return true;
	}

	IConsolePrint(CC_WARNING, "No valid type was given.");
	return false;
}


/* Content downloading only is available with ZLIB */
#if defined(WITH_ZLIB)

/**
 * Resolve a string to a content type.
 * @param str The string to resolve.
 * @return The content type, or #ContentType::End when the string is not a content type.
 */
static ContentType StringToContentType(std::string_view str)
{
	static const std::initializer_list<std::pair<std::string_view, ContentType>> content_types = {
		{"base",      ContentType::BaseGraphics},
		{"newgrf",    ContentType::NewGRF},
		{"ai",        ContentType::Ai},
		{"ailib",     ContentType::AiLibrary},
		{"scenario",  ContentType::Scenario},
		{"heightmap", ContentType::Heightmap},
	};
	for (const auto &ct : content_types) {
		if (StrEqualsIgnoreCase(str, ct.first)) return ct.second;
	}
	return ContentType::End;
}

/** Asynchronous callback */
struct ConsoleContentCallback : public ContentCallback {
	void OnConnect(bool success) override
	{
		IConsolePrint(CC_DEFAULT, "Content server connection {}.", success ? "established" : "failed");
	}

	void OnDisconnect() override
	{
		IConsolePrint(CC_DEFAULT, "Content server connection closed.");
	}

	void OnDownloadComplete(ContentID cid) override
	{
		IConsolePrint(CC_DEFAULT, "Completed download of {}.", cid);
	}
};

/**
 * Outputs content state information to console
 * @param ci the content info
 */
static void OutputContentState(const ContentInfo &ci)
{
	static constexpr EnumIndexArray<std::string_view, ContentType, ContentType::End> types{
		"", "Base graphics", "NewGRF", "AI", "AI library", "Scenario", "Heightmap", "Base sound", "Base music", "Game script", "GS library"
	};
	static constexpr EnumIndexArray<std::string_view, ContentInfo::State, ContentInfo::State::End> states{
		"Not selected", "Selected", "Dep Selected", "Installed", "Unknown"
	};
	static constexpr EnumIndexArray<TextColour, ContentInfo::State, ContentInfo::State::End> state_to_colour{
		CC_COMMAND, CC_INFO, CC_INFO, CC_WHITE, CC_ERROR
	};

	IConsolePrint(state_to_colour[ci.state], "{}, {}, {}, {}, {:08X}, {}", ci.id, types[ci.type], states[ci.state], ci.name, ci.unique_id, FormatArrayAsHex(ci.md5sum));
}

/** Downloading of content from the server. @copydoc IConsoleCmdProc */
static bool ConContent(std::span<std::string_view> argv)
{
	[[maybe_unused]] static ContentCallback *const cb = []() {
			auto res = new ConsoleContentCallback();
			_network_content_client.AddCallback(res);
			return res;
		}();

	if (argv.size() <= 1) {
		IConsolePrint(CC_HELP, "Query, select and download content. Usage: 'content update|upgrade|select [id]|unselect [all|id]|state [filter]|download'.");
		IConsolePrint(CC_HELP, "  update: get a new list of downloadable content; must be run first.");
		IConsolePrint(CC_HELP, "  upgrade: select all items that are upgrades.");
		IConsolePrint(CC_HELP, "  select: select a specific item given by its id. If no parameter is given, all selected content will be listed.");
		IConsolePrint(CC_HELP, "  unselect: unselect a specific item given by its id or 'all' to unselect all.");
		IConsolePrint(CC_HELP, "  state: show the download/select state of all downloadable content. Optionally give a filter string.");
		IConsolePrint(CC_HELP, "  download: download all content you've selected.");
		return true;
	}

	if (StrEqualsIgnoreCase(argv[1], "update")) {
		_network_content_client.RequestContentList((argv.size() > 2) ? StringToContentType(argv[2]) : ContentType::End);
		return true;
	}

	if (StrEqualsIgnoreCase(argv[1], "upgrade")) {
		_network_content_client.SelectUpgrade();
		return true;
	}

	if (StrEqualsIgnoreCase(argv[1], "select")) {
		if (argv.size() <= 2) {
			/* List selected content */
			IConsolePrint(CC_WHITE, "id, type, state, name");
			for (const ContentInfo &ci : _network_content_client.Info()) {
				if (ci.state != ContentInfo::State::Selected && ci.state != ContentInfo::State::Autoselected) continue;
				OutputContentState(ci);
			}
		} else if (StrEqualsIgnoreCase(argv[2], "all")) {
			/* The intention of this function was that you could download
			 * everything after a filter was applied; but this never really
			 * took off. Instead, a select few people used this functionality
			 * to download every available package on BaNaNaS. This is not in
			 * the spirit of this service. Additionally, these few people were
			 * good for 70% of the consumed bandwidth of BaNaNaS. */
			IConsolePrint(CC_ERROR, "'select all' is no longer supported since 1.11.");
		} else if (auto content_id = ParseType<ContentID>(argv[2]); content_id.has_value()) {
			_network_content_client.Select(*content_id);
		} else {
			IConsolePrint(CC_ERROR, "The given content-id is not a number or 'all'");
		}
		return true;
	}

	if (StrEqualsIgnoreCase(argv[1], "unselect")) {
		if (argv.size() <= 2) {
			IConsolePrint(CC_ERROR, "You must enter the id.");
			return false;
		}
		if (StrEqualsIgnoreCase(argv[2], "all")) {
			_network_content_client.UnselectAll();
		} else if (auto content_id = ParseType<ContentID>(argv[2]); content_id.has_value()) {
			_network_content_client.Unselect(*content_id);
		} else {
			IConsolePrint(CC_ERROR, "The given content-id is not a number or 'all'");
		}
		return true;
	}

	if (StrEqualsIgnoreCase(argv[1], "state")) {
		IConsolePrint(CC_WHITE, "id, type, state, name");
		for (const ContentInfo &ci : _network_content_client.Info()) {
			if (argv.size() > 2 && !StrContainsIgnoreCase(ci.name, argv[2])) continue;
			OutputContentState(ci);
		}
		return true;
	}

	if (StrEqualsIgnoreCase(argv[1], "download")) {
		uint files;
		uint bytes;
		_network_content_client.DownloadSelectedContent(files, bytes);
		IConsolePrint(CC_DEFAULT, "Downloading {} file(s) ({} bytes).", files, bytes);
		return true;
	}

	return false;
}
#endif /* defined(WITH_ZLIB) */

/**
 * Get FontSize by name
 * @param name The name to look up.
 * @return The FontSize matching the given name,
 */
static FontSize GetFontSizeByName(std::string_view name)
{
	for (FontSize fs : EnumRange(FontSize::End)) {
		if (StrEqualsIgnoreCase(name, FontSizeToName(fs))) return fs;
	}
	return FontSize::End;
}

/** Managing the font configuration. @copydoc IConsoleCmdProc */
static bool ConFont(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Manage the fonts configuration.");
		IConsolePrint(CC_HELP, "Usage 'font'.");
		IConsolePrint(CC_HELP, "  Print out the fonts configuration.");
		IConsolePrint(CC_HELP, "  The \"Currently active\" configuration is the one actually in effect (after interface scaling and replacing unavailable fonts).");
		IConsolePrint(CC_HELP, "  The \"Requested\" configuration is the one requested via console command or config file.");
		IConsolePrint(CC_HELP, "Usage 'font [medium|small|large|mono] [<font name>] [<size>]'.");
		IConsolePrint(CC_HELP, "  Change the configuration for a font.");
		IConsolePrint(CC_HELP, "  Omitting an argument will keep the current value.");
		IConsolePrint(CC_HELP, "  Set <font name> to \"\" for the default font. Note that <size> has no effect if the default font is in use, and fixed defaults are used instead.");
		IConsolePrint(CC_HELP, "  If the sprite font is enabled in Game Options, it is used instead of the default font.");
		IConsolePrint(CC_HELP, "  The <size> is automatically multiplied by the current interface scaling.");
		return true;
	}

	if (argv.size() > 2) {
		/* First argument must be a FontSize. */
		FontSize argfs = GetFontSizeByName(argv[1]);
		if (argfs == FontSize::End) return false;

		FontCacheSubSetting *setting = GetFontCacheSubSetting(argfs);
		std::string font = setting->font;
		uint size = setting->size;
		uint8_t arg_index = 2;
		/* For <name> we want a string. */

		if (!ParseInteger(argv[arg_index]).has_value()) {
			font = argv[arg_index++];
		}

		if (argv.size() > arg_index) {
			/* For <size> we want a number. */
			auto v = ParseInteger(argv[arg_index]);
			if (v.has_value()) {
				size = *v;
				arg_index++;
			}
		}

		SetFont(argfs, font, size);
	}

	for (FontSize fs : EnumRange(FontSize::End)) {
		FontCache *fc = FontCache::Get(fs);
		FontCacheSubSetting *setting = GetFontCacheSubSetting(fs);
		/* Make sure all non sprite fonts are loaded. */
		if (!setting->font.empty() && !fc->HasParent()) {
			FontCache::LoadFontCaches(fs);
			fc = FontCache::Get(fs);
		}
		IConsolePrint(CC_DEFAULT, "{} font:", FontSizeToName(fs));
		IConsolePrint(CC_DEFAULT, "Currently active: \"{}\", size {}", fc->GetFontName(), fc->GetFontSize());
		IConsolePrint(CC_DEFAULT, "Requested: \"{}\", size {}", setting->font, setting->size);
	}

	return true;
}

/** Change settings of the current game. @copydoc IConsoleCmdProc */
static bool ConSetting(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Change setting for all clients. Usage: 'setting <name> [<value>]'.");
		IConsolePrint(CC_HELP, "Omitting <value> will print out the current value of the setting.");
		return true;
	}

	if (argv.size() == 1 || argv.size() > 3) return false;

	if (argv.size() == 2) {
		IConsoleGetSetting(argv[1]);
	} else {
		IConsoleSetSetting(argv[1], argv[2]);
	}

	return true;
}

/** Change settings of for a new game. @copydoc IConsoleCmdProc */
static bool ConSettingNewgame(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Change setting for the next game. Usage: 'setting_newgame <name> [<value>]'.");
		IConsolePrint(CC_HELP, "Omitting <value> will print out the current value of the setting.");
		return true;
	}

	if (argv.size() == 1 || argv.size() > 3) return false;

	if (argv.size() == 2) {
		IConsoleGetSetting(argv[1], true);
	} else {
		IConsoleSetSetting(argv[1], argv[2], true);
	}

	return true;
}

/** List all settings. @copydoc IConsoleCmdProc */
static bool ConListSettings(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "List settings. Usage: 'list_settings [<pre-filter>]'.");
		return true;
	}

	if (argv.size() > 2) return false;

	IConsoleListSettings((argv.size() == 2) ? argv[1] : std::string_view{});
	return true;
}

/** Print the gamelog. @copydoc IConsoleCmdProc */
static bool ConGamelogPrint(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Print logged fundamental changes to the game since the start. Usage: 'gamelog'.");
		return true;
	}

	_gamelog.PrintConsole();
	return true;
}

/** Reload all active NewGRFs. @copydoc IConsoleCmdProc */
static bool ConNewGRFReload(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Reloads all active NewGRFs from disk. Equivalent to reapplying NewGRFs via the settings, but without asking for confirmation. This might crash OpenTTD!");
		return true;
	}

	ReloadNewGRFData();
	return true;
}

/** List the locations of all of the game's different sub directories. @copydoc IConsoleCmdProc */
static bool ConListDirs(std::span<std::string_view> argv)
{
	struct SubdirNameMap {
		std::string_view name; ///< UI name for the directory
		Subdirectory subdir; ///< Index of subdirectory type
		bool default_only; ///< Whether only the default (first existing) directory for this is interesting
	};
	static const SubdirNameMap subdir_name_map[] = {
		/* Game data directories */
		{ "baseset", Subdirectory::Baseset, false },
		{ "newgrf", Subdirectory::NewGrf, false },
		{ "ai", Subdirectory::Ai, false },
		{ "ailib", Subdirectory::AiLibrary, false },
		{ "gs", Subdirectory::Gs, false },
		{ "gslib", Subdirectory::GsLibrary, false },
		{ "scenario", Subdirectory::Scenario, false },
		{ "heightmap", Subdirectory::Heightmap, false },
		/* Default save locations for user data */
		{ "save", Subdirectory::Save, true },
		{ "autosave", Subdirectory::Autosave, true },
		{ "screenshot", Subdirectory::Screenshot, true },
		{ "social_integration", Subdirectory::SocialIntegration, true },
	};

	if (argv.size() != 2) {
		IConsolePrint(CC_HELP, "List all search paths or default directories for various categories.");
		IConsolePrint(CC_HELP, "Usage: list_dirs <category>");
		std::string cats{subdir_name_map[0].name};
		bool first = true;
		for (const SubdirNameMap &sdn : subdir_name_map) {
			if (!first) {
				cats += ", ";
				cats += sdn.name;
			}
			first = false;
		}
		IConsolePrint(CC_HELP, "Valid categories: {}", cats);
		return true;
	}

	std::set<std::string> seen_dirs;
	for (const SubdirNameMap &sdn : subdir_name_map) {
		if (!StrEqualsIgnoreCase(argv[1], sdn.name))  continue;
		bool found = false;
		for (Searchpath sp : _valid_searchpaths) {
			/* Get the directory */
			std::string path = FioGetDirectory(sp, sdn.subdir);
			/* Check it hasn't already been listed */
			if (seen_dirs.find(path) != seen_dirs.end()) continue;
			seen_dirs.insert(path);
			/* Check if exists and mark found */
			bool exists = FileExists(path);
			found |= exists;
			/* Print */
			if (!sdn.default_only || exists) {
				IConsolePrint(exists ? CC_DEFAULT : CC_INFO, "{} {}", path, exists ? "[ok]" : "[not found]");
				if (sdn.default_only) break;
			}
		}
		if (!found) {
			IConsolePrint(CC_ERROR, "No directories exist for category {}", argv[1]);
		}
		return true;
	}

	IConsolePrint(CC_ERROR, "Invalid category name: {}", argv[1]);
	return false;
}

/** Management of NewGRF profiling. @copydoc IConsoleCmdProc */
static bool ConNewGRFProfile(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Collect performance data about NewGRF sprite requests and callbacks. Sub-commands can be abbreviated.");
		IConsolePrint(CC_HELP, "Usage: 'newgrf_profile [list]':");
		IConsolePrint(CC_HELP, "  List all NewGRFs that can be profiled, and their status.");
		IConsolePrint(CC_HELP, "Usage: 'newgrf_profile select <grf-num>...':");
		IConsolePrint(CC_HELP, "  Select one or more GRFs for profiling.");
		IConsolePrint(CC_HELP, "Usage: 'newgrf_profile unselect <grf-num>...':");
		IConsolePrint(CC_HELP, "  Unselect one or more GRFs from profiling. Use the keyword \"all\" instead of a GRF number to unselect all. Removing an active profiler aborts data collection.");
		IConsolePrint(CC_HELP, "Usage: 'newgrf_profile start [<num-ticks>]':");
		IConsolePrint(CC_HELP, "  Begin profiling all selected GRFs. If a number of ticks is provided, profiling stops after that many game ticks. There are 74 ticks in a calendar day.");
		IConsolePrint(CC_HELP, "Usage: 'newgrf_profile stop':");
		IConsolePrint(CC_HELP, "  End profiling and write the collected data to CSV files.");
		IConsolePrint(CC_HELP, "Usage: 'newgrf_profile abort':");
		IConsolePrint(CC_HELP, "  End profiling and discard all collected data.");
		return true;
	}

	std::span<const GRFFile> files = GetAllGRFFiles();

	/* "list" sub-command */
	if (argv.size() == 1 || StrStartsWithIgnoreCase(argv[1], "lis")) {
		IConsolePrint(CC_INFO, "Loaded GRF files:");
		int i = 1;
		for (const auto &grf : files) {
			auto profiler = std::ranges::find(_newgrf_profilers, &grf, &NewGRFProfiler::grffile);
			bool selected = profiler != _newgrf_profilers.end();
			bool active = selected && profiler->active;
			TextColour tc = active ? TextColour::LightBlue : selected ? TextColour::Green : CC_INFO;
			std::string_view statustext = active ? " (active)" : selected ? " (selected)" : "";
			IConsolePrint(tc, "{}: [{}] {}{}", i, FormatArrayAsHex(grf.grfid), grf.filename, statustext);
			i++;
		}
		return true;
	}

	/* "select" sub-command */
	if (StrStartsWithIgnoreCase(argv[1], "sel") && argv.size() >= 3) {
		for (size_t argnum = 2; argnum < argv.size(); ++argnum) {
			auto grfnum = ParseInteger(argv[argnum]);
			if (!grfnum.has_value() || *grfnum < 1 || static_cast<size_t>(*grfnum) > files.size()) {
				IConsolePrint(CC_WARNING, "GRF number {} out of range, not added.", *grfnum);
				continue;
			}
			const GRFFile *grf = &files[*grfnum - 1];
			if (std::any_of(_newgrf_profilers.begin(), _newgrf_profilers.end(), [&](NewGRFProfiler &pr) { return pr.grffile == grf; })) {
				IConsolePrint(CC_WARNING, "GRF number {} [{}] is already selected for profiling.", *grfnum, FormatArrayAsHex(grf->grfid));
				continue;
			}
			_newgrf_profilers.emplace_back(grf);
		}
		return true;
	}

	/* "unselect" sub-command */
	if (StrStartsWithIgnoreCase(argv[1], "uns") && argv.size() >= 3) {
		for (size_t argnum = 2; argnum < argv.size(); ++argnum) {
			if (StrEqualsIgnoreCase(argv[argnum], "all")) {
				_newgrf_profilers.clear();
				break;
			}
			auto grfnum = ParseInteger(argv[argnum]);
			if (!grfnum.has_value() || *grfnum < 1 || static_cast<size_t>(*grfnum) > files.size()) {
				IConsolePrint(CC_WARNING, "GRF number {} out of range, not removing.", *grfnum);
				continue;
			}
			const GRFFile *grf = &files[*grfnum - 1];
			_newgrf_profilers.erase(std::ranges::find(_newgrf_profilers, grf, &NewGRFProfiler::grffile));
		}
		return true;
	}

	/* "start" sub-command */
	if (StrStartsWithIgnoreCase(argv[1], "sta")) {
		std::string grfids;
		size_t started = 0;
		for (NewGRFProfiler &pr : _newgrf_profilers) {
			if (!pr.active) {
				pr.Start();
				started++;

				if (!grfids.empty()) grfids += ", ";
				format_append(grfids, "[{}]", FormatArrayAsHex(pr.grffile->grfid));
			}
		}
		if (started > 0) {
			IConsolePrint(CC_DEBUG, "Started profiling for GRFID{} {}.", (started > 1) ? "s" : "", grfids);

			if (argv.size() >= 3) {
				auto ticks = StringConsumer{argv[2]}.TryReadIntegerBase<uint64_t>(0);
				if (!ticks.has_value()) {
					IConsolePrint(CC_ERROR, "No valid amount of ticks was given, profiling will not stop automatically.");
				} else {
					NewGRFProfiler::StartTimer(*ticks);
					IConsolePrint(CC_DEBUG, "Profiling will automatically stop after {} ticks.", *ticks);
				}
			}
		} else if (_newgrf_profilers.empty()) {
			IConsolePrint(CC_ERROR, "No GRFs selected for profiling, did not start.");
		} else {
			IConsolePrint(CC_ERROR, "Did not start profiling for any GRFs, all selected GRFs are already profiling.");
		}
		return true;
	}

	/* "stop" sub-command */
	if (StrStartsWithIgnoreCase(argv[1], "sto")) {
		NewGRFProfiler::FinishAll();
		return true;
	}

	/* "abort" sub-command */
	if (StrStartsWithIgnoreCase(argv[1], "abo")) {
		for (NewGRFProfiler &pr : _newgrf_profilers) {
			pr.Abort();
		}
		NewGRFProfiler::AbortTimer();
		return true;
	}

	return false;
}

#ifdef _DEBUG
/******************
 *  debug commands
 ******************/

static void IConsoleDebugLibRegister()
{
	IConsole::CmdRegister("resettile",        ConResetTile);
	IConsole::AliasRegister("dbg_echo",       "echo %A; echo %B");
	IConsole::AliasRegister("dbg_echo2",      "echo %!");
}
#endif

/** Show the current framerate statistics. @copydoc IConsoleCmdProc */
static bool ConFramerate(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Show frame rate and game speed information.");
		return true;
	}

	ConPrintFramerate();
	return true;
}

/** Show the framerate statistics window. @copydoc IConsoleCmdProc */
static bool ConFramerateWindow(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Open the frame rate window.");
		return true;
	}

	if (_network_dedicated) {
		IConsolePrint(CC_ERROR, "Can not open frame rate window on a dedicated server.");
		return false;
	}

	ShowFramerateWindow();
	return true;
}

/* Supply-driven town growth: GUI debug command. */

void ShowTownSupplyWindow(TownID town);

/** Open the supply window of a town. @copydoc IConsoleCmdProc */
static bool ConTownSupplyWindow(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Open the supply and growth window of a town. Usage: 'town_supply_window <town id>'.");
		return true;
	}

	if (argv.size() != 2) return false;

	auto town = ParseType<TownID>(argv[1]);
	if (!town.has_value() || !Town::IsValidID(*town)) {
		IConsolePrint(CC_ERROR, "Unknown town id '{}'.", argv[1]);
		return true;
	}

	ShowTownSupplyWindow(*town);
	return true;
}

/* --- Rail blueprints --- */

/**
 * Parse a tile coordinate argument.
 * @param arg The argument.
 * @param max Exclusive upper bound.
 * @return The coordinate, or std::nullopt when invalid.
 */
static std::optional<uint> ParseBlueprintCoordinate(std::string_view arg, uint max)
{
	auto v = ParseType<uint>(arg);
	if (!v.has_value() || *v >= max) return std::nullopt;
	return v;
}

/** Copy the rail infrastructure of an area into a blueprint file. @copydoc IConsoleCmdProc */
static bool ConBlueprintCopy(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Copy the rail infrastructure of an area into a blueprint. Usage: 'blueprint_copy <x1> <y1> <x2> <y2> <name>'.");
		return true;
	}
	if (argv.size() != 6) return false;

	auto x1 = ParseBlueprintCoordinate(argv[1], Map::SizeX());
	auto y1 = ParseBlueprintCoordinate(argv[2], Map::SizeY());
	auto x2 = ParseBlueprintCoordinate(argv[3], Map::SizeX());
	auto y2 = ParseBlueprintCoordinate(argv[4], Map::SizeY());
	if (!x1.has_value() || !y1.has_value() || !x2.has_value() || !y2.has_value()) {
		IConsolePrint(CC_ERROR, "Invalid coordinates.");
		return true;
	}

	auto start = std::chrono::steady_clock::now();
	BlueprintCaptureStats stats;
	Blueprint bp = CaptureBlueprint(TileXY(*x1, *y1), TileXY(*x2, *y2), argv[5], &stats);
	auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
	if (!SaveBlueprint(bp)) {
		IConsolePrint(CC_ERROR, "Could not write '{}'.", GetBlueprintFileName(bp.name));
		return true;
	}
	IConsolePrint(CC_INFO, "Blueprint '{}': {}x{}, {} track tiles, {} signals, {} depots, {} bridges/tunnels, {} station tiles; {} partial bridges/tunnels left out; {} ms.",
		bp.name, bp.width, bp.height, bp.tracks.size(), bp.signals.size(), bp.depots.size(), bp.tunnelbridges.size(), bp.stations.size(), stats.partial_tunnelbridges, ms);
	return true;
}

/** Paste a blueprint. @copydoc IConsoleCmdProc */
static bool ConBlueprintPaste(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Paste a blueprint with its north corner at x,y. Usage: 'blueprint_paste <name> <x> <y> [<rotation 0-3> [<mirror 0|1> [<company> [<rail type label>]]]]'.");
		IConsolePrint(CC_HELP, "Rotation is in quarter turns clockwise, the mirror flips north-east and south-west before rotating. Company: 1-based; defaults to your company.");
		return true;
	}
	if (argv.size() < 4 || argv.size() > 8) return false;

	std::optional<Blueprint> bp = LoadBlueprint(argv[1]);
	if (!bp.has_value()) {
		IConsolePrint(CC_ERROR, "Blueprint '{}' not found or broken.", argv[1]);
		return true;
	}
	auto x = ParseBlueprintCoordinate(argv[2], Map::SizeX());
	auto y = ParseBlueprintCoordinate(argv[3], Map::SizeY());
	if (!x.has_value() || !y.has_value()) {
		IConsolePrint(CC_ERROR, "Invalid coordinates.");
		return true;
	}

	BlueprintPasteOptions options;
	if (argv.size() > 4) {
		auto rotation = ParseType<uint>(argv[4]);
		if (!rotation.has_value() || *rotation > 3) return false;
		options.transform.rotation = static_cast<uint8_t>(*rotation);
	}
	if (argv.size() > 5) {
		auto mirror = ParseType<uint>(argv[5]);
		if (!mirror.has_value() || *mirror > 1) return false;
		options.transform.mirror = *mirror == 1;
	}
	CompanyID company = _local_company;
	if (argv.size() > 6) {
		auto c = ParseType<uint>(argv[6]);
		if (!c.has_value() || *c == 0 || !Company::IsValidID(static_cast<CompanyID>(*c - 1))) {
			IConsolePrint(CC_ERROR, "Unknown company '{}'.", argv[6]);
			return true;
		}
		company = static_cast<CompanyID>(*c - 1);
		if (!_network_server && _networking && company != _local_company) {
			IConsolePrint(CC_ERROR, "Only the server can paste for another company.");
			return true;
		}
	}
	if (argv.size() > 7) {
		if (argv[7].size() != 4) return false;
		RailTypeLabel label{};
		for (size_t i = 0; i < 4; i++) label[i] = static_cast<uint8_t>(argv[7][i]);
		options.railtype = GetRailTypeByLabel(label);
		if (options.railtype == INVALID_RAILTYPE) {
			IConsolePrint(CC_ERROR, "Unknown rail type '{}'.", argv[7]);
			return true;
		}
	}
	if (!Company::IsValidID(company)) {
		IConsolePrint(CC_ERROR, "No company to paste for; give one.");
		return true;
	}

	BlueprintPastePlan plan = PlanBlueprintPaste(*bp, TileXY(*x, *y), options);
	if (plan.chunks.empty()) {
		IConsolePrint(CC_ERROR, "Cannot paste '{}': {}.", bp->name, plan.error);
		return true;
	}
	size_t chunks = plan.chunks.size();
	size_t items = plan.items;
	if (!StartBlueprintPaste(bp->name, std::move(plan), company)) {
		IConsolePrint(CC_ERROR, "Another blueprint is being pasted.");
		return true;
	}
	IConsolePrint(CC_INFO, "Pasting '{}': {} items in {} commands.", bp->name, items, chunks);
	return true;
}

/** List the saved blueprints. @copydoc IConsoleCmdProc */
static bool ConBlueprintList(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "List the saved blueprints. Usage: 'blueprint_list'.");
		return true;
	}
	std::vector<std::string> names = ListBlueprints();
	IConsolePrint(CC_INFO, "{} blueprints in {}", names.size(), GetBlueprintDirectory());
	for (const std::string &name : names) IConsolePrint(CC_INFO, "  {}", name);
	return true;
}

/** Show the contents of a blueprint. @copydoc IConsoleCmdProc */
static bool ConBlueprintInfo(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Show what a blueprint contains. Usage: 'blueprint_info <name>'.");
		return true;
	}
	if (argv.size() != 2) return false;
	std::optional<Blueprint> bp = LoadBlueprint(argv[1]);
	if (!bp.has_value()) {
		IConsolePrint(CC_ERROR, "Blueprint '{}' not found or broken.", argv[1]);
		return true;
	}
	std::string railtypes;
	for (const RailTypeLabel &label : bp->railtypes) format_append(railtypes, " {}", std::string_view{reinterpret_cast<const char *>(label.data()), label.size()});
	IConsolePrint(CC_INFO, "Blueprint '{}': {}x{}; rail types:{}; {} track tiles, {} signals, {} depots, {} bridges/tunnels, {} station tiles, {} NewGRF station specs.",
		bp->name, bp->width, bp->height, railtypes, bp->tracks.size(), bp->signals.size(), bp->depots.size(), bp->tunnelbridges.size(), bp->stations.size(), bp->specs.size());
	return true;
}

/** Delete a blueprint. @copydoc IConsoleCmdProc */
static bool ConBlueprintDelete(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Delete a blueprint. Usage: 'blueprint_delete <name>'.");
		return true;
	}
	if (argv.size() != 2) return false;
	if (DeleteBlueprint(argv[1])) {
		IConsolePrint(CC_INFO, "Blueprint '{}' deleted.", argv[1]);
	} else {
		IConsolePrint(CC_ERROR, "Blueprint '{}' not found.", argv[1]);
	}
	return true;
}

/** Compare two blueprints, the first one transformed. @copydoc IConsoleCmdProc */
static bool ConBlueprintCompare(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Compare blueprint a, rotated and mirrored, with blueprint b. Usage: 'blueprint_compare <a> <b> [<rotation 0-3> [<mirror 0|1>]]'.");
		return true;
	}
	if (argv.size() < 3 || argv.size() > 5) return false;
	std::optional<Blueprint> a = LoadBlueprint(argv[1]);
	std::optional<Blueprint> b = LoadBlueprint(argv[2]);
	if (!a.has_value() || !b.has_value()) {
		IConsolePrint(CC_ERROR, "Blueprint not found or broken.");
		return true;
	}
	BlueprintTransform transform;
	if (argv.size() > 3) {
		auto rotation = ParseType<uint>(argv[3]);
		if (!rotation.has_value() || *rotation > 3) return false;
		transform.rotation = static_cast<uint8_t>(*rotation);
	}
	if (argv.size() > 4) {
		auto mirror = ParseType<uint>(argv[4]);
		if (!mirror.has_value() || *mirror > 1) return false;
		transform.mirror = *mirror == 1;
	}
	std::string diff = a->Transformed(transform).Compare(*b);
	if (diff.empty()) {
		IConsolePrint(CC_INFO, "Blueprints equal ({} items).", a->CountItems());
	} else {
		IConsolePrint(CC_WARNING, "Blueprints differ:");
		std::string_view rest = diff;
		while (!rest.empty()) {
			size_t eol = rest.find('\n');
			IConsolePrint(CC_WARNING, "  {}", rest.substr(0, eol));
			if (eol == std::string_view::npos) break;
			rest.remove_prefix(eol + 1);
		}
	}
	return true;
}

/** Show or abort the current paste. @copydoc IConsoleCmdProc */
static bool ConBlueprintStatus(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Show the progress of the current blueprint paste, or abort it. Usage: 'blueprint_status [abort]'.");
		return true;
	}
	if (argv.size() == 2 && argv[1] == "abort") AbortBlueprintPaste();
	const BlueprintPasteStatus &s = GetBlueprintPasteStatus();
	IConsolePrint(CC_INFO, "Blueprint paste '{}': {}, commands {}/{}/{} (done/sent/total), {} built, {} failed, cost {}{}{}.",
		s.name, s.active ? "running" : "finished", s.chunks_done, s.chunks_sent, s.chunks_total, s.built, s.failed, static_cast<int64_t>(s.cost),
		s.out_of_money ? ", out of money" : "", s.aborted ? ", aborted" : "");
	return true;
}

/** List all road types and their configuration. */
static void ConDumpRoadTypes()
{
	IConsolePrint(CC_DEFAULT, "  Flags:");
	IConsolePrint(CC_DEFAULT, "    c = catenary");
	IConsolePrint(CC_DEFAULT, "    l = no level crossings");
	IConsolePrint(CC_DEFAULT, "    X = no houses");
	IConsolePrint(CC_DEFAULT, "    h = hidden");
	IConsolePrint(CC_DEFAULT, "    T = buildable by towns");

	std::map<GrfID, const GRFFile *> grfs;
	for (RoadType rt : EnumRange(ROADTYPE_END)) {
		const RoadTypeInfo *rti = GetRoadTypeInfo(rt);
		if (rti->label.Empty()) continue;
		GrfID grfid{};
		const GRFFile *grf = rti->grffile[RoadSpriteType::Ground];
		if (grf != nullptr) {
			grfid = grf->grfid;
			grfs.emplace(grfid, grf);
		}
		IConsolePrint(CC_DEFAULT, "  {:02d} {} {}, Flags: {}{}{}{}{}, GRF: {}, {}",
				(uint)rt,
				RoadTypeIsTram(rt) ? "Tram" : "Road",
				rti->label.AsString(),
				rti->flags.Test(RoadTypeFlag::Catenary)        ? 'c' : '-',
				rti->flags.Test(RoadTypeFlag::NoLevelCrossing) ? 'l' : '-',
				rti->flags.Test(RoadTypeFlag::NoHouses)        ? 'X' : '-',
				rti->flags.Test(RoadTypeFlag::Hidden)          ? 'h' : '-',
				rti->flags.Test(RoadTypeFlag::TownBuild)       ? 'T' : '-',
				FormatArrayAsHex(grfid),
				GetStringPtr(rti->strings.name)
		);
	}
	for (const auto &grf : grfs) {
		IConsolePrint(CC_DEFAULT, "  GRF: {} = {}", FormatArrayAsHex(grf.first), grf.second->filename);
	}
}

/** List all rail types and their configuration. */
static void ConDumpRailTypes()
{
	IConsolePrint(CC_DEFAULT, "  Flags:");
	IConsolePrint(CC_DEFAULT, "    c = catenary");
	IConsolePrint(CC_DEFAULT, "    l = no level crossings");
	IConsolePrint(CC_DEFAULT, "    h = hidden");
	IConsolePrint(CC_DEFAULT, "    s = no sprite combine");
	IConsolePrint(CC_DEFAULT, "    a = always allow 90 degree turns");
	IConsolePrint(CC_DEFAULT, "    d = always disallow 90 degree turns");

	std::map<GrfID, const GRFFile *> grfs;
	for (RailType rt : EnumRange(RAILTYPE_END)) {
		const RailTypeInfo *rti = GetRailTypeInfo(rt);
		if (rti->label.Empty()) continue;
		GrfID grfid{};
		const GRFFile *grf = rti->grffile[RailSpriteType::Ground];
		if (grf != nullptr) {
			grfid = grf->grfid;
			grfs.emplace(grfid, grf);
		}
		IConsolePrint(CC_DEFAULT, "  {:02d} {}, Flags: {}{}{}{}{}{}, GRF: {}, {}",
				(uint)rt,
				rti->label.AsString(),
				rti->flags.Test(RailTypeFlag::Catenary)        ? 'c' : '-',
				rti->flags.Test(RailTypeFlag::NoLevelCrossing) ? 'l' : '-',
				rti->flags.Test(RailTypeFlag::Hidden)          ? 'h' : '-',
				rti->flags.Test(RailTypeFlag::NoSpriteCombine) ? 's' : '-',
				rti->flags.Test(RailTypeFlag::Allow90Deg)      ? 'a' : '-',
				rti->flags.Test(RailTypeFlag::Disallow90Deg)   ? 'd' : '-',
				FormatArrayAsHex(grfid),
				GetStringPtr(rti->strings.name)
		);
	}
	for (const auto &grf : grfs) {
		IConsolePrint(CC_DEFAULT, "  GRF: {} = {}", FormatArrayAsHex(grf.first), grf.second->filename);
	}
}

/** List all cargo types and their configuration. */
static void ConDumpCargoTypes()
{
	IConsolePrint(CC_DEFAULT, "  Cargo classes:");
	IConsolePrint(CC_DEFAULT, "    p = passenger");
	IConsolePrint(CC_DEFAULT, "    m = mail");
	IConsolePrint(CC_DEFAULT, "    x = express");
	IConsolePrint(CC_DEFAULT, "    a = armoured");
	IConsolePrint(CC_DEFAULT, "    b = bulk");
	IConsolePrint(CC_DEFAULT, "    g = piece goods");
	IConsolePrint(CC_DEFAULT, "    l = liquid");
	IConsolePrint(CC_DEFAULT, "    r = refrigerated");
	IConsolePrint(CC_DEFAULT, "    h = hazardous");
	IConsolePrint(CC_DEFAULT, "    c = covered/sheltered");
	IConsolePrint(CC_DEFAULT, "    o = oversized");
	IConsolePrint(CC_DEFAULT, "    d = powderized");
	IConsolePrint(CC_DEFAULT, "    n = not pourable");
	IConsolePrint(CC_DEFAULT, "    e = potable");
	IConsolePrint(CC_DEFAULT, "    i = non-potable");
	IConsolePrint(CC_DEFAULT, "    S = special");

	std::map<GrfID, const GRFFile *> grfs;
	for (const CargoSpec *spec : CargoSpec::Iterate()) {
		GrfID grfid{};
		const GRFFile *grf = spec->grffile;
		if (grf != nullptr) {
			grfid = grf->grfid;
			grfs.emplace(grfid, grf);
		}
		IConsolePrint(CC_DEFAULT, "  {:02d} Bit: {:2d}, Label: {}, Callback mask: 0x{:02X}, Cargo class: {}{}{}{}{}{}{}{}{}{}{}{}{}{}{}{}, GRF: {}, {}",
				spec->Index(),
				spec->bitnum,
				spec->label.AsString(),
				spec->callback_mask.base(),
				spec->classes.Test(CargoClass::Passengers)   ? 'p' : '-',
				spec->classes.Test(CargoClass::Mail)         ? 'm' : '-',
				spec->classes.Test(CargoClass::Express)      ? 'x' : '-',
				spec->classes.Test(CargoClass::Armoured)     ? 'a' : '-',
				spec->classes.Test(CargoClass::Bulk)         ? 'b' : '-',
				spec->classes.Test(CargoClass::PieceGoods)   ? 'g' : '-',
				spec->classes.Test(CargoClass::Liquid)       ? 'l' : '-',
				spec->classes.Test(CargoClass::Refrigerated) ? 'r' : '-',
				spec->classes.Test(CargoClass::Hazardous)    ? 'h' : '-',
				spec->classes.Test(CargoClass::Covered)      ? 'c' : '-',
				spec->classes.Test(CargoClass::Oversized)    ? 'o' : '-',
				spec->classes.Test(CargoClass::Powderized)   ? 'd' : '-',
				spec->classes.Test(CargoClass::NotPourable)  ? 'n' : '-',
				spec->classes.Test(CargoClass::Potable)      ? 'e' : '-',
				spec->classes.Test(CargoClass::NonPotable)   ? 'i' : '-',
				spec->classes.Test(CargoClass::Special)      ? 'S' : '-',
				FormatArrayAsHex(grfid),
				GetStringPtr(spec->name)
		);
	}
	for (const auto &grf : grfs) {
		IConsolePrint(CC_DEFAULT, "  GRF: {} = {}", FormatArrayAsHex(grf.first), grf.second->filename);
	}
}

/** Dump information about some NewGRF types. @copydoc IConsoleCmdProc */
static bool ConDumpInfo(std::span<std::string_view> argv)
{
	if (argv.size() != 2) {
		IConsolePrint(CC_HELP, "Dump debugging information.");
		IConsolePrint(CC_HELP, "Usage: 'dump_info roadtypes|railtypes|cargotypes'.");
		IConsolePrint(CC_HELP, "  Show information about road/tram types, rail types or cargo types.");
		return true;
	}

	if (StrEqualsIgnoreCase(argv[1], "roadtypes")) {
		ConDumpRoadTypes();
		return true;
	}

	if (StrEqualsIgnoreCase(argv[1], "railtypes")) {
		ConDumpRailTypes();
		return true;
	}

	if (StrEqualsIgnoreCase(argv[1], "cargotypes")) {
		ConDumpCargoTypes();
		return true;
	}

	return false;
}

/** Console command registration. */
void IConsoleStdLibRegister()
{
	IConsole::CmdRegister("debug_level",             ConDebugLevel);
	IConsole::CmdRegister("echo",                    ConEcho);
	IConsole::CmdRegister("echoc",                   ConEchoC);
	IConsole::CmdRegister("exec",                    ConExec);
	IConsole::CmdRegister("schedule",                ConSchedule);
	IConsole::CmdRegister("exit",                    ConExit);
	IConsole::CmdRegister("part",                    ConPart);
	IConsole::CmdRegister("help",                    ConHelp);
	IConsole::CmdRegister("info_cmd",                ConInfoCmd);
	IConsole::CmdRegister("list_cmds",               ConListCommands);
	IConsole::CmdRegister("list_aliases",            ConListAliases);
	IConsole::CmdRegister("newgame",                 ConNewGame);
	IConsole::CmdRegister("restart",                 ConRestart);
	IConsole::CmdRegister("reload",                  ConReload);
	IConsole::CmdRegister("getseed",                 ConGetSeed);
	IConsole::CmdRegister("getdate",                 ConGetDate);
	IConsole::CmdRegister("getsysdate",              ConGetSysDate);
	IConsole::CmdRegister("quit",                    ConExit);
	IConsole::CmdRegister("resetengines",            ConResetEngines,     ConHookNoNetwork);
	IConsole::CmdRegister("reset_enginepool",        ConResetEnginePool,  ConHookNoNetwork);
	IConsole::CmdRegister("return",                  ConReturn);
	IConsole::CmdRegister("screenshot",              ConScreenShot);
	IConsole::CmdRegister("script",                  ConScript);
	IConsole::CmdRegister("zoomto",                  ConZoomToLevel);
	IConsole::CmdRegister("scrollto",                ConScrollToTile);
	IConsole::CmdRegister("alias",                   ConAlias);
	IConsole::CmdRegister("load",                    ConLoad);
	IConsole::CmdRegister("load_save",               ConLoad);
	IConsole::CmdRegister("load_scenario",           ConLoadScenario);
	IConsole::CmdRegister("load_heightmap",          ConLoadHeightmap);
	IConsole::CmdRegister("rm",                      ConRemove);
	IConsole::CmdRegister("save",                    ConSave);
	IConsole::CmdRegister("saveconfig",              ConSaveConfig);
	IConsole::CmdRegister("ls",                      ConListFiles);
	IConsole::CmdRegister("list_saves",              ConListFiles);
	IConsole::CmdRegister("list_scenarios",          ConListScenarios);
	IConsole::CmdRegister("list_heightmaps",         ConListHeightmaps);
	IConsole::CmdRegister("cd",                      ConChangeDirectory);
	IConsole::CmdRegister("pwd",                     ConPrintWorkingDirectory);
	IConsole::CmdRegister("clear",                   ConClearBuffer);
	IConsole::CmdRegister("font",                    ConFont);
	IConsole::CmdRegister("setting",                 ConSetting);
	IConsole::CmdRegister("setting_newgame",         ConSettingNewgame);
	IConsole::CmdRegister("list_settings",           ConListSettings);
	IConsole::CmdRegister("gamelog",                 ConGamelogPrint);
	IConsole::CmdRegister("rescan_newgrf",           ConRescanNewGRF);
	IConsole::CmdRegister("list_dirs",               ConListDirs);

	IConsole::AliasRegister("dir",                   "ls");
	IConsole::AliasRegister("del",                   "rm %+");
	IConsole::AliasRegister("newmap",                "newgame");
	IConsole::AliasRegister("patch",                 "setting %+");
	IConsole::AliasRegister("set",                   "setting %+");
	IConsole::AliasRegister("set_newgame",           "setting_newgame %+");
	IConsole::AliasRegister("list_patches",          "list_settings %+");
	IConsole::AliasRegister("developer",             "setting developer %+");

	IConsole::CmdRegister("list_ai_libs",            ConListAILibs);
	IConsole::CmdRegister("list_ai",                 ConListAI);
	IConsole::CmdRegister("reload_ai",               ConReloadAI);
	IConsole::CmdRegister("rescan_ai",               ConRescanAI);
	IConsole::CmdRegister("start_ai",                ConStartAI);
	IConsole::CmdRegister("stop_ai",                 ConStopAI);

	IConsole::CmdRegister("list_game",               ConListGame);
	IConsole::CmdRegister("list_game_libs",          ConListGameLibs);
	IConsole::CmdRegister("rescan_game",             ConRescanGame);

	IConsole::CmdRegister("companies",               ConCompanies);
	IConsole::AliasRegister("players",               "companies");

	IConsole::CmdRegister("town_needs",              ConTownNeeds);
	IConsole::CmdRegister("town_force_need",         ConTownForceNeed,    ConHookServerOrNoNetwork);
	IConsole::CmdRegister("power_grid",              ConPowerGrid);
	IConsole::CmdRegister("power_add_fuel",          ConPowerAddFuel,     ConHookServerOrNoNetwork);
	IConsole::CmdRegister("townsim_log",             ConTownSimLog);

	/* networking functions */

/* Content downloading is only available with ZLIB */
#if defined(WITH_ZLIB)
	IConsole::CmdRegister("content",                 ConContent);
#endif /* defined(WITH_ZLIB) */

	/*** Networking commands ***/
	IConsole::CmdRegister("say",                     ConSay,              ConHookNeedNetwork);
	IConsole::CmdRegister("say_company",             ConSayCompany,       ConHookNeedNetwork);
	IConsole::AliasRegister("say_player",            "say_company %+");
	IConsole::CmdRegister("say_client",              ConSayClient,        ConHookNeedNetwork);

	IConsole::CmdRegister("connect",                 ConNetworkConnect,   ConHookClientOnly);
	IConsole::CmdRegister("clients",                 ConNetworkClients,   ConHookNeedNetwork);
	IConsole::CmdRegister("status",                  ConStatus,           ConHookServerOnly);
	IConsole::CmdRegister("server_info",             ConServerInfo,       ConHookServerOnly);
	IConsole::AliasRegister("info",                  "server_info");
	IConsole::CmdRegister("reconnect",               ConNetworkReconnect, ConHookClientOnly);
	IConsole::CmdRegister("rcon",                    ConRcon,             ConHookNeedNetwork);

	IConsole::CmdRegister("join",                    ConJoinCompany,      ConHookNeedNonDedicatedNetwork);
	IConsole::AliasRegister("spectate",              "join 255");
	IConsole::CmdRegister("move",                    ConMoveClient,       ConHookServerOnly);
	IConsole::CmdRegister("reset_company",           ConResetCompany,     ConHookServerOnly);
	IConsole::AliasRegister("clean_company",         "reset_company %A");
	IConsole::CmdRegister("client_name",             ConClientNickChange, ConHookServerOnly);
	IConsole::CmdRegister("kick",                    ConKick,             ConHookServerOnly);
	IConsole::CmdRegister("ban",                     ConBan,              ConHookServerOnly);
	IConsole::CmdRegister("unban",                   ConUnBan,            ConHookServerOnly);
	IConsole::CmdRegister("banlist",                 ConBanList,          ConHookServerOnly);

	IConsole::CmdRegister("pause",                   ConPauseGame,        ConHookServerOrNoNetwork);
	IConsole::CmdRegister("unpause",                 ConUnpauseGame,      ConHookServerOrNoNetwork);

	IConsole::CmdRegister("authorized_key", ConNetworkAuthorizedKey, ConHookServerOnly);
	IConsole::AliasRegister("ak", "authorized_key %+");

	IConsole::AliasRegister("net_frame_freq",        "setting frame_freq %+");
	IConsole::AliasRegister("net_sync_freq",         "setting sync_freq %+");
	IConsole::AliasRegister("server_pw",             "setting server_password %+");
	IConsole::AliasRegister("server_password",       "setting server_password %+");
	IConsole::AliasRegister("rcon_pw",               "setting rcon_password %+");
	IConsole::AliasRegister("rcon_password",         "setting rcon_password %+");
	IConsole::AliasRegister("name",                  "setting client_name %+");
	IConsole::AliasRegister("server_name",           "setting server_name %+");
	IConsole::AliasRegister("server_port",           "setting server_port %+");
	IConsole::AliasRegister("max_clients",           "setting max_clients %+");
	IConsole::AliasRegister("max_companies",         "setting max_companies %+");
	IConsole::AliasRegister("max_join_time",         "setting max_join_time %+");
	IConsole::AliasRegister("pause_on_join",         "setting pause_on_join %+");
	IConsole::AliasRegister("autoclean_companies",   "setting autoclean_companies %+");
	IConsole::AliasRegister("autoclean_protected",   "setting autoclean_protected %+");
	IConsole::AliasRegister("restart_game_year",     "setting restart_game_year %+");
	IConsole::AliasRegister("min_players",           "setting min_active_clients %+");
	IConsole::AliasRegister("reload_cfg",            "setting reload_cfg %+");

	/* debugging stuff */
#ifdef _DEBUG
	IConsoleDebugLibRegister();
#endif
	IConsole::CmdRegister("fps",                     ConFramerate);
	IConsole::CmdRegister("fps_wnd",                 ConFramerateWindow);

	/* Supply-driven town growth: GUI */
	IConsole::CmdRegister("town_supply_window",      ConTownSupplyWindow);
	IConsole::CmdRegister("blueprint_copy",          ConBlueprintCopy);
	IConsole::CmdRegister("blueprint_paste",         ConBlueprintPaste);
	IConsole::CmdRegister("blueprint_list",          ConBlueprintList);
	IConsole::CmdRegister("blueprint_info",          ConBlueprintInfo);
	IConsole::CmdRegister("blueprint_delete",        ConBlueprintDelete);
	IConsole::CmdRegister("blueprint_compare",       ConBlueprintCompare);
	IConsole::CmdRegister("blueprint_status",        ConBlueprintStatus);

	/* NewGRF development stuff */
	IConsole::CmdRegister("reload_newgrfs",          ConNewGRFReload,     ConHookNewGRFDeveloperTool);
	IConsole::CmdRegister("newgrf_profile",          ConNewGRFProfile,    ConHookNewGRFDeveloperTool);

	IConsole::CmdRegister("dump_info",               ConDumpInfo);
}
