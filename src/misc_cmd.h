/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file misc_cmd.h Miscellaneous command definitions. */

#ifndef MISC_CMD_H
#define MISC_CMD_H

#include "command_type.h"
#include "economy_type.h"
#include "openttd.h"

#include <optional>

/** Different ways to determine the amount to loan/repay. */
enum class LoanCommand : uint8_t {
	Interval, ///< Loan/repay LOAN_INTERVAL.
	Max, ///< Loan/repay the maximum amount permitting money/settings.
	Amount, ///< Loan/repay the given amount.
};

CommandCost CmdMoneyCheat(DoCommandFlags, Money amount);
CommandCost CmdChangeBankBalance(DoCommandFlags flags, TileIndex tile, Money delta, CompanyID company, ExpensesType expenses_type);
CommandCost CmdIncreaseLoan(DoCommandFlags flags, LoanCommand cmd, Money amount);
CommandCost CmdDecreaseLoan(DoCommandFlags flags, LoanCommand cmd, Money amount);
CommandCost CmdSetCompanyMaxLoan(DoCommandFlags flags, CompanyID company, Money amount);
CommandCost CmdPause(DoCommandFlags flags, PauseMode mode, bool pause);

/**
 * Get the current revision of the pause state.
 *
 * The revision is bumped by every accepted change of the pause mode through Commands::Pause, so a
 * caller can tell "the pause state is still the one I read" from "someone paused or unpaused since
 * then" without guessing who did it.
 * @return The revision; monotonic for the lifetime of the process.
 */
uint32_t PauseRevision();

/** Why a guarded pause request was rejected. */
enum class PauseGuardResult : uint8_t {
	Accepted,         ///< The request may be applied.
	RevisionMismatch, ///< The pause state changed after the revision was observed.
	EpochChanged,     ///< The revision was observed before the current epoch started.
};

/** What a guarded pause request has to do. */
struct PauseRequestDecision {
	PauseGuardResult guard = PauseGuardResult::Accepted; ///< Whether the guard accepts the request.
	bool post = false;                                   ///< Whether a pause command has to be posted.
};

/**
 * Decide what a pause request has to do, given the state the caller observed.
 *
 * The guard is the revision the caller read, which is the revision of the pause state at that
 * moment; it is accepted only while the state has not moved since. A request that would not change
 * the pause bit this tool controls needs no command at all, so a revision only moves on a real
 * decision.
 *
 * The epoch check is an additional, coarse one: a revision below the epoch start cannot be from
 * this epoch, but an equal revision can be, because an epoch that starts when the pause state was
 * left alone begins with the revision of the previous epoch. The real protection across a load is
 * the world/epoch binding of the request itself, not this comparison.
 *
 * @param normal_now Whether the pause bit the tool controls is set now.
 * @param requested The state the caller asks for.
 * @param expected_revision The revision the caller observed, if it guards the request.
 * @param revision The current revision.
 * @param revision_at_epoch_start The revision the current epoch started with.
 * @return The guard result and whether a command has to be posted.
 */
inline PauseRequestDecision DecidePauseRequest(bool normal_now, bool requested, std::optional<uint32_t> expected_revision,
		uint32_t revision, uint32_t revision_at_epoch_start)
{
	PauseRequestDecision decision;

	if (expected_revision.has_value()) {
		if (*expected_revision < revision_at_epoch_start) {
			decision.guard = PauseGuardResult::EpochChanged;
			return decision;
		}
		if (*expected_revision != revision) {
			decision.guard = PauseGuardResult::RevisionMismatch;
			return decision;
		}
	}

	decision.post = normal_now != requested;
	return decision;
}

DEF_CMD_TRAIT(Commands::MoneyCheat, CmdMoneyCheat, CommandFlags({CommandFlag::Offline, CommandFlag::NoEst}), CommandType::Cheat)
DEF_CMD_TRAIT(Commands::ChangeBankBalance, CmdChangeBankBalance, CommandFlag::Deity, CommandType::MoneyManagement)
DEF_CMD_TRAIT(Commands::IncreaseLoan, CmdIncreaseLoan, {}, CommandType::MoneyManagement)
DEF_CMD_TRAIT(Commands::DecreaseLoan, CmdDecreaseLoan, {}, CommandType::MoneyManagement)
DEF_CMD_TRAIT(Commands::SetCompanyMaxLoan, CmdSetCompanyMaxLoan, CommandFlag::Deity, CommandType::MoneyManagement)
DEF_CMD_TRAIT(Commands::Pause, CmdPause, CommandFlags({CommandFlag::Server, CommandFlag::NoEst}), CommandType::ServerSetting)

#endif /* MISC_CMD_H */
