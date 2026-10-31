/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file blueprint_cmd.h Command definitions for pasting rail blueprints. */

#ifndef BLUEPRINT_CMD_H
#define BLUEPRINT_CMD_H

#include "blueprint.h"
#include "command_type.h"
#include "misc/endian_buffer.hpp"

std::tuple<CommandCost, Money, uint32_t, uint32_t, StringID> CmdPasteBlueprint(DoCommandFlags flags, TileIndex origin, const BlueprintChunkData &chunk);

/* Test and execution differ: items of a chunk depend on items built before them (signals on track, stations joining). */
DEF_CMD_TRAIT(Commands::PasteBlueprint, CmdPasteBlueprint, CommandFlags({CommandFlag::Auto, CommandFlag::NoWater, CommandFlag::NoTest, CommandFlag::NoEst}), CommandType::LandscapeConstruction)

void CcPasteBlueprint(Commands cmd, const CommandCost &result, Money extra_money, uint32_t built, uint32_t failed, StringID last_error);

template <typename Tcont, typename Titer>
inline EndianBufferWriter<Tcont, Titer> &operator <<(EndianBufferWriter<Tcont, Titer> &buffer, const BlueprintChunkData &chunk)
{
	size_t size = std::min(chunk.data.size(), BlueprintChunkData::MAX_SIZE);
	buffer << static_cast<uint16_t>(size);
	for (size_t i = 0; i < size; i++) buffer << chunk.data[i];
	return buffer;
}

inline EndianBufferReader &operator >>(EndianBufferReader &buffer, BlueprintChunkData &chunk)
{
	uint16_t size = 0;
	buffer >> size;
	chunk.data.clear();
	for (uint16_t i = 0; i < std::min<size_t>(size, BlueprintChunkData::MAX_SIZE); i++) {
		uint8_t b = 0;
		buffer >> b;
		chunk.data.push_back(b);
	}
	return buffer;
}

#endif /* BLUEPRINT_CMD_H */
