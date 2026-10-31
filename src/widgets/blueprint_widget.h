/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file blueprint_widget.h Types related to the rail blueprint widgets. */

#ifndef WIDGETS_BLUEPRINT_WIDGET_H
#define WIDGETS_BLUEPRINT_WIDGET_H

/** Widgets of the #BlueprintWindow class. */
enum BlueprintWidgets : WidgetID {
	WID_BP_CAPTION, ///< Caption of the window.
	WID_BP_LIST, ///< List of the saved blueprints.
	WID_BP_SCROLLBAR, ///< Scrollbar of the list.
	WID_BP_COPY, ///< Start selecting an area to copy.
	WID_BP_PASTE, ///< Start placing the selected blueprint.
	WID_BP_RENAME, ///< Rename the selected blueprint.
	WID_BP_DELETE, ///< Delete the selected blueprint.
	WID_BP_ROTATE_LEFT, ///< Rotate a quarter turn counter-clockwise.
	WID_BP_ROTATE_RIGHT, ///< Rotate a quarter turn clockwise.
	WID_BP_MIRROR, ///< Mirror the blueprint.
	WID_BP_RAILTYPE, ///< Toggle: paste with the rail type selected in the rail toolbar.
	WID_BP_STATUS, ///< Transformation and paste progress.
	WID_BP_ABORT, ///< Abort the running paste.
};

#endif /* WIDGETS_BLUEPRINT_WIDGET_H */
