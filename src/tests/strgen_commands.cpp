/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/gpl-2.0>.
 */

/**
 * @file strgen_commands.cpp Test the parameter compatibility check of strgen.
 *
 * The diagnostics are the ones the library already links in (`src/game/game_text.cpp` defines
 * `StrgenWarningI`/`StrgenErrorI`/`StrgenFatalI` for the engine's own text compiler), so the test
 * uses them instead of defining its own: they log and count, and a fatal one throws.
 */

#include "../stdafx.h"

#include "../3rdparty/catch2/catch.hpp"

#include "../strgen/strgen.h"

#include "../safeguards.h"

TEST_CASE("Strgen - command match")
{
	/* The check only runs while a translation is compiled. */
	_strgen.translation = true;

	/* A {RAW_STRING} master string may be translated with {RAW_STRING} as well: both name the same
	 * kind of parameter, a string that is formatted as-is. This used to be reported as a mismatch,
	 * which dropped the translation and silently fell back to English. */
	CHECK(CheckCommandsMatch(" ({RAW_STRING})", " ({RAW_STRING})", "STR_TEST"));
	CHECK(CheckCommandsMatch("{GRAY}Agent: {RAW_STRING}", "{GRAY}Agent: {RAW_STRING}", "STR_TEST"));
	CHECK(CheckCommandsMatch("{RAW_STRING} (stale)", "{RAW_STRING} (stale)", "STR_TEST"));
	CHECK(CheckCommandsMatch("{RAW_STRING} - {RAW_STRING}", "{RAW_STRING} - {RAW_STRING}", "STR_TEST"));
	CHECK(CheckCommandsMatch("{RAW_STRING} {NUM}", "{RAW_STRING} {NUM}", "STR_TEST"));

	/* Spelling a string parameter as {STRING} for a {RAW_STRING} master string stays accepted, as
	 * it always was: many existing translations (e.g. the music titles) do that, and the check
	 * must not start dropping them. */
	CHECK(CheckCommandsMatch("{RAW_STRING} (stale)", "{STRING} (stale)", "STR_TEST"));
	CHECK(CheckCommandsMatch("{GRAY}Agent: {RAW_STRING}", "{GRAY}Agent: {STRING}", "STR_TEST"));

	/* An integer parameter is still not interchangeable with a string parameter. */
	CHECK_FALSE(CheckCommandsMatch("{RAW_STRING}", "{NUM}", "STR_TEST"));
	CHECK_FALSE(CheckCommandsMatch("{NUM}", "{RAW_STRING}", "STR_TEST"));
	CHECK_FALSE(CheckCommandsMatch("{RAW_STRING} {NUM}", "{RAW_STRING} {RAW_STRING}", "STR_TEST"));

	/* A different number of parameters is a mismatch as well. */
	CHECK_FALSE(CheckCommandsMatch("{RAW_STRING}", "{RAW_STRING} {RAW_STRING}", "STR_TEST"));
	CHECK_FALSE(CheckCommandsMatch("{NUM}", "{NUM} {NUM}", "STR_TEST"));

	/* Strings without parameters always match. */
	CHECK(CheckCommandsMatch("Agent: idle", "Agent: idle", "STR_TEST"));

	/* Without a translation there is nothing to compare. */
	_strgen.translation = false;
	CHECK(CheckCommandsMatch("{NUM}", "{RAW_STRING}", "STR_TEST"));
}
