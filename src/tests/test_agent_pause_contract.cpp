/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * @file test_agent_pause_contract.cpp Test the guard that decides whether a pause request may be
 * applied, i.e. the read/restore decision the agent bridge exposes as `world.pause`.
 *
 * The decision is a pure function of the pause bit the tool controls, the requested state and the
 * revision the caller observed, so it is tested here without a game, a video driver or a command:
 * the cases below are the ones the contract promises the environment worker.
 */

#include "../stdafx.h"

#include "../3rdparty/catch2/catch.hpp"

#include "../misc_cmd.h"

#include "../safeguards.h"

/**
 * Run the pause decision with the epoch start at revision 0 unless a case says otherwise.
 * @param normal_now Whether the pause bit the tool controls is set now.
 * @param requested The state the caller asks for.
 * @param expected The revision the caller observed, if any.
 * @param revision The current revision.
 * @param revision_at_epoch_start The revision the current epoch started with.
 * @return The decision.
 */
static PauseRequestDecision Decide(bool normal_now, bool requested, std::optional<uint32_t> expected,
		uint32_t revision, uint32_t revision_at_epoch_start = 0)
{
	return DecidePauseRequest(normal_now, requested, expected, revision, revision_at_epoch_start);
}

TEST_CASE("Pause contract: an unguarded request only posts a real change")
{
	/* Nothing to change: no command, so no revision moves. */
	CHECK(Decide(false, false, std::nullopt, 7).guard == PauseGuardResult::Accepted);
	CHECK_FALSE(Decide(false, false, std::nullopt, 7).post);
	CHECK_FALSE(Decide(true, true, std::nullopt, 7).post);

	/* A real change is posted. */
	CHECK(Decide(false, true, std::nullopt, 7).post);
	CHECK(Decide(true, false, std::nullopt, 7).post);
}

TEST_CASE("Pause contract: the guard accepts only the observed revision")
{
	/* The revision the caller read is still the one in force. */
	CHECK(Decide(false, true, uint32_t{7}, 7).guard == PauseGuardResult::Accepted);
	CHECK(Decide(false, true, uint32_t{7}, 7).post);

	/* A pause decision made after the read, by the player or by another task, is stale. */
	const PauseRequestDecision newer = Decide(false, true, uint32_t{7}, 8);
	CHECK(newer.guard == PauseGuardResult::RevisionMismatch);
	CHECK_FALSE(newer.post);

	/* A revision from the future is just as unusable. */
	CHECK(Decide(false, true, uint32_t{9}, 8).guard == PauseGuardResult::RevisionMismatch);
}

TEST_CASE("Pause contract: the epoch check is only an additional one")
{
	/* Older than the epoch start: it cannot have been read in this epoch. */
	CHECK(Decide(false, true, uint32_t{4}, 9, 5).guard == PauseGuardResult::EpochChanged);

	/* Equal to the epoch start is accepted, because an epoch that starts while the pause state is
	 * left alone begins with the revision of the previous epoch. Only the world/epoch binding of the
	 * request can tell those two reads apart, which is why it stays required. */
	CHECK(Decide(false, true, uint32_t{5}, 5, 5).guard == PauseGuardResult::Accepted);
}

TEST_CASE("Pause contract: restoring a temporary pause uses the revision the pause returned")
{
	/* Read: the game runs, revision 10, the tool's pause bit is clear. */
	const bool previous_normal = false;
	const uint32_t observed = 10;

	/* The agent pauses. The engine accepts it and the revision moves to 11; that returned revision
	 * is the one the restore has to name, not the one from the read. */
	const PauseRequestDecision pause = Decide(previous_normal, true, std::optional<uint32_t>{observed}, observed);
	CHECK(pause.guard == PauseGuardResult::Accepted);
	CHECK(pause.post);
	const uint32_t after_pause = observed + 1;

	/* Nothing else happened: restoring the previous state with the returned revision is accepted
	 * and really unpauses. */
	const PauseRequestDecision restore = Decide(true, previous_normal, std::optional<uint32_t>{after_pause}, after_pause);
	CHECK(restore.guard == PauseGuardResult::Accepted);
	CHECK(restore.post);

	/* The player paused in between (revision 12): the restore must not undo that decision, so it is
	 * rejected and the player's pause stays in force. */
	const PauseRequestDecision stale = Decide(true, previous_normal, std::optional<uint32_t>{after_pause}, after_pause + 1);
	CHECK(stale.guard == PauseGuardResult::RevisionMismatch);
	CHECK_FALSE(stale.post);
}

TEST_CASE("Pause contract: restoring does not turn another pause reason into a normal pause")
{
	/* The game is paused for a reason of its own (saving, loading): `paused` is true, but the bit the
	 * tool controls is clear. Restoring that previous state must leave the bit alone, so the other
	 * reason survives instead of being replaced by a normal pause. */
	const bool previous_normal = false;
	const PauseRequestDecision restore = Decide(false, previous_normal, std::optional<uint32_t>{20}, 20);
	CHECK(restore.guard == PauseGuardResult::Accepted);
	CHECK_FALSE(restore.post);
}
