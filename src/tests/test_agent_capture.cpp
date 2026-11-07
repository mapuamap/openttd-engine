/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file test_agent_capture.cpp Test the camera and projection helpers of the agent bridge. */

#include "../stdafx.h"

#include "../3rdparty/catch2/catch.hpp"

#include "../agent_capture.h"
#include "../core/math_func.hpp"
#include "../tile_type.h"
#include "../viewport_type.h"
#include "../window_func.h"
#include "../window_type.h"
#include "../zoom_func.h"

#include "../safeguards.h"

/**
 * A viewport that is not attached to a window, with a scroll position that is not a
 * multiple of the zoom scale, like a real one.
 */
static Viewport MakeTestViewport(ZoomLevel zoom)
{
	Viewport vp{};
	vp.left = 0;
	vp.top = 0;
	vp.width = 800;
	vp.height = 600;
	vp.zoom = zoom;
	vp.virtual_width = ScaleByZoom(vp.width, vp.zoom);
	vp.virtual_height = ScaleByZoom(vp.height, vp.zoom);
	vp.virtual_left = 12345;
	vp.virtual_top = 6789;
	return vp;
}

TEST_CASE("Agent capture: WorldToScreen round-trips through the game's inverse")
{
	for (int z = 0; z <= 6; z++) {
		const Viewport vp = MakeTestViewport(static_cast<ZoomLevel>(z));

		for (int tx = 0; tx < 64; tx++) {
			for (int ty = 0; ty < 64; ty++) {
				const int wx = tx * static_cast<int>(TILE_SIZE) + static_cast<int>(TILE_SIZE) / 2;
				const int wy = ty * static_cast<int>(TILE_SIZE) + static_cast<int>(TILE_SIZE) / 2;

				const Point screen = InGameAgent::WorldToScreen(vp, wx, wy, 0);

				/* Exactly what TranslateXYToTileCoord() does with a screen pixel. */
				const int vx = ScaleByZoom(screen.x - vp.left, vp.zoom) + vp.virtual_left;
				const int vy = ScaleByZoom(screen.y - vp.top, vp.zoom) + vp.virtual_top;
				const Point back = InverseRemapCoords(vx, vy);

				/* The zoom scaling rounds, so the world position may be off by less than a
				 * quarter tile, but the tile itself must be the same. */
				CHECK(std::abs(back.x - wx) < static_cast<int>(TILE_SIZE) / 2);
				CHECK(std::abs(back.y - wy) < static_cast<int>(TILE_SIZE) / 2);
				CHECK(back.x / static_cast<int>(TILE_SIZE) == tx);
				CHECK(back.y / static_cast<int>(TILE_SIZE) == ty);
			}
		}
	}
}

TEST_CASE("Agent capture: WorldToScreen puts the viewport centre in the middle")
{
	for (int z = 0; z <= 6; z++) {
		const Viewport vp = MakeTestViewport(static_cast<ZoomLevel>(z));

		const Point world = InverseRemapCoords(vp.virtual_left + vp.virtual_width / 2, vp.virtual_top + vp.virtual_height / 2);
		const Point screen = InGameAgent::WorldToScreen(vp, world.x, world.y, 0);

		/* InverseRemapCoords() rounds down, so allow the rounding of the zoom scale. */
		CHECK(std::abs(screen.x - (vp.left + vp.width / 2)) <= 8);
		CHECK(std::abs(screen.y - (vp.top + vp.height / 2)) <= 8);
	}
}

TEST_CASE("Agent capture: hide_agent_ui is read strictly")
{
	SECTION("absent means false") {
		bool hide = true;
		CHECK(InGameAgent::ReadHideAgentUi(InGameAgent::Json::object(), hide));
		CHECK(hide == false);
	}

	SECTION("a boolean is taken as it is") {
		bool hide = false;
		CHECK(InGameAgent::ReadHideAgentUi(InGameAgent::Json{{"hide_agent_ui", true}}, hide));
		CHECK(hide == true);
		CHECK(InGameAgent::ReadHideAgentUi(InGameAgent::Json{{"hide_agent_ui", false}}, hide));
		CHECK(hide == false);
	}

	SECTION("a non-boolean is rejected, never read as truthy") {
		for (const InGameAgent::Json &value : {InGameAgent::Json("true"), InGameAgent::Json("false"),
				InGameAgent::Json(1), InGameAgent::Json(0), InGameAgent::Json(nullptr),
				InGameAgent::Json::array(), InGameAgent::Json::object()}) {
			bool hide = false;
			CHECK(InGameAgent::ReadHideAgentUi(InGameAgent::Json{{"hide_agent_ui", value}}, hide) == false);
		}
	}
}

TEST_CASE("Agent capture: a wrongly typed hide_agent_ui is a capture error")
{
	/* The type is checked before the video driver, so a caller error is reported as such even
	 * when there is nothing to capture (as in this windowless test binary). */
	CHECK(FindWindowById(WindowClass::MainWindow, 0) == nullptr);

	const InGameAgent::Json wrong = InGameAgent::CaptureGameFrame(InGameAgent::Json{{"hide_agent_ui", "yes"}});
	CHECK(wrong["ok"] == false);
	CHECK(wrong["error"] == "invalid_params");
	CHECK(wrong.contains("tick"));

	/* A correctly typed field gets past the validation and fails on the missing frame instead. */
	const InGameAgent::Json accepted = InGameAgent::CaptureGameFrame(InGameAgent::Json{{"hide_agent_ui", true}});
	CHECK(accepted["ok"] == false);
	CHECK(accepted["error"] == "headless_unavailable");
}

TEST_CASE("Agent capture: independent and minimap requests are validated strictly")
{
	using Json = InGameAgent::Json;

	/* All of these are rejected while parsing, i.e. before the video driver is looked at, so the
	 * windowless test binary sees the caller error itself. */
	SECTION("a target needs an object") {
		CHECK(InGameAgent::CaptureGameFrame(Json{{"target", "tile 5"}})["error"] == "invalid_params");
	}

	SECTION("a target with the wrong kind is rejected, not ignored") {
		CHECK(InGameAgent::CaptureGameFrame(Json{{"kind", "region"}, {"target", {{"tile", 0}}}, {"region", {{"left", 0}, {"top", 0}, {"width", 4}, {"height", 4}}}})["error"] == "invalid_params");
		CHECK(InGameAgent::CaptureGameFrame(Json{{"kind", "minimap"}, {"target", {{"tile", 0}}}})["error"] == "invalid_params");
	}

	SECTION("a size or zoom without a target is rejected, not ignored") {
		CHECK(InGameAgent::CaptureGameFrame(Json{{"width", 64}, {"height", 64}})["error"] == "invalid_params");
		CHECK(InGameAgent::CaptureGameFrame(Json{{"zoom", 2}})["error"] == "invalid_params");
		CHECK(InGameAgent::CaptureGameFrame(Json{{"kind", "region"}, {"zoom", 2}, {"region", {{"left", 0}, {"top", 0}, {"width", 4}, {"height", 4}}}})["error"] == "invalid_params");
	}

	SECTION("half a size and a bad zoom are rejected") {
		CHECK(InGameAgent::CaptureGameFrame(Json{{"target", {{"tile", 0}}}, {"width", 64}})["error"] == "invalid_params");
		CHECK(InGameAgent::CaptureGameFrame(Json{{"target", {{"tile", 0}}}, {"zoom", 7}})["error"] == "invalid_params");
		CHECK(InGameAgent::CaptureGameFrame(Json{{"target", {{"tile", 0}}}, {"zoom", -1}})["error"] == "invalid_params");
		CHECK(InGameAgent::CaptureGameFrame(Json{{"target", {{"tile", 0}}}, {"width", 100000}, {"height", 100000}})["error"] == "invalid_params");
	}

	SECTION("minimap rejects a target and a zoom") {
		CHECK(InGameAgent::CaptureGameFrame(Json{{"kind", "minimap"}, {"zoom", 2}})["error"] == "invalid_params");
		CHECK(InGameAgent::CaptureGameFrame(Json{{"kind", "minimap"}, {"width", 64}})["error"] == "invalid_params");
		CHECK(InGameAgent::CaptureGameFrame(Json{{"kind", "minimap"}, {"width", "big"}, {"height", 64}})["error"] == "invalid_params");
	}

	SECTION("wrong types are rejected") {
		CHECK(InGameAgent::CaptureGameFrame(Json{{"kind", "minimap"}, {"width", "big"}, {"height", 64}})["error"] == "invalid_params");
		CHECK(InGameAgent::CaptureGameFrame(Json{{"kind", 5}})["error"] == "invalid_params");
		CHECK(InGameAgent::CaptureGameFrame(Json{{"target", {{"tile", "zero"}}}})["error"] == "invalid_params");
	}

	SECTION("a well formed request gets past validation") {
		/* Past the parsing there is no frame buffer in this binary, so the honest answer is that. */
		CHECK(InGameAgent::CaptureGameFrame(Json{{"kind", "minimap"}})["error"] == "headless_unavailable");
		CHECK(InGameAgent::CaptureGameFrame(Json{{"kind", "minimap"}, {"width", 256}, {"height", 256}})["error"] == "headless_unavailable");

		/* A target is parsed here and then fails for a reason of the binary, not of the parser: with
		 * no map it is out of bounds, and when another test case allocated one the render still has
		 * no frame buffer. Both answers are honest, `invalid_params` would not be. */
		const Json targeted = InGameAgent::CaptureGameFrame(Json{{"target", {{"tile", 0}}}, {"width", 64}, {"height", 64}, {"zoom", 3}});
		CHECK(targeted["ok"] == false);
		CHECK((targeted["error"] == "out_of_bounds" || targeted["error"] == "headless_unavailable"));
	}
}

TEST_CASE("Agent capture: an independent camera centres its target in the image")
{
	/* The viewport of an independent render is a plain Viewport, so the mapping helpers of the
	 * module apply to it unchanged. */
	Viewport camera{};
	camera.left = 0;
	camera.top = 0;
	camera.width = 640;
	camera.height = 480;
	camera.zoom = ZoomLevel::Normal;
	camera.virtual_width = ScaleByZoom(camera.width, camera.zoom);
	camera.virtual_height = ScaleByZoom(camera.height, camera.zoom);

	/* The scroll position is in remapped space, like a real viewport: the centre of the camera is
	 * the remapped world point it looks at. */
	const Point centre = RemapCoords(20000, 10000, 0);
	camera.virtual_left = centre.x - camera.virtual_width / 2;
	camera.virtual_top = centre.y - camera.virtual_height / 2;

	/* A world point at the centre of the camera maps to the centre of the image. */
	const Point middle = InGameAgent::WorldToScreen(camera, 20000, 10000, 0);
	CHECK(std::abs(middle.x - camera.width / 2) <= 1);
	CHECK(std::abs(middle.y - camera.height / 2) <= 1);

	/* And the mapping stays inside the image, i.e. the camera does not fall back on the screen. */
	CHECK(InGameAgent::WorldToScreen(camera, 20000, 10000, 0).x >= 0);
	CHECK(InGameAgent::WorldToScreen(camera, 20000, 10000, 0).y >= 0);
}

TEST_CASE("Agent capture: camera queries without a main window")
{
	/* The test binary has no window system, so there is no main viewport. */
	CHECK(FindWindowById(WindowClass::MainWindow, 0) == nullptr);

	SECTION("InspectGameViewport reports the missing viewport") {
		const InGameAgent::Json result = InGameAgent::InspectGameViewport(InGameAgent::Json::object());
		CHECK(result["ok"] == false);
		CHECK(result["error"] == "viewport_unavailable");
		CHECK(result.contains("tick"));
	}

	SECTION("SetGameCamera reports the missing viewport") {
		const InGameAgent::Json result = InGameAgent::SetGameCamera(InGameAgent::Json{{"zoom", 2}});
		CHECK(result["ok"] == false);
		CHECK(result["error"] == "viewport_unavailable");
		CHECK(result.contains("tick"));
	}
}

TEST_CASE("Agent capture: camera requests are validated before the viewport is used")
{
	SECTION("InspectGameViewport rejects a negative tile") {
		const InGameAgent::Json result = InGameAgent::InspectGameViewport(InGameAgent::Json{{"tile", -1}});
		CHECK(result["ok"] == false);
		CHECK(result["error"] == "invalid_params");
	}

	SECTION("InspectGameViewport rejects a tile that is not on any map") {
		const InGameAgent::Json result = InGameAgent::InspectGameViewport(InGameAgent::Json{{"tile", 0xFFFFFFF}});
		CHECK(result["ok"] == false);
		CHECK(result["error"] == "out_of_bounds");
	}

	SECTION("InspectGameViewport rejects half a screen position") {
		const InGameAgent::Json result = InGameAgent::InspectGameViewport(InGameAgent::Json{{"screen_x", 10}});
		CHECK(result["ok"] == false);
		CHECK(result["error"] == "invalid_params");
	}

	SECTION("InspectGameViewport rejects two projections at once") {
		const InGameAgent::Json result = InGameAgent::InspectGameViewport(InGameAgent::Json{{"tile", 0}, {"screen_x", 10}, {"screen_y", 10}});
		CHECK(result["ok"] == false);
		CHECK(result["error"] == "invalid_params");
	}

	SECTION("InspectGameViewport rejects a tile of the wrong type") {
		const InGameAgent::Json result = InGameAgent::InspectGameViewport(InGameAgent::Json{{"tile", "zero"}});
		CHECK(result["ok"] == false);
		CHECK(result["error"] == "invalid_params");
	}

	SECTION("SetGameCamera rejects a request that changes nothing") {
		const InGameAgent::Json result = InGameAgent::SetGameCamera(InGameAgent::Json::object());
		CHECK(result["ok"] == false);
		CHECK(result["error"] == "invalid_params");
	}

	SECTION("SetGameCamera rejects a zoom of the wrong type") {
		const InGameAgent::Json result = InGameAgent::SetGameCamera(InGameAgent::Json{{"zoom", "far"}});
		CHECK(result["ok"] == false);
		CHECK(result["error"] == "invalid_params");
	}

	SECTION("SetGameCamera rejects a tile and a scroll at once") {
		const InGameAgent::Json result = InGameAgent::SetGameCamera(InGameAgent::Json{{"tile", 0}, {"scroll", {{"dx", 1}, {"dy", 1}}}});
		CHECK(result["ok"] == false);
		CHECK(result["error"] == "invalid_params");
	}

	SECTION("SetGameCamera rejects a tile that is not on any map") {
		const InGameAgent::Json result = InGameAgent::SetGameCamera(InGameAgent::Json{{"tile", 0xFFFFFFF}});
		CHECK(result["ok"] == false);
		CHECK(result["error"] == "out_of_bounds");
	}
}

TEST_CASE("Agent capture: the camera parameters the bridge advertises are recognised")
{
	/* world.camera.get and a world.project projection advertise exactly these names, and
	 * world.camera.set advertises the moves. The test binary has neither a map nor a window, so the
	 * call cannot succeed here, but a name the helper does not know is rejected as invalid_params
	 * before anything else: that is the difference this case pins down. */
	for (const InGameAgent::Json &params : {
			InGameAgent::Json{{"tile", 0}},
			InGameAgent::Json{{"tile_x", 10}, {"tile_y", 10}},
			InGameAgent::Json{{"center", {{"x", 4}, {"y", 4}}}},
			InGameAgent::Json{{"screen_x", 10}, {"screen_y", 10}},
			InGameAgent::Json{{"screen_x", 10}, {"screen_y", 10}, {"clamp_to_map", false}},
	}) {
		const InGameAgent::Json read = InGameAgent::InspectGameViewport(params);
		CHECK(read["ok"] == false);
		CHECK(read["error"] != "invalid_params");
	}

	for (const InGameAgent::Json &params : {
			InGameAgent::Json{{"tile", 0}},
			InGameAgent::Json{{"tile_x", 10}, {"tile_y", 10}},
			InGameAgent::Json{{"center", {{"x", 4}, {"y", 4}}}},
			InGameAgent::Json{{"scroll", {{"dx", 8}, {"dy", 8}}}},
			InGameAgent::Json{{"zoom", 2}},
			InGameAgent::Json{{"zoom_delta", 1}},
			InGameAgent::Json{{"zoom", 2}, {"instant", false}},
	}) {
		const InGameAgent::Json write = InGameAgent::SetGameCamera(params);
		CHECK(write["ok"] == false);
		CHECK(write["error"] != "invalid_params");
	}
}
