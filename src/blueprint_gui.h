/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file blueprint_gui.h GUI of the rail blueprints: the blueprint window and the paste preview in the viewport. */

#ifndef BLUEPRINT_GUI_H
#define BLUEPRINT_GUI_H

#include "window_type.h"

/** What the paste preview shows on one tile of the blueprint. */
enum class BlueprintPreviewFlag : uint8_t {
	/* Bits 0..5 hold the track bits of the tile. */
	Mark = 6, ///< A depot, a station or waypoint tile, or an end of a bridge or tunnel: draw a tile selection rectangle.
};

/** Mask of the track bits in a preview tile value. */
static constexpr uint8_t BLUEPRINT_PREVIEW_TRACK_MASK = 0x3F;

Window *ShowBlueprintWindow();

bool IsBlueprintPreviewActive();
uint8_t GetBlueprintPreviewTile(uint x, uint y);

#endif /* BLUEPRINT_GUI_H */
