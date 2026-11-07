/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file agent_capture.h Capture of the live game frame and camera queries for the in-game agent bridge. */

#ifndef AGENT_CAPTURE_H
#define AGENT_CAPTURE_H

#include "3rdparty/nlohmann/json.hpp"
#include "landscape.h"
#include "viewport_type.h"
#include "zoom_func.h"

/** Bridge between the running game and an external agent. */
namespace InGameAgent {

using Json = nlohmann::json;

/**
 * Capture the frame that the game is currently showing and return it as a PNG.
 *
 * The pixels are produced by the game's own renderers and encoded with the regular PNG
 * screenshot provider; nothing is synthesised. The screen modes copy the live screen buffer,
 * and before that copy the dirty blocks are redrawn, exactly like a viewport screenshot does,
 * so a capture requested after a command shows the state that command produced instead of the
 * frame from before it. The image is written to a private temporary directory, read back, and
 * the directory is removed again, so no user file is ever touched.
 *
 * Three kinds of image are supported:
 * - "viewport" without a "target" (the default) is the whole screen frame, every window
 *   included, copied from the live screen buffer. "region" is a rectangle of that frame in
 *   screen pixels. To find out where a tile is on that screen, use InspectGameViewport()
 *   (or WorldToScreen()) first; the capture itself does not move the camera.
 * - "viewport" with a "target" renders an independent camera view of the map at the
 *   requested tile, size and zoom. It is rendered from its own Viewport, so the player's
 *   viewport is neither moved nor read, and no UI is in the image.
 * - "minimap" renders the smallmap "owner" colouring of the whole world, one pixel per tile
 *   (or nearest tile when a smaller image is asked for), exactly what the built-in minimap
 *   screenshot draws.
 *
 * Must be called from the main thread: the thread that runs the video driver's draw
 * tick and owns the video buffer (the one that executes VideoDriver::QueueOnMainThread
 * callbacks), while that buffer is locked. A caller on the game thread has to wrap the
 * call in VideoDriver::GetInstance()->QueueOnMainThread(...). The project exposes no
 * public game-thread identity, so this is a caller contract and not a runtime check.
 *
 * Parameters (all optional; a field that does not apply to the requested kind, or that has
 * the wrong type, is "invalid_params" and is never silently ignored):
 * - "kind":        "viewport" (default), "region" or "minimap". "region" is a part of the screen
 *                  frame; "minimap" is the smallmap render described above.
 * - "region":      object with "left", "top", "width", "height" in screen pixels; required
 *                  for "region" and clamped to the frame that actually exists.
 * - "target":      object asking for an independent camera render instead of the screen frame;
 *                  exactly one of "tile" (tile index), "tile_x"/"tile_y" (tile coordinates),
 *                  "town" (town id) or "center" (object with "x"/"y" tile coordinates). Only
 *                  valid with "kind": "viewport". The tile centre at ground height is put in the
 *                  middle of the image. With a target, the capture reads and writes nothing of the
 *                  player's viewport, so the camera stays exactly where the player left it.
 * - "width"/"height": image size in pixels, both or neither, at most 8192 each. For "target" they
 *                  default to the screen size; for "minimap" they default to one pixel per tile.
 *                  A smaller minimap is sampled per nearest tile. Only valid with "target" or
 *                  "minimap".
 * - "zoom":        zoom level of a "target" render (0 = 4x in ... 6 = 8x out, default 2 = normal,
 *                  the same numbers InspectGameViewport() reports). Only valid with "target".
 * - "max_pixels":  upper bound on the number of captured pixels (default 16M, hard cap 64M).
 * - "max_bytes":   upper bound on the size of the encoded PNG (default 5MiB, clamped to the
 *                  largest PNG that still fits the backend's 8MiB request body after base64
 *                  and JSON overhead, about 5.9MiB).
 * - "include_png": whether to include the base64 PNG in the result (default true).
 * - "hide_agent_ui": move the in-game agent windows (WindowClass::AgentChat and
 *                  WindowClass::AgentChatStatus) off screen for the copy, so the pixels show the
 *                  map that the chat was covering. Only those two windows are touched, only their
 *                  position, and they are put back exactly where they were before the call
 *                  returns - also when the capture fails. Default false, which keeps the whole
 *                  screen frame (chat included) as before. A rendered image ("target" or
 *                  "minimap") contains no UI, so there it hides nothing and reports that.
 *
 * On success the result contains:
 * - "ok": true, "kind", "mode" ("screen_frame", "screen_frame_region", "independent_viewport"
 *   or "minimap"), "width", "height", "tick" (the game tick the pixels were rendered from),
 *   "frame" (redrawn/tick), "hash" (MD5 of the PNG bytes), "hash_algorithm", "parent_hash" (hash
 *   of the previous capture in this process, or null), "png_bytes" and, unless disabled,
 *   "png_base64". The backend derives its own SHA-256 from the decoded bytes.
 * - "camera" (left/top/width/height of the copied screen rectangle) for the screen modes.
 * - "coordinate_space", "projection" and, for "minimap", "map" and "legend" for the rendered
 *   modes: "projection.viewport" holds the Viewport of an independent render, so that
 *   WorldToScreen(viewport, x, y, z) and TranslateXYToTileCoord(viewport, x, y) map between the
 *   image and the map (the player's viewport is a different camera; InspectGameViewport() reports
 *   that one). "projection.player_viewport_unchanged" is true. For "minimap", "projection"
 *   describes the pixel/tile mapping (x is mirrored like the built-in minimap screenshot) and
 *   "legend" gives the palette indices of the fixed colour meanings.
 * - "ui_in_frame" (whether the image can contain UI at all), "hide_agent_ui" (true only when
 *   agent windows were really hidden for this copy), "hidden_window_count" (how many) and
 *   "agent_ui_restored" (false only when a hidden window was gone before it could be put back).
 *
 * On failure the result contains "ok": false, "error" and "tick". Possible errors are
 * "headless_unavailable" (no video driver, blitter or frame buffer), "invalid_params",
 * "too_large" (with "png_bytes"/"max_bytes" or "pixels"/"max_pixels"), "stale_frame" (the
 * game advanced while the image was made, so the pixels are not the state of the reported
 * tick), "map_unavailable" (a "minimap" without a map, e.g. in the menu), "out_of_bounds"
 * (a target that is not on the map), "png_unavailable", "temp_dir_unavailable" and
 * "encode_failed".
 *
 * @param params The request parameters.
 * @return The JSON result describing the capture.
 */
Json CaptureGameFrame(const Json &params);

/**
 * Read the optional "hide_agent_ui" parameter of a capture.
 *
 * The field is optional and defaults to false. A present but non-boolean value is a caller error
 * and is reported instead of being silently ignored (a string "false" is not false).
 *
 * @param params The capture parameters.
 * @param hide   Receives the requested value; set to false when the field is absent.
 * @return True iff the field is absent or a boolean.
 */
inline bool ReadHideAgentUi(const Json &params, bool &hide)
{
	const auto field = params.find("hide_agent_ui");
	if (field == params.end()) {
		hide = false;
		return true;
	}
	if (!field->is_boolean()) return false;
	hide = field->get<bool>();
	return true;
}

/**
 * Screen position of a world point in a viewport.
 *
 * This is the inverse of TranslateXYToTileCoord(): the point is remapped with RemapCoords()
 * and then scaled by the viewport zoom with the same rounding the viewport uses when it
 * draws (SetupViewportArea() in viewport.cpp), so the result is the screen pixel the point
 * is actually drawn at, not an approximation. The result is in screen pixels, i.e. the same
 * coordinate space as Viewport::left/top and the capture region.
 *
 * @param vp The viewport to project into.
 * @param x  X world coordinate.
 * @param y  Y world coordinate.
 * @param z  Z world coordinate.
 * @return The screen position of the point.
 */
inline Point WorldToScreen(const Viewport &vp, int x, int y, int z)
{
	const Point world = RemapCoords(x, y, z);
	const int mask = ScaleByZoom(-1, vp.zoom);
	return {UnScaleByZoom(world.x - (vp.virtual_left & mask), vp.zoom) + vp.left,
			UnScaleByZoom(world.y - (vp.virtual_top & mask), vp.zoom) + vp.top};
}

/**
 * Report the camera of the main viewport and, on request, project a tile or a screen pixel.
 *
 * Read-only: nothing is scrolled or zoomed. The camera is described with the real viewport
 * state (Viewport::left/top/width/height and virtual_left/top/width/height, the scroll
 * position and the zoom level), so a caller can tell exactly which part of the map the
 * screen shows. All screen coordinates are screen pixels of the current frame; the
 * "coordinate_space" of every object says which space its numbers are in.
 *
 * Parameters (all optional, at most one projection per call):
 * - "tile":        tile index to project to the screen.
 * - "tile_x"/"tile_y": tile coordinates to project to the screen (instead of "tile").
 * - "town":        town id; its centre tile is projected to the screen.
 * - "center":      object with tile coordinates "x"/"y" to project to the screen.
 * - "screen_x"/"screen_y": screen pixel to project to the map (both required).
 * - "clamp_to_map": clamp the screen-to-tile result to the map (default true).
 *
 * The result always contains "ok": true, "tick", "coordinate_space" ("screen_pixels"),
 * "game_mode", "in_game", "screen" (width/height of the frame), "viewport" (left, top,
 * width, height, virtual_left, virtual_top, virtual_width, virtual_height, zoom,
 * zoom_name) and "camera" (center_world, center_tile, scrollpos, dest_scrollpos, zoom,
 * zoom_name). "center_tile" is the tile coordinate of the viewport centre, or null when the
 * centre is outside the map; it is derived from the virtual centre, so use a projection for
 * the terrain-aware answer.
 *
 * A requested projection is returned as "projection":
 * - tile to screen: "coordinate_space": "screen_pixels", "tile" (index/x/y), "world"
 *   (x/y/z of the tile centre at ground height), "screen" (x/y), "in_viewport", "visible",
 *   "occluding_window" (null, or the class/number of the window covering that pixel) and
 *   "zoom".
 * - screen to tile: "coordinate_space": "world", "screen" (x/y), "world" (x/y as returned
 *   by TranslateXYToTileCoord(), or null when the pixel is outside the viewport), "tile"
 *   (tile coordinates of that world position, or null when it is outside the map),
 *   "tile_index" (the tile index, or null), "in_viewport", "visible", "occluding_window",
 *   "clamp_to_map" and "zoom".
 *
 * On failure the result contains "ok": false, "error" and "tick". Possible errors are
 * "viewport_unavailable" (no main window with a viewport), "invalid_params" and
 * "out_of_bounds" (the tile, town or tile coordinates are not on the map).
 *
 * Must be called from the main thread, like CaptureGameFrame().
 *
 * @param params The request parameters.
 * @return The JSON result describing the camera and the projection.
 */
Json InspectGameViewport(const Json &params);

/**
 * Move and zoom the main viewport through the normal native UI camera APIs.
 *
 * This is a UI operation, not a simulation-memory change: the camera is moved with
 * ScrollMainWindowToTile()/ScrollMainWindowTo() and zoomed with DoZoomInOutWindow(), the
 * same calls the mouse and the zoom buttons use, so the game state is untouched. It must
 * run on the main thread (the draw thread that executes VideoDriver::QueueOnMainThread
 * callbacks), like CaptureGameFrame().
 *
 * Parameters (at least one of "tile"/"tile_x"/"tile_y"/"town"/"center", "scroll" or
 * "zoom"/"zoom_delta" is required):
 * - "tile":        tile index to centre on.
 * - "tile_x"/"tile_y": tile coordinates to centre on (instead of "tile").
 * - "town":        town id; centres on the town centre tile.
 * - "center":      object with tile coordinates "x"/"y" to centre on.
 * - "scroll":      object with "dx"/"dy" in screen pixels to pan by, from the current centre.
 * - "zoom":        absolute zoom level (0 = 4x in, 2 = normal, 6 = 8x out), clamped to the
 *                  user's zoom limits.
 * - "zoom_delta":  relative zoom steps, clamped to the user's zoom limits.
 * - "instant":     jump instead of scrolling smoothly (default true).
 *
 * The result contains "ok": true, "tick", "changed" (whether the camera actually moved),
 * "instant", "zoom_clamped" (the requested zoom was outside the allowed range),
 * "coordinate_space" ("screen_pixels"), "screen", "viewport" and "camera" in the same shape
 * as InspectGameViewport(), describing the state after the change. With "instant": true the
 * new position is applied before the result is built, so the reported state is the state the
 * next frame is drawn from; with "instant": false the camera is still moving and
 * "dest_scrollpos" is the destination.
 *
 * On failure the result contains "ok": false, "error" and "tick". Possible errors are
 * "viewport_unavailable", "invalid_params" and "out_of_bounds".
 *
 * @param params The request parameters.
 * @return The JSON result describing the resulting camera.
 */
Json SetGameCamera(const Json &params);

} // namespace InGameAgent

#endif /* AGENT_CAPTURE_H */
