/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file agent_capture.cpp Capture of the live game frame for the in-game agent bridge. */

#include "stdafx.h"
#include "agent_capture.h"
#include "blitter/factory.hpp"
#include "core/backup_type.hpp"
#include "core/format.hpp"
#include "core/math_func.hpp"
#include "gfx_func.h"
#include "map_func.h"
#include "openttd.h"
#include "palette_func.h"
#include "screenshot_gui.h"
#include "screenshot_type.h"
#include "settings_type.h"
#include "smallmap_gui.h"
#include "string_func.h"
#include "tile_map.h"
#include "tile_type.h"
#include "timer/timer_game_tick.h"
#include "town.h"
#include "video/video_driver.hpp"
#include "viewport_func.h"
#include "window_func.h"
#include "window_gui.h"
#include "window_type.h"
#include "3rdparty/md5/md5.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <vector>

#include "safeguards.h"

namespace InGameAgent {

/** Part of the screen frame that is captured, in screen pixels. */
struct CaptureRect {
	int left;
	int top;
	int width;
	int height;
};

/** Default and hard limits of a single capture, so one request stays bounded. */
static constexpr uint64_t DEFAULT_MAX_PIXELS = 4096ull * 4096ull;
static constexpr uint64_t HARD_MAX_PIXELS = 8192ull * 8192ull;
static constexpr int MAX_CAPTURE_SIDE = 8192; ///< Longest side of a rendered image, so one buffer stays bounded.

/**
 * The backend rejects a request body above 8 MiB (`MAX_BODY` in agent/backend/server.py), and a
 * result it refuses is retried by the bridge forever. The PNG travels base64 encoded (4 bytes for
 * every 3) inside the JSON result, so the PNG itself has to stay well below that limit.
 */
static constexpr size_t MAX_RESULT_BODY_BYTES = 8ull * 1024ull * 1024ull;
static constexpr size_t RESULT_BODY_RESERVE = 128ull * 1024ull; ///< Room for the JSON envelope around the base64 payload.
static constexpr size_t MAX_PNG_BYTES = (MAX_RESULT_BODY_BYTES - RESULT_BODY_RESERVE) / 4 * 3; ///< Largest PNG that still fits the body limit.
static constexpr size_t DEFAULT_MAX_BYTES = 5ull * 1024ull * 1024ull; ///< Default cap; a 1280x800 frame is about 1 MiB.

static std::atomic<uint64_t> _capture_serial{0}; ///< Serial number, to give every capture its own directory.
static std::string _last_capture_hash; ///< Hash of the previous capture, reported as the parent hash.

/**
 * Private temporary directory of one capture, removed again however the scope is left.
 *
 * The directory is created with an atomic mkdir, so two captures can never pick the same name and
 * an existing directory is never reused or overwritten. The PNG lives inside it, so the file name
 * itself does not have to be unique.
 */
class CaptureTempDir {
public:
	CaptureTempDir() = default;
	~CaptureTempDir() { this->Remove(); }

	CaptureTempDir(const CaptureTempDir &) = delete;
	CaptureTempDir &operator=(const CaptureTempDir &) = delete;

	/**
	 * Create a new unique directory in the system temporary directory.
	 * @return True iff the directory was created.
	 */
	bool Create()
	{
		std::error_code ec;
		const std::filesystem::path base = std::filesystem::temp_directory_path(ec);
		if (ec) return false;

		for (int attempt = 0; attempt < 16; attempt++) {
			const std::filesystem::path candidate = base / fmt::format("openttd-agent-capture-{}-{}-{}",
					_capture_serial.fetch_add(1), std::chrono::steady_clock::now().time_since_epoch().count(), std::random_device{}());

			std::error_code create_ec;
			if (std::filesystem::create_directory(candidate, create_ec)) {
				this->path = candidate;
				/* Keep the pixels private to this account; best effort, the name is already unique. */
				std::filesystem::permissions(candidate, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace, ec);
				return true;
			}
			if (create_ec) return false; // not a name collision but a real error
		}
		return false;
	}

	/** The directory that was created. */
	const std::filesystem::path &Path() const { return this->path; }

private:
	void Remove()
	{
		if (this->path.empty()) return;
		std::error_code ec;
		std::filesystem::remove_all(this->path, ec);
		this->path.clear();
	}

	std::filesystem::path path;
};

/**
 * Encode binary data as base64.
 * @param data The data to encode.
 * @return The base64 representation of the data.
 */
static std::string Base64Encode(std::string_view data)
{
	static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

	std::string out;
	out.reserve(((data.size() + 2) / 3) * 4);

	size_t i = 0;
	for (; i + 2 < data.size(); i += 3) {
		const uint32_t v = (static_cast<uint8_t>(data[i]) << 16) | (static_cast<uint8_t>(data[i + 1]) << 8) | static_cast<uint8_t>(data[i + 2]);
		out.push_back(alphabet[(v >> 18) & 0x3F]);
		out.push_back(alphabet[(v >> 12) & 0x3F]);
		out.push_back(alphabet[(v >> 6) & 0x3F]);
		out.push_back(alphabet[v & 0x3F]);
	}

	if (i + 1 == data.size()) {
		const uint32_t v = static_cast<uint8_t>(data[i]) << 16;
		out.push_back(alphabet[(v >> 18) & 0x3F]);
		out.push_back(alphabet[(v >> 12) & 0x3F]);
		out.append("==");
	} else if (i + 2 == data.size()) {
		const uint32_t v = (static_cast<uint8_t>(data[i]) << 16) | (static_cast<uint8_t>(data[i + 1]) << 8);
		out.push_back(alphabet[(v >> 18) & 0x3F]);
		out.push_back(alphabet[(v >> 12) & 0x3F]);
		out.push_back(alphabet[(v >> 6) & 0x3F]);
		out.push_back('=');
	}

	return out;
}

/**
 * Copy a part of the current screen frame into the buffer of the screenshot provider.
 * @param rect  The part of the screen frame to copy.
 * @param buf   Destination buffer.
 * @param y     First line to copy.
 * @param pitch Number of pixels per line in the destination buffer.
 * @param n     Number of lines to copy.
 */
static void CaptureFrameCallback(const CaptureRect &rect, void *buf, uint y, uint pitch, uint n)
{
	Blitter *blitter = BlitterFactory::GetCurrentBlitter();
	void *src = blitter->MoveTo(_screen.dst_ptr, rect.left, rect.top + static_cast<int>(y));
	blitter->CopyImageToBuffer(src, buf, rect.width, n, pitch);
}

/**
 * Create the result of a failed capture.
 * @param error Machine readable reason of the failure.
 * @return The JSON result.
 */
static Json Failure(std::string_view error)
{
	Json result = Json::object();
	result["ok"] = false;
	result["error"] = error;
	result["tick"] = TimerGameTick::counter;
	return result;
}

/**
 * One agent window that was moved off screen, with the position it has to be returned to.
 */
struct HiddenAgentWindow {
	WindowClass window_class; ///< Class of the window.
	WindowNumber window_number; ///< Number of the window within its class.
	Window *window; ///< The window that was moved.
	int left; ///< Position before the move.
	int top; ///< Position before the move.
};

/** Nesting depth of agent UI hides; only the outermost scope moves the windows. */
static int _agent_ui_hide_depth = 0;
/** How many agent windows the outermost hide scope currently keeps off screen. */
static int _agent_ui_hidden_windows = 0;

/**
 * Temporarily move the in-game agent windows off screen, and put them back however the scope is
 * left.
 *
 * This is the idiom of SetScreenshotWindowVisibility(): the area a window covers is marked dirty
 * first, so the next DrawDirtyBlocks() redraws the world that was behind it, and the window is
 * then moved out of the frame. Only WindowClass::AgentChat and WindowClass::AgentChatStatus are
 * touched, and only their position: nothing is closed, resized or otherwise changed, so the chat
 * keeps its state, its scroll position and its focus. The screenshot window keeps its own, separate
 * handling in SetScreenshotWindowVisibility(), which this scope neither uses nor changes.
 *
 * The windows are put back as soon as the pixels exist and, because the restore also runs from
 * the destructor, an encoder failure or an exception cannot leave the chat off screen either. It
 * happens before the caller's video buffer lock is released. Nesting is safe: an inner scope
 * finds the windows already hidden, does not move them a second time and reports the windows that
 * were off screen for its own copy.
 */
class AgentUiHider {
public:
	/**
	 * Hide the agent windows, unless an outer scope already did.
	 * @param hide Whether the capture asked for the agent UI to be hidden.
	 */
	explicit AgentUiHider(bool hide)
	{
		if (!hide) return;

		/* Reserve before any state changes: with the capacity in place the loop below cannot
		 * reallocate, so nothing after this line throws and a scope that moved a window always
		 * runs its destructor and puts it back. */
		this->windows.reserve(2);

		this->counted = true;
		if (_agent_ui_hide_depth++ > 0) {
			/* An outer scope already moved the windows; they are off screen for this copy as well. */
			this->hidden_windows = _agent_ui_hidden_windows;
			return;
		}
		this->owns = true;

		for (const WindowClass window_class : {WindowClass::AgentChat, WindowClass::AgentChatStatus}) {
			Window *w = FindWindowById(window_class, 0);
			if (w == nullptr) continue;
			/* Record the position before moving, so a restore can never guess it. */
			this->windows.push_back({window_class, w->window_number, w, w->left, w->top});
			w->SetDirty(); // the world behind the window has to be redrawn
			w->left += 2 * _screen.width; // off screen, like SetScreenshotWindowVisibility()
		}
		this->hidden_windows = static_cast<int>(this->windows.size());
		_agent_ui_hidden_windows = this->hidden_windows;
	}

	~AgentUiHider()
	{
		if (this->counted) _agent_ui_hide_depth--;
		this->Restore();
	}

	AgentUiHider(const AgentUiHider &) = delete;
	AgentUiHider &operator=(const AgentUiHider &) = delete;

	/**
	 * Return every window this scope moved to where it was, and mark the restored area dirty.
	 *
	 * Called explicitly once the pixels exist, and again from the destructor, which is then a
	 * no-op: the restore happens exactly once, on every path out of the capture.
	 */
	void Restore()
	{
		if (!this->owns) return;
		this->owns = false;

		for (const HiddenAgentWindow &hidden : this->windows) {
			/* The window could have been closed meanwhile; only move the window that is still there. */
			Window *w = FindWindowById(hidden.window_class, hidden.window_number);
			if (w != hidden.window) {
				this->restored = false;
				continue;
			}
			w->left = hidden.left;
			w->top = hidden.top;
			w->SetDirty();
		}
		this->windows.clear();
		_agent_ui_hidden_windows = 0;
	}

	/**
	 * How many agent windows were off screen for this capture.
	 *
	 * That is the number this scope moved, or the number an outer scope kept hidden for it. The
	 * count is taken when the windows move and survives Restore(), so the caller can report what
	 * the pixels show after the windows are already back.
	 */
	int HiddenWindowCount() const { return this->hidden_windows; }

	/** Whether every window this scope moved was returned to its exact position. */
	bool Restored() const { return this->restored; }

private:
	std::vector<HiddenAgentWindow> windows; ///< The windows this scope moved.
	int hidden_windows = 0; ///< Agent windows off screen for this capture.
	bool counted = false; ///< Whether this scope is part of the nesting depth.
	bool owns = false; ///< Whether this scope is the one that moved the windows.
	bool restored = true; ///< Whether the restore returned every window exactly.
};

/** What a capture request asks for, parsed and validated before any game state is used. */
struct CaptureRequest {
	/** How the pixels of the capture are produced. */
	enum class Mode : uint8_t {
		ScreenFrame,     ///< The live screen frame, as before.
		Region,          ///< A rectangle of the live screen frame.
		IndependentView, ///< A viewport rendered on its own, without touching the player's viewport.
		Minimap,         ///< The smallmap owner-colour map of the whole world.
	};

	Mode mode = Mode::ScreenFrame;
	std::string kind = "viewport";
	bool hide_agent_ui = false;
	bool include_png = true;
	uint64_t max_pixels = DEFAULT_MAX_PIXELS;
	size_t max_bytes = DEFAULT_MAX_BYTES;
	CaptureRect region{0, 0, 0, 0}; ///< Requested region, before it is clamped to the frame.
	int width = 0; ///< Requested image width, 0 for the default of the mode.
	int height = 0; ///< Requested image height, 0 for the default of the mode.
	ZoomLevel zoom = ZoomLevel::Normal; ///< Zoom level of an independent render.
	TileIndex target = INVALID_TILE; ///< Tile an independent render is centred on.
};

/**
 * Name of a capture mode, for the JSON results.
 * @param mode The mode.
 * @return The name of the mode.
 */
static std::string_view CaptureModeName(CaptureRequest::Mode mode)
{
	switch (mode) {
		case CaptureRequest::Mode::ScreenFrame: return "screen_frame";
		case CaptureRequest::Mode::Region: return "screen_frame_region";
		case CaptureRequest::Mode::IndependentView: return "independent_viewport";
		case CaptureRequest::Mode::Minimap: return "minimap";
	}
	return "unknown";
}

static std::string_view ZoomLevelName(ZoomLevel zoom);
static Json ViewportJson(const Viewport &vp);
static bool ResolveTile(const Json &params, TileIndex &tile, std::string_view &error);

/**
 * Parse and validate the parameters of a capture request.
 *
 * Only the request itself is looked at, so a caller error is reported as such even when there is
 * nothing to capture. Nothing is silently ignored: a field that does not apply to the requested
 * kind, or that has the wrong type, is "invalid_params".
 *
 * @param params  The request parameters.
 * @param request Receives the parsed request.
 * @return The error to report, or an empty view on success.
 */
static std::string_view ParseCaptureRequest(const Json &params, CaptureRequest &request)
{
	try {
		if (!ReadHideAgentUi(params, request.hide_agent_ui)) return "invalid_params";

		request.kind = params.value("kind", request.kind);
		request.max_pixels = std::min<uint64_t>(params.value("max_pixels", DEFAULT_MAX_PIXELS), HARD_MAX_PIXELS);
		/* Clamp to the largest PNG the backend can still receive, so an accepted result is deliverable. */
		request.max_bytes = std::min<size_t>(params.value("max_bytes", DEFAULT_MAX_BYTES), MAX_PNG_BYTES);
		request.include_png = params.value("include_png", request.include_png);

		const bool has_target = params.contains("target");
		const bool has_size = params.contains("width") || params.contains("height");
		const bool has_zoom = params.contains("zoom");
		const bool has_region = params.contains("region");

		if (request.kind == "viewport") {
			if (has_region) return "invalid_params";
			if (!has_target) {
				/* The screen frame has the size of the screen; a size or a zoom would be ignored. */
				if (has_size || has_zoom) return "invalid_params";
				return {};
			}

			request.mode = CaptureRequest::Mode::IndependentView;
			if (!params["target"].is_object()) return "invalid_params";
			request.width = params.value("width", 0);
			request.height = params.value("height", 0);
			if ((request.width == 0) != (request.height == 0)) return "invalid_params";
			if (request.width < 0 || request.height < 0) return "invalid_params";
			if (request.width > MAX_CAPTURE_SIDE || request.height > MAX_CAPTURE_SIDE) return "invalid_params";
			const int zoom = params.value("zoom", to_underlying(ZoomLevel::Normal));
			if (zoom < to_underlying(ZoomLevel::Min) || zoom > to_underlying(ZoomLevel::Max)) return "invalid_params";
			request.zoom = static_cast<ZoomLevel>(zoom);
			std::string_view error;
			if (!ResolveTile(params["target"], request.target, error)) return error;
			return {};
		}

		if (request.kind == "region") {
			if (has_target || has_size || has_zoom) return "invalid_params";
			if (!has_region || !params["region"].is_object()) return "invalid_params";
			const Json &region = params["region"];
			if (!region.contains("left") || !region.contains("top") || !region.contains("width") || !region.contains("height")) return "invalid_params";
			request.mode = CaptureRequest::Mode::Region;
			request.region.left = region.value("left", 0);
			request.region.top = region.value("top", 0);
			request.region.width = region.value("width", 0);
			request.region.height = region.value("height", 0);
			return {};
		}

		if (request.kind == "minimap") {
			/* The whole map is shown, so there is nothing to aim at and nothing to zoom. */
			if (has_target || has_zoom || has_region) return "invalid_params";
			request.mode = CaptureRequest::Mode::Minimap;
			request.width = params.value("width", 0);
			request.height = params.value("height", 0);
			if ((request.width == 0) != (request.height == 0)) return "invalid_params";
			if (request.width < 0 || request.height < 0) return "invalid_params";
			if (request.width > MAX_CAPTURE_SIDE || request.height > MAX_CAPTURE_SIDE) return "invalid_params";
			return {};
		}
	} catch (const Json::exception &) {
		return "invalid_params";
	}

	return "invalid_params";
}

/** The camera of an independent render: the viewport of the image, not of the player. */
struct IndependentCamera {
	Viewport viewport; ///< Viewport the image is rendered from.
	int center_x; ///< World x of the point in the middle of the image.
	int center_y; ///< World y of the point in the middle of the image.
	int center_z; ///< World z of the point in the middle of the image.
};

/**
 * Build the camera of an independent render, centred on a tile.
 *
 * The formulas are those of SetupScreenshotViewport(): the virtual size follows from the image size
 * and the zoom, and the virtual position is chosen so that the requested point ends up in the middle
 * of the image. Nothing of the player's viewport is read or written.
 *
 * @param tile   The tile to centre on.
 * @param width  Image width in pixels.
 * @param height Image height in pixels.
 * @param zoom   Zoom level of the render.
 * @return The camera to render.
 */
static IndependentCamera MakeIndependentCamera(TileIndex tile, int width, int height, ZoomLevel zoom)
{
	IndependentCamera camera{};
	/* The centre of the tile at ground height, the same point ScrollMainWindowToTile() centres on. */
	camera.center_x = TileX(tile) * TILE_SIZE + TILE_SIZE / 2;
	camera.center_y = TileY(tile) * TILE_SIZE + TILE_SIZE / 2;
	camera.center_z = GetSlopePixelZ(camera.center_x, camera.center_y);

	const Point world = RemapCoords(camera.center_x, camera.center_y, camera.center_z);
	Viewport &vp = camera.viewport;
	vp.left = 0;
	vp.top = 0;
	vp.width = width;
	vp.height = height;
	vp.zoom = zoom;
	vp.virtual_width = ScaleByZoom(width, zoom);
	vp.virtual_height = ScaleByZoom(height, zoom);
	vp.virtual_left = world.x - vp.virtual_width / 2;
	vp.virtual_top = world.y - vp.virtual_height / 2;
	vp.overlay = nullptr;
	return camera;
}

/**
 * Render the lines of an independent viewport into the image buffer.
 *
 * This is the rendering idiom of LargeWorldCallback() in screenshot.cpp: the screen buffer and the
 * current drawing target are swapped for the image buffer for the duration of the call, the
 * viewport is rendered in blocks of 1600 pixels and everything is restored afterwards.
 *
 * @param vp  The viewport to render.
 * @param buf Destination buffer.
 * @param y   First line to render.
 * @param pitch Number of pixels per line in the destination buffer.
 * @param n   Number of lines to render.
 */
static void IndependentViewCallback(const Viewport &vp, void *buf, uint y, uint pitch, uint n)
{
	DrawPixelInfo dpi{
		.dst_ptr = buf,
		.left = 0,
		.top = static_cast<int>(y),
		.width = vp.width,
		.height = static_cast<int>(n),
		.pitch = static_cast<int>(pitch),
		.zoom = ZoomLevel::WorldScreenshot
	};

	/* We are no longer rendering to the screen. */
	AutoRestoreBackup screen_backup(_screen, {
		.dst_ptr = buf,
		.left = 0,
		.top = 0,
		.width = static_cast<int>(pitch),
		.height = static_cast<int>(n),
		.pitch = static_cast<int>(pitch),
		.zoom = ZoomLevel::Min
	});
	AutoRestoreBackup disable_anim_backup(_screen_disable_anim, true);
	AutoRestoreBackup dpi_backup(_cur_dpi, &dpi);

	/* Render the viewport in blocks of 1600 pixels width. */
	int left = 0;
	while (vp.width - left != 0) {
		const int wx = std::min(vp.width - left, 1600);
		left += wx;

		ViewportDoDraw(vp,
			ScaleByZoom(left - wx - vp.left, vp.zoom) + vp.virtual_left,
			ScaleByZoom(y - vp.top, vp.zoom) + vp.virtual_top,
			ScaleByZoom(left - vp.left, vp.zoom) + vp.virtual_left,
			ScaleByZoom((y + n) - vp.top, vp.zoom) + vp.virtual_top
		);
	}
}

/** Size of the minimap image and of the map it shows. */
struct MinimapRender {
	int map_width; ///< Width of the map in tiles.
	int map_height; ///< Height of the map in tiles.
	int width; ///< Width of the image in pixels.
	int height; ///< Height of the image in pixels.
};

/**
 * Render the lines of a minimap image: the smallmap owner colours of the whole world.
 *
 * One pixel shows one tile, or the nearest tile when the image is smaller than the map. The x axis
 * is mirrored like the built-in minimap screenshot (MakeMinimapWorldScreenshot()), so the image
 * lines up with the isometric world.
 *
 * @param render The map and image sizes.
 * @param buf    Destination buffer.
 * @param y      First line to render.
 * @param pitch  Number of pixels per line in the destination buffer.
 * @param n      Number of lines to render.
 */
static void MinimapCallback(const MinimapRender &render, void *buf, uint y, uint pitch, uint n)
{
	uint32_t *row_buf = static_cast<uint32_t *>(buf);
	for (uint row = 0; row < n; row++, row_buf += pitch) {
		const int tile_y = static_cast<int>((y + row) * render.map_height / render.height);
		for (int x = 0; x < render.width; x++) {
			const int tile_x = render.map_width - 1 - x * render.map_width / render.width;
			const TileIndex tile = TileXY(static_cast<uint>(tile_x), static_cast<uint>(tile_y));
			const uint8_t colour = GetSmallMapOwnerPixels(tile, GetTileType(tile), IncludeHeightmap::Never) & 0xFF;

			const Colour &palette_colour = _cur_palette.palette[colour];
			row_buf[x] = static_cast<uint32_t>(palette_colour.b) |
					(static_cast<uint32_t>(palette_colour.g) << 8) |
					(static_cast<uint32_t>(palette_colour.r) << 16);
		}
	}
}

Json CaptureGameFrame(const Json &params)
{
	/* Parse and validate the whole request first, so a caller error is reported as such even when
	 * there is nothing to capture. */
	CaptureRequest request;
	const std::string_view parse_error = ParseCaptureRequest(params, request);
	if (!parse_error.empty()) return Failure(parse_error);

	/* Without a video driver, blitter or frame buffer there is nothing to capture; say so. */
	Blitter *blitter = BlitterFactory::GetCurrentBlitter();
	if (blitter == nullptr || VideoDriver::GetInstance() == nullptr || _screen.dst_ptr == nullptr || _screen.width <= 0 || _screen.height <= 0) {
		return Failure("headless_unavailable");
	}

	/* The image, and for the screen modes the part of the frame it is copied from. */
	CaptureRect rect{0, 0, _screen.width, _screen.height};
	int width = _screen.width;
	int height = _screen.height;
	switch (request.mode) {
		case CaptureRequest::Mode::ScreenFrame:
			break;

		case CaptureRequest::Mode::Region: {
			rect = request.region;

			/* Clamp the requested region to the frame that actually exists. */
			const int64_t right = std::min<int64_t>(static_cast<int64_t>(rect.left) + rect.width, _screen.width);
			const int64_t bottom = std::min<int64_t>(static_cast<int64_t>(rect.top) + rect.height, _screen.height);
			rect.left = std::max(rect.left, 0);
			rect.top = std::max(rect.top, 0);
			rect.width = static_cast<int>(right - rect.left);
			rect.height = static_cast<int>(bottom - rect.top);
			if (rect.width <= 0 || rect.height <= 0) return Failure("invalid_params");
			width = rect.width;
			height = rect.height;
			break;
		}

		case CaptureRequest::Mode::IndependentView:
			/* Without an explicit size the image is as large as the screen, at the requested zoom. */
			width = request.width != 0 ? request.width : _screen.width;
			height = request.height != 0 ? request.height : _screen.height;
			break;

		case CaptureRequest::Mode::Minimap:
			if (Map::SizeX() == 0 || Map::SizeY() == 0) return Failure("map_unavailable");
			width = request.width != 0 ? request.width : static_cast<int>(Map::SizeX());
			height = request.height != 0 ? request.height : static_cast<int>(Map::SizeY());
			break;
	}

	const uint64_t pixels = static_cast<uint64_t>(width) * static_cast<uint64_t>(height);
	if (pixels > request.max_pixels) {
		Json result = Failure("too_large");
		result["width"] = width;
		result["height"] = height;
		result["pixels"] = pixels;
		result["max_pixels"] = request.max_pixels;
		return result;
	}

	/* Use the regular PNG screenshot provider, so the encoding is the same as a normal screenshot. */
	const ScreenshotProvider *provider = nullptr;
	for (const ScreenshotProvider *candidate : ProviderManager<ScreenshotProvider>::GetProviders()) {
		if (candidate->GetName() == "png") {
			provider = candidate;
			break;
		}
	}
	if (provider == nullptr) return Failure("png_unavailable");

	/* Write to a private directory in the temporary directory, never to a file of the user. */
	CaptureTempDir temp_dir;
	if (!temp_dir.Create()) return Failure("temp_dir_unavailable");
	const std::filesystem::path path = temp_dir.Path() / "capture.png";

	const uint64_t frame_tick = TimerGameTick::counter;
	bool written = false;
	int hidden_window_count = 0;
	bool agent_ui_restored = true;
	IndependentCamera camera;
	if (request.mode == CaptureRequest::Mode::ScreenFrame || request.mode == CaptureRequest::Mode::Region) {
		/* The screen buffer can still show the frame from before a command that ran earlier in this
		 * same game loop iteration. Redraw the dirty blocks first, exactly like a viewport
		 * screenshot does, so the pixels belong to the state whose tick we report. */
		{
			/* Lock the video buffer like the crashlog screenshot does, so the frame is stable while it is copied. */
			VideoDriver::VideoBufferLocker video_buffer_lock;

			/* The agent UI is hidden inside the lock and put back before the lock is released, so the
			 * windows never stay off screen and the pixels are the world the chat was covering. */
			AgentUiHider hider(request.hide_agent_ui);
			SetScreenshotWindowVisibility(true);
			UndrawMouseCursor();
			DrawDirtyBlocks();
			SetScreenshotWindowVisibility(false);

			written = provider->MakeImage(path.string(), [&rect](void *buf, uint y, uint pitch, uint n) {
				CaptureFrameCallback(rect, buf, y, pitch, n);
			}, width, height, blitter->GetScreenDepth(), _cur_palette.palette);

			/* Put the windows back right away, so the reported state is the state after the capture.
			 * The hidden count was taken when they moved, so it still describes the captured pixels. */
			hider.Restore();
			hidden_window_count = hider.HiddenWindowCount();
			agent_ui_restored = hider.Restored();
		}
	} else if (request.mode == CaptureRequest::Mode::IndependentView) {
		/* The image is rendered from its own camera; the player's viewport is neither read nor moved. */
		camera = MakeIndependentCamera(request.target, width, height, request.zoom);
		written = provider->MakeImage(path.string(), [&camera](void *buf, uint y, uint pitch, uint n) {
			IndependentViewCallback(camera.viewport, buf, y, pitch, n);
		}, width, height, blitter->GetScreenDepth(), _cur_palette.palette);
	} else {
		const MinimapRender render{static_cast<int>(Map::SizeX()), static_cast<int>(Map::SizeY()), width, height};
		written = provider->MakeImage(path.string(), [&render](void *buf, uint y, uint pitch, uint n) {
			MinimapCallback(render, buf, y, pitch, n);
		}, width, height, 32, _cur_palette.palette);
	}

	/* If the game advanced while the image was made, the pixels are older than the state we would
	 * report; a stale frame is not visual verification. */
	const uint64_t tick_after_copy = TimerGameTick::counter;
	if (tick_after_copy != frame_tick) {
		Json result = Json::object();
		result["ok"] = false;
		result["error"] = "stale_frame";
		result["kind"] = request.kind;
		result["mode"] = CaptureModeName(request.mode);
		result["tick"] = frame_tick;
		result["tick_now"] = tick_after_copy;
		result["hide_agent_ui"] = hidden_window_count > 0;
		result["hidden_window_count"] = hidden_window_count;
		result["agent_ui_restored"] = agent_ui_restored;
		return result;
	}

	std::string png;
	if (written) {
		std::ifstream file(path, std::ios::binary);
		if (file) png.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
	}
	if (!written || png.empty()) return Failure("encode_failed");

	if (png.size() > request.max_bytes) {
		Json result = Failure("too_large");
		result["tick"] = frame_tick;
		result["png_bytes"] = png.size();
		result["max_bytes"] = request.max_bytes;
		result["max_body_bytes"] = MAX_RESULT_BODY_BYTES;
		return result;
	}

	Md5 md5;
	md5.Append(png.data(), png.size());
	MD5Hash hash;
	md5.Finish(hash);
	const std::string hash_hex = FormatArrayAsHex(hash);

	Json result = Json::object();
	result["ok"] = true;
	result["kind"] = request.kind;
	result["mode"] = CaptureModeName(request.mode);
	result["width"] = width;
	result["height"] = height;
	result["tick"] = frame_tick;
	result["hash"] = hash_hex;
	result["hash_algorithm"] = "md5";
	result["parent_hash"] = _last_capture_hash.empty() ? Json(nullptr) : Json(_last_capture_hash);
	/* True only when agent windows were really hidden; the request alone is not evidence. A render
	 * that contains no UI at all hides nothing and says so. */
	result["ui_in_frame"] = request.mode == CaptureRequest::Mode::ScreenFrame || request.mode == CaptureRequest::Mode::Region;
	result["hide_agent_ui"] = hidden_window_count > 0;
	result["hidden_window_count"] = hidden_window_count;
	result["agent_ui_restored"] = agent_ui_restored;

	switch (request.mode) {
		case CaptureRequest::Mode::ScreenFrame:
		case CaptureRequest::Mode::Region:
			result["frame"] = {
				{"redrawn", true},
				{"tick", frame_tick},
			};
			result["camera"] = {
				{"left", rect.left},
				{"top", rect.top},
				{"width", rect.width},
				{"height", rect.height},
			};
			break;

		case CaptureRequest::Mode::IndependentView:
			/* Everything needed to map between the image and the map, for this image only: the
			 * player's viewport is not what was rendered here. */
			result["coordinate_space"] = "screen_pixels";
			result["frame"] = {
				{"redrawn", false},
				{"tick", frame_tick},
			};
			result["projection"] = {
				{"kind", "isometric_viewport"},
				{"world_to_screen", "WorldToScreen(viewport, x, y, z) gives the image pixel of a world point"},
				{"screen_to_tile", "TranslateXYToTileCoord(viewport, x, y, clamp) gives the world position of an image pixel"},
				{"viewport", ViewportJson(camera.viewport)},
				{"target_tile", {{"index", request.target.base()}, {"x", TileX(request.target)}, {"y", TileY(request.target)}}},
				{"target_world", {{"x", camera.center_x}, {"y", camera.center_y}, {"z", camera.center_z}}},
				{"zoom", to_underlying(camera.viewport.zoom)},
				{"zoom_name", ZoomLevelName(camera.viewport.zoom)},
				{"sprite_detail_zoom", "normal"}, // like the built-in viewport screenshots, sprites are drawn at normal detail
				{"player_viewport_unchanged", true},
			};
			break;

		case CaptureRequest::Mode::Minimap: {
			const int map_width = static_cast<int>(Map::SizeX());
			const int map_height = static_cast<int>(Map::SizeY());
			const bool native = map_width == width && map_height == height;
			result["coordinate_space"] = "map_tiles";
			result["frame"] = {
				{"redrawn", false},
				{"tick", frame_tick},
			};
			result["map"] = {{"size_x", map_width}, {"size_y", map_height}};
			result["projection"] = {
				{"kind", "smallmap_owner_colours"},
				{"source", "GetSmallMapOwnerPixels(tile, GetTileType(tile), IncludeHeightmap::Never), the smallmap 'owner' mode"},
				{"x_mirrored", true},
				{"sampling", native ? "one_pixel_per_tile" : "nearest_tile"},
				{"tiles_per_pixel", {{"x", static_cast<double>(map_width) / width}, {"y", static_cast<double>(map_height) / height}}},
				{"pixel_from_tile", "pixel_x = ((map_size_x - 1 - tile_x) * width) / map_size_x; pixel_y = (tile_y * height) / map_size_y (integer division)"},
				{"tile_from_pixel", "tile_x = map_size_x - 1 - (pixel_x * map_size_x) / width; tile_y = (pixel_y * map_size_y) / height (integer division)"},
			};
			result["legend"] = {
				{"colour_space", "openttd_palette_index"},
				{"entries", Json::array({
					Json{{"meaning", "void"}, {"colour_index", PC_BLACK.p}},
					Json{{"meaning", "industry"}, {"colour_index", PC_DARK_GREY.p}},
					Json{{"meaning", "town_owned_land_or_house"}, {"colour_index", PC_DARK_RED.p}},
					Json{{"meaning", "water"}, {"colour_index", PC_WATER.p}},
					Json{{"meaning", "company_owned_land_or_road"}, {"colour_index", "company colour of the owner"}},
				})},
				{"unowned_land", "the player's smallmap land colour scheme (height or default colour), not a fixed palette index"},
			};
			break;
		}
	}

	result["png_bytes"] = png.size();
	if (request.include_png) result["png_base64"] = Base64Encode(png);

	_last_capture_hash = hash_hex;
	return result;
}

/**
 * Name of a zoom level, for the JSON results.
 * @param zoom The zoom level.
 * @return The name of the zoom level.
 */
static std::string_view ZoomLevelName(ZoomLevel zoom)
{
	switch (zoom) {
		case ZoomLevel::In4x: return "in4x";
		case ZoomLevel::In2x: return "in2x";
		case ZoomLevel::Normal: return "normal";
		case ZoomLevel::Out2x: return "out2x";
		case ZoomLevel::Out4x: return "out4x";
		case ZoomLevel::Out8x: return "out8x";
		case ZoomLevel::End: break;
	}
	return "unknown";
}

/**
 * Name of the game mode, for the JSON results.
 * @param mode The game mode.
 * @return The name of the game mode.
 */
static std::string_view GameModeName(GameMode mode)
{
	switch (mode) {
		case GameMode::Menu: return "menu";
		case GameMode::Normal: return "normal";
		case GameMode::Editor: return "editor";
		case GameMode::Bootstrap: return "bootstrap";
	}
	return "unknown";
}

/**
 * The main window, when it has a viewport.
 * @return The main window, or nullptr when the game shows no main viewport.
 */
static Window *GetMainViewportWindow()
{
	Window *w = FindWindowById(WindowClass::MainWindow, 0);
	return (w != nullptr && w->viewport != nullptr) ? w : nullptr;
}

/**
 * Describe the rectangle and the zoom of a viewport, in screen pixels.
 * @param vp The viewport.
 * @return The JSON description.
 */
static Json ViewportJson(const Viewport &vp)
{
	return Json{
		{"left", vp.left},
		{"top", vp.top},
		{"width", vp.width},
		{"height", vp.height},
		{"virtual_left", vp.virtual_left},
		{"virtual_top", vp.virtual_top},
		{"virtual_width", vp.virtual_width},
		{"virtual_height", vp.virtual_height},
		{"zoom", to_underlying(vp.zoom)},
		{"zoom_name", ZoomLevelName(vp.zoom)},
	};
}

/**
 * Describe where a viewport looks and how far it is zoomed in.
 * @param vp The viewport data of the main window.
 * @return The JSON description.
 */
static Json CameraJson(const ViewportData &vp)
{
	const int center_x = vp.virtual_left + vp.virtual_width / 2;
	const int center_y = vp.virtual_top + vp.virtual_height / 2;
	const bool center_on_map = center_x >= 0 && center_y >= 0 &&
			center_x < static_cast<int>(Map::SizeX()) * static_cast<int>(TILE_SIZE) &&
			center_y < static_cast<int>(Map::SizeY()) * static_cast<int>(TILE_SIZE);

	return Json{
		{"center_world", {{"x", center_x}, {"y", center_y}}},
		{"center_tile", center_on_map ? Json{{"x", center_x / static_cast<int>(TILE_SIZE)}, {"y", center_y / static_cast<int>(TILE_SIZE)}} : Json(nullptr)},
		{"scrollpos", {{"x", vp.scrollpos_x}, {"y", vp.scrollpos_y}}},
		{"dest_scrollpos", {{"x", vp.dest_scrollpos_x}, {"y", vp.dest_scrollpos_y}}},
		{"zoom", to_underlying(vp.zoom)},
		{"zoom_name", ZoomLevelName(vp.zoom)},
	};
}

/**
 * The window that covers a screen position, when it is not the main window.
 * @param x Screen x.
 * @param y Screen y.
 * @return The occluding window as JSON, or null when the main window is on top there.
 */
static Json OccludingWindowJson(int x, int y)
{
	const Window *w = FindWindowFromPt(x, y);
	if (w == nullptr || w->window_class == WindowClass::MainWindow) return Json(nullptr);
	return Json{
		{"class", to_underlying(w->window_class)},
		{"number", static_cast<int32_t>(w->window_number)},
	};
}

/**
 * Project a tile to the screen.
 * @param vp   The viewport to project into.
 * @param tile The tile to project.
 * @return The projection as JSON.
 */
static Json ProjectTile(const Viewport &vp, TileIndex tile)
{
	/* The centre of the tile at ground height, the same point ScrollMainWindowToTile() centres on. */
	const int x = TileX(tile) * TILE_SIZE + TILE_SIZE / 2;
	const int y = TileY(tile) * TILE_SIZE + TILE_SIZE / 2;
	const int z = GetSlopePixelZ(x, y);
	const Point screen = WorldToScreen(vp, x, y, z);
	const bool in_viewport = screen.x >= vp.left && screen.x < vp.left + vp.width && screen.y >= vp.top && screen.y < vp.top + vp.height;
	const Json occluding = OccludingWindowJson(screen.x, screen.y);

	return Json{
		{"coordinate_space", "screen_pixels"},
		{"tile", {{"index", tile.base()}, {"x", TileX(tile)}, {"y", TileY(tile)}}},
		{"world", {{"x", x}, {"y", y}, {"z", z}}},
		{"screen", {{"x", screen.x}, {"y", screen.y}}},
		{"in_viewport", in_viewport},
		{"visible", in_viewport && occluding.is_null()},
		{"occluding_window", occluding},
		{"zoom", to_underlying(vp.zoom)},
	};
}

/**
 * Project a screen pixel to the map.
 * @param vp           The viewport to project into.
 * @param x            Screen x.
 * @param y            Screen y.
 * @param clamp_to_map Clamp the result to the map.
 * @return The projection as JSON.
 */
static Json ProjectScreen(const Viewport &vp, int x, int y, bool clamp_to_map)
{
	const bool in_viewport = x >= vp.left && x < vp.left + vp.width && y >= vp.top && y < vp.top + vp.height;
	const Json occluding = OccludingWindowJson(x, y);

	/* TranslateXYToTileCoord() returns the world position under the pixel, not a tile index. */
	Json world = Json(nullptr);
	Json tile = Json(nullptr);
	Json tile_index = Json(nullptr);
	if (in_viewport) {
		const Point pt = TranslateXYToTileCoord(vp, x, y, clamp_to_map);
		world = {{"x", pt.x}, {"y", pt.y}};
		if (pt.x >= 0 && pt.y >= 0 &&
				pt.x < static_cast<int>(Map::SizeX()) * static_cast<int>(TILE_SIZE) &&
				pt.y < static_cast<int>(Map::SizeY()) * static_cast<int>(TILE_SIZE)) {
			tile = {{"x", pt.x / static_cast<int>(TILE_SIZE)}, {"y", pt.y / static_cast<int>(TILE_SIZE)}};
			tile_index = TileVirtXY(pt.x, pt.y).base();
		}
	}

	return Json{
		{"coordinate_space", "world"},
		{"screen", {{"x", x}, {"y", y}}},
		{"world", world},
		{"tile", tile},
		{"tile_index", tile_index},
		{"in_viewport", in_viewport},
		{"visible", in_viewport && occluding.is_null()},
		{"occluding_window", occluding},
		{"clamp_to_map", clamp_to_map},
		{"zoom", to_underlying(vp.zoom)},
	};
}

/**
 * Resolve the tile a request refers to.
 *
 * Accepts "tile" (a tile index), "tile_x"/"tile_y" (tile coordinates), "town" (a town id,
 * resolved to the town centre tile) and "center" (an object with tile coordinates "x"/"y").
 *
 * @param params The request parameters.
 * @param tile   Receives the resolved tile.
 * @param error  Receives the error to report when the tile cannot be resolved.
 * @return True iff a tile was requested and resolved.
 */
static bool ResolveTile(const Json &params, TileIndex &tile, std::string_view &error)
{
	if (params.contains("tile")) {
		const int64_t index = params.value("tile", int64_t{-1});
		if (index < 0 || index > UINT32_MAX) {
			error = "invalid_params";
			return false;
		}
		tile = static_cast<TileIndex>(index);
	} else if (params.contains("town")) {
		const int64_t town_id = params.value("town", int64_t{-1});
		if (town_id < 0 || town_id > UINT16_MAX || !Town::IsValidID(static_cast<TownID>(town_id))) {
			error = "out_of_bounds";
			return false;
		}
		tile = Town::Get(static_cast<TownID>(town_id))->xy;
	} else {
		int64_t x;
		int64_t y;
		if (params.contains("center")) {
			if (!params["center"].is_object()) {
				error = "invalid_params";
				return false;
			}
			x = params["center"].value("x", int64_t{-1});
			y = params["center"].value("y", int64_t{-1});
		} else {
			if (!params.contains("tile_x") || !params.contains("tile_y")) {
				error = "invalid_params";
				return false;
			}
			x = params.value("tile_x", int64_t{-1});
			y = params.value("tile_y", int64_t{-1});
		}
		if (x < 0 || y < 0 || x >= Map::SizeX() || y >= Map::SizeY()) {
			error = "out_of_bounds";
			return false;
		}
		tile = TileXY(static_cast<uint>(x), static_cast<uint>(y));
	}

	if (!IsValidTile(tile)) {
		error = "out_of_bounds";
		return false;
	}
	return true;
}

Json InspectGameViewport(const Json &params)
{
	/* Parse and validate the request before touching any game state. */
	const bool project_tile = params.contains("tile") || params.contains("tile_x") || params.contains("tile_y") || params.contains("town") || params.contains("center");
	const bool project_screen = params.contains("screen_x") || params.contains("screen_y");
	if (project_tile && project_screen) return Failure("invalid_params");
	if (project_screen && (!params.contains("screen_x") || !params.contains("screen_y"))) return Failure("invalid_params");

	TileIndex tile = INVALID_TILE;
	int screen_x = 0;
	int screen_y = 0;
	bool clamp_to_map = true;
	try {
		if (project_tile) {
			std::string_view error;
			if (!ResolveTile(params, tile, error)) return Failure(error);
		}
		if (project_screen) {
			screen_x = params.value("screen_x", 0);
			screen_y = params.value("screen_y", 0);
		}
		clamp_to_map = params.value("clamp_to_map", true);
	} catch (const Json::exception &) {
		return Failure("invalid_params");
	}

	Window *w = GetMainViewportWindow();
	if (w == nullptr) return Failure("viewport_unavailable");
	const ViewportData &vp = *w->viewport;

	Json result = Json::object();
	result["ok"] = true;
	result["tick"] = TimerGameTick::counter;
	result["coordinate_space"] = "screen_pixels";
	result["game_mode"] = GameModeName(_game_mode);
	result["in_game"] = _game_mode != GameMode::Menu;
	result["screen"] = {{"width", _screen.width}, {"height", _screen.height}};
	result["viewport"] = ViewportJson(vp);
	result["camera"] = CameraJson(vp);
	if (project_tile) result["projection"] = ProjectTile(vp, tile);
	if (project_screen) result["projection"] = ProjectScreen(vp, screen_x, screen_y, clamp_to_map);
	return result;
}

Json SetGameCamera(const Json &params)
{
	/* Parse and validate the request before touching any game state. */
	const bool has_tile = params.contains("tile") || params.contains("tile_x") || params.contains("tile_y") || params.contains("town") || params.contains("center");
	const bool has_scroll = params.contains("scroll");
	const bool has_zoom = params.contains("zoom");
	const bool has_zoom_delta = params.contains("zoom_delta");
	if (!has_tile && !has_scroll && !has_zoom && !has_zoom_delta) return Failure("invalid_params");
	if (has_zoom && has_zoom_delta) return Failure("invalid_params");
	if (has_tile && has_scroll) return Failure("invalid_params");

	bool instant = true;
	int zoom = 0;
	int zoom_delta = 0;
	int scroll_x = 0;
	int scroll_y = 0;
	TileIndex tile = INVALID_TILE;
	try {
		instant = params.value("instant", true);
		if (has_zoom) zoom = params.value("zoom", 0);
		if (has_zoom_delta) zoom_delta = params.value("zoom_delta", 0);
		if (has_scroll) {
			if (!params["scroll"].is_object()) return Failure("invalid_params");
			scroll_x = params["scroll"].value("dx", 0);
			scroll_y = params["scroll"].value("dy", 0);
		}
		if (has_tile) {
			std::string_view error;
			if (!ResolveTile(params, tile, error)) return Failure(error);
		}
	} catch (const Json::exception &) {
		return Failure("invalid_params");
	}

	Window *w = GetMainViewportWindow();
	if (w == nullptr) return Failure("viewport_unavailable");
	ViewportData &vp = *w->viewport;

	const int old_zoom = to_underlying(vp.zoom);
	const int old_scroll_x = vp.scrollpos_x;
	const int old_scroll_y = vp.scrollpos_y;

	/* Zoom through the normal UI zoom path, bounded by the user's zoom limits. */
	bool zoom_clamped = false;
	if (has_zoom || has_zoom_delta) {
		const int min_zoom = to_underlying(std::max(_settings_client.gui.zoom_min, ZoomLevel::Min));
		const int max_zoom = to_underlying(std::min(_settings_client.gui.zoom_max, ZoomLevel::Max));
		const int wanted = has_zoom ? zoom : old_zoom + zoom_delta;
		const int target = Clamp(wanted, min_zoom, max_zoom);
		zoom_clamped = target != wanted;
		while (to_underlying(vp.zoom) > target) {
			if (!DoZoomInOutWindow(ZOOM_IN, w)) break;
		}
		while (to_underlying(vp.zoom) < target) {
			if (!DoZoomInOutWindow(ZOOM_OUT, w)) break;
		}
	}

	/* Move through the normal UI scroll path; the requested centre is validated above and the
	 * viewport update clamps the destination to the map. */
	if (has_tile) {
		ScrollMainWindowToTile(tile, instant);
	} else if (has_scroll) {
		const int center_x = vp.virtual_left + vp.virtual_width / 2;
		const int center_y = vp.virtual_top + vp.virtual_height / 2;
		const int max_x = static_cast<int>(Map::SizeX()) * static_cast<int>(TILE_SIZE) - 1;
		const int max_y = static_cast<int>(Map::SizeY()) * static_cast<int>(TILE_SIZE) - 1;
		const int x = Clamp(center_x + ScaleByZoom(scroll_x, vp.zoom), 0, max_x);
		const int y = Clamp(center_y + ScaleByZoom(scroll_y, vp.zoom), 0, max_y);
		ScrollMainWindowTo(x, y, -1, instant);
	}

	/* An instant move only sets the destination; apply it now, so the reported state and the
	 * projection are the state the next frame is drawn from. */
	if (instant) UpdateViewportPosition(w, 0);

	Json result = Json::object();
	result["ok"] = true;
	result["tick"] = TimerGameTick::counter;
	result["changed"] = to_underlying(vp.zoom) != old_zoom || vp.scrollpos_x != old_scroll_x || vp.scrollpos_y != old_scroll_y;
	result["instant"] = instant;
	result["zoom_clamped"] = zoom_clamped;
	result["coordinate_space"] = "screen_pixels";
	result["screen"] = {{"width", _screen.width}, {"height", _screen.height}};
	result["viewport"] = ViewportJson(vp);
	result["camera"] = CameraJson(vp);
	return result;
}

} // namespace InGameAgent
