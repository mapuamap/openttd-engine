/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file test_network_http.cpp Tests for the HTTP request helpers. */

#include "../stdafx.h"

#include "../3rdparty/catch2/catch.hpp"

#include "../network/core/http.h"

#include "../safeguards.h"

TEST_CASE("IsValidHTTPHeaders")
{
	/* No headers at all, and headers without any CR or LF, are fine. */
	CHECK(IsValidHTTPHeaders({}));
	CHECK(IsValidHTTPHeaders({{"Authorization", "Bearer token"}}));
	CHECK(IsValidHTTPHeaders({{"Content-Type", "application/json"}, {"X-Test", "value"}}));

	/* A CR or LF in a value or name would allow injecting headers, so it is rejected. */
	CHECK_FALSE(IsValidHTTPHeaders({{"X-Test", "value\r\nInjected: yes"}}));
	CHECK_FALSE(IsValidHTTPHeaders({{"X-Test", "value\nInjected: yes"}}));
	CHECK_FALSE(IsValidHTTPHeaders({{"X-Test", "value\r"}}));
	CHECK_FALSE(IsValidHTTPHeaders({{"X-Test\r\nInjected", "value"}}));
	CHECK_FALSE(IsValidHTTPHeaders({{"X-Test", "value"}, {"X-Other", "value\n"}}));
}
