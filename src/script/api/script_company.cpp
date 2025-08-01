/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file script_company.cpp Implementation of ScriptCompany. */

#include "../../stdafx.h"
#include "script_company.hpp"
#include "script_error.hpp"
#include "script_companymode.hpp"
#include "../../company_func.h"
#include "../../company_base.h"
#include "../../company_manager_face.h"
#include "../../economy_func.h"
#include "../../object_type.h"
#include "../../strings_func.h"
#include "../../tile_map.h"
#include "../../string_func.h"
#include "../../settings_func.h"
#include "../../company_cmd.h"
#include "../../misc_cmd.h"
#include "../../object_cmd.h"
#include "../../settings_cmd.h"

#include "table/strings.h"

#include "../../safeguards.h"

/* static */ ::CompanyID ScriptCompany::FromScriptCompanyID(ScriptCompany::CompanyID company)
{
	/* If this assert gets triggered, then ScriptCompany::ResolveCompanyID needed to be called before. */
	assert(company != ScriptCompany::COMPANY_SELF && company != ScriptCompany::COMPANY_SPECTATOR);

	if (company == ScriptCompany::COMPANY_INVALID) return ::CompanyID::Invalid();
	return static_cast<::CompanyID>(company);
}

/* static */ ScriptCompany::CompanyID ScriptCompany::ToScriptCompanyID(::CompanyID company)
{
	if (company == ::CompanyID::Invalid()) return ScriptCompany::COMPANY_INVALID;
	return static_cast<::ScriptCompany::CompanyID>(company.base());
}

/* static */ ScriptCompany::CompanyID ScriptCompany::ResolveCompanyID(ScriptCompany::CompanyID company)
{
	if (company == ScriptCompany::COMPANY_SELF) {
		if (!::Company::IsValidID(_current_company)) return ScriptCompany::COMPANY_INVALID;
		return ScriptCompany::ToScriptCompanyID(_current_company);
	}

	return ::Company::IsValidID(ScriptCompany::FromScriptCompanyID(company)) ? company : ScriptCompany::COMPANY_INVALID;
}

/* static */ bool ScriptCompany::IsMine(ScriptCompany::CompanyID company)
{
	EnforceCompanyModeValid(false);
	return ResolveCompanyID(company) == ResolveCompanyID(ScriptCompany::COMPANY_SELF);
}

/* static */ bool ScriptCompany::SetName(Text *name)
{
	ScriptObjectRef counter(name);

	EnforceCompanyModeValid(false);
	EnforcePrecondition(false, name != nullptr);
	const std::string &text = name->GetDecodedText();
	EnforcePreconditionEncodedText(false, text);
	EnforcePreconditionCustomError(false, ::Utf8StringLength(text) < MAX_LENGTH_COMPANY_NAME_CHARS, ScriptError::ERR_PRECONDITION_STRING_TOO_LONG);

	return ScriptObject::Command<CMD_RENAME_COMPANY>::Do(text);
}

/* static */ std::optional<std::string> ScriptCompany::GetName(ScriptCompany::CompanyID company)
{
	company = ResolveCompanyID(company);
	if (company == ScriptCompany::COMPANY_INVALID) return std::nullopt;

	return ::StrMakeValid(::GetString(STR_COMPANY_NAME, ScriptCompany::FromScriptCompanyID(company)), {});
}

/* static */ bool ScriptCompany::SetPresidentName(Text *name)
{
	ScriptObjectRef counter(name);

	EnforceCompanyModeValid(false);
	EnforcePrecondition(false, name != nullptr);
	const std::string &text = name->GetDecodedText();
	EnforcePreconditionEncodedText(false, text);
	EnforcePreconditionCustomError(false, ::Utf8StringLength(text) < MAX_LENGTH_PRESIDENT_NAME_CHARS, ScriptError::ERR_PRECONDITION_STRING_TOO_LONG);

	return ScriptObject::Command<CMD_RENAME_PRESIDENT>::Do(text);
}

/* static */ std::optional<std::string> ScriptCompany::GetPresidentName(ScriptCompany::CompanyID company)
{
	company = ResolveCompanyID(company);
	if (company == ScriptCompany::COMPANY_INVALID) return std::nullopt;

	return ::StrMakeValid(::GetString(STR_PRESIDENT_NAME, ScriptCompany::FromScriptCompanyID(company)), {});
}

/* static */ bool ScriptCompany::SetPresidentGender(Gender gender)
{
	EnforceCompanyModeValid(false);
	EnforcePrecondition(false, gender == GENDER_MALE || gender == GENDER_FEMALE);
	EnforcePrecondition(false, GetPresidentGender(ScriptCompany::COMPANY_SELF) != gender);

	assert(GetNumCompanyManagerFaceStyles() >= 2); /* At least two styles are needed to fake a gender. */

	/* Company faces no longer have a defined gender, so pick a random face style instead. */
	Randomizer &randomizer = ScriptObject::GetRandomizer();
	CompanyManagerFace cmf{};
	do {
		cmf.style = randomizer.Next(GetNumCompanyManagerFaceStyles());
	} while ((HasBit(cmf.style, 0) ? GENDER_FEMALE : GENDER_MALE) != gender);

	RandomiseCompanyManagerFaceBits(cmf, GetCompanyManagerFaceVars(cmf.style), randomizer);

	return ScriptObject::Command<CMD_SET_COMPANY_MANAGER_FACE>::Do(cmf.style, cmf.bits);
}

/* static */ ScriptCompany::Gender ScriptCompany::GetPresidentGender(ScriptCompany::CompanyID company)
{
	company = ResolveCompanyID(company);
	if (company == ScriptCompany::COMPANY_INVALID) return GENDER_INVALID;

	/* Company faces no longer have a defined gender, so fake one based on the style index. This might not match
	 * the face appearance. */
	const auto &cmf = ::Company::Get(ScriptCompany::FromScriptCompanyID(company))->face;
	return HasBit(cmf.style, 0) ? GENDER_FEMALE : GENDER_MALE;
}

/* static */ Money ScriptCompany::GetQuarterlyIncome(ScriptCompany::CompanyID company, SQInteger quarter)
{
	company = ResolveCompanyID(company);
	if (company == ScriptCompany::COMPANY_INVALID) return -1;
	if (quarter > EARLIEST_QUARTER) return -1;
	if (quarter < CURRENT_QUARTER) return -1;

	if (quarter == CURRENT_QUARTER) {
		return ::Company::Get(ScriptCompany::FromScriptCompanyID(company))->cur_economy.income;
	}
	return ::Company::Get(ScriptCompany::FromScriptCompanyID(company))->old_economy[quarter - 1].income;
}

/* static */ Money ScriptCompany::GetQuarterlyExpenses(ScriptCompany::CompanyID company, SQInteger quarter)
{
	company = ResolveCompanyID(company);
	if (company == ScriptCompany::COMPANY_INVALID) return -1;
	if (quarter > EARLIEST_QUARTER) return -1;
	if (quarter < CURRENT_QUARTER) return -1;

	if (quarter == CURRENT_QUARTER) {
		return ::Company::Get(ScriptCompany::FromScriptCompanyID(company))->cur_economy.expenses;
	}
	return ::Company::Get(ScriptCompany::FromScriptCompanyID(company))->old_economy[quarter - 1].expenses;
}

/* static */ SQInteger ScriptCompany::GetQuarterlyCargoDelivered(ScriptCompany::CompanyID company, SQInteger quarter)
{
	company = ResolveCompanyID(company);
	if (company == ScriptCompany::COMPANY_INVALID) return -1;
	if (quarter > EARLIEST_QUARTER) return -1;
	if (quarter < CURRENT_QUARTER) return -1;

	if (quarter == CURRENT_QUARTER) {
		return ::Company::Get(ScriptCompany::FromScriptCompanyID(company))->cur_economy.delivered_cargo.GetSum<OverflowSafeInt32>();
	}
	return ::Company::Get(ScriptCompany::FromScriptCompanyID(company))->old_economy[quarter - 1].delivered_cargo.GetSum<OverflowSafeInt32>();
}

/* static */ SQInteger ScriptCompany::GetQuarterlyPerformanceRating(ScriptCompany::CompanyID company, SQInteger quarter)
{
	company = ResolveCompanyID(company);
	if (company == ScriptCompany::COMPANY_INVALID) return -1;
	if (quarter > EARLIEST_QUARTER) return -1;
	if (quarter <= CURRENT_QUARTER) return -1;

	return ::Company::Get(ScriptCompany::FromScriptCompanyID(company))->old_economy[quarter - 1].performance_history;
}

/* static */ Money ScriptCompany::GetQuarterlyCompanyValue(ScriptCompany::CompanyID company, SQInteger quarter)
{
	company = ResolveCompanyID(company);
	if (company == ScriptCompany::COMPANY_INVALID) return -1;
	if (quarter > EARLIEST_QUARTER) return -1;
	if (quarter < CURRENT_QUARTER) return -1;

	if (quarter == CURRENT_QUARTER) {
		return ::CalculateCompanyValue(::Company::Get(ScriptCompany::FromScriptCompanyID(company)));
	}
	return ::Company::Get(ScriptCompany::FromScriptCompanyID(company))->old_economy[quarter - 1].company_value;
}


/* static */ Money ScriptCompany::GetBankBalance(ScriptCompany::CompanyID company)
{
	company = ResolveCompanyID(company);
	if (company == ScriptCompany::COMPANY_INVALID) return -1;
	/* If we return INT64_MAX as usual, overflows may occur in the script. So return a smaller value. */
	if (_settings_game.difficulty.infinite_money) return INT32_MAX;

	return GetAvailableMoney(ScriptCompany::FromScriptCompanyID(company));
}

/* static */ Money ScriptCompany::GetLoanAmount()
{
	ScriptCompany::CompanyID company = ResolveCompanyID(ScriptCompany::COMPANY_SELF);
	if (company == ScriptCompany::COMPANY_INVALID) return -1;

	return ::Company::Get(ScriptCompany::FromScriptCompanyID(company))->current_loan;
}

/* static */ Money ScriptCompany::GetMaxLoanAmount()
{
	if (ScriptCompanyMode::IsDeity()) return _economy.max_loan;

	ScriptCompany::CompanyID company = ResolveCompanyID(ScriptCompany::COMPANY_SELF);
	if (company == ScriptCompany::COMPANY_INVALID) return -1;

	return ::Company::Get(ScriptCompany::FromScriptCompanyID(company))->GetMaxLoan();
}

/* static */ bool ScriptCompany::SetMaxLoanAmountForCompany(ScriptCompany::CompanyID company, Money amount)
{
	EnforceDeityMode(false);
	EnforcePrecondition(false, amount >= 0 && amount <= (Money)MAX_LOAN_LIMIT);

	company = ResolveCompanyID(company);
	EnforcePrecondition(false, company != ScriptCompany::COMPANY_INVALID);
	return ScriptObject::Command<CMD_SET_COMPANY_MAX_LOAN>::Do(ScriptCompany::FromScriptCompanyID(company), amount);
}

/* static */ bool ScriptCompany::ResetMaxLoanAmountForCompany(ScriptCompany::CompanyID company)
{
	EnforceDeityMode(false);

	company = ResolveCompanyID(company);
	EnforcePrecondition(false, company != ScriptCompany::COMPANY_INVALID);

	return ScriptObject::Command<CMD_SET_COMPANY_MAX_LOAN>::Do(ScriptCompany::FromScriptCompanyID(company), COMPANY_MAX_LOAN_DEFAULT);
}

/* static */ Money ScriptCompany::GetLoanInterval()
{
	return LOAN_INTERVAL;
}

/* static */ bool ScriptCompany::SetLoanAmount(Money loan)
{
	EnforceCompanyModeValid(false);
	EnforcePrecondition(false, loan >= 0);
	EnforcePrecondition(false, ((int64_t)loan % GetLoanInterval()) == 0);
	EnforcePrecondition(false, loan <= GetMaxLoanAmount());
	EnforcePrecondition(false, (loan - GetLoanAmount() + GetBankBalance(ScriptCompany::COMPANY_SELF)) >= 0);

	if (loan == GetLoanAmount()) return true;

	Money amount = abs(loan - GetLoanAmount());

	if (loan > GetLoanAmount()) {
		return ScriptObject::Command<CMD_INCREASE_LOAN>::Do(LoanCommand::Amount, amount);
	} else {
		return ScriptObject::Command<CMD_DECREASE_LOAN>::Do(LoanCommand::Amount, amount);
	}
}

/* static */ bool ScriptCompany::SetMinimumLoanAmount(Money loan)
{
	EnforceCompanyModeValid(false);
	EnforcePrecondition(false, loan >= 0);

	Money over_interval = (int64_t)loan % GetLoanInterval();
	if (over_interval != 0) loan += GetLoanInterval() - over_interval;

	EnforcePrecondition(false, loan <= GetMaxLoanAmount());

	SetLoanAmount(loan);

	return GetLoanAmount() == loan;
}

/* static */ bool ScriptCompany::ChangeBankBalance(ScriptCompany::CompanyID company, Money delta, ExpensesType expenses_type, TileIndex tile)
{
	EnforceDeityMode(false);
	EnforcePrecondition(false, expenses_type < (ExpensesType)::EXPENSES_END);
	EnforcePrecondition(false, tile == INVALID_TILE || ::IsValidTile(tile));

	company = ResolveCompanyID(company);
	EnforcePrecondition(false, company != ScriptCompany::COMPANY_INVALID);

	/* Network commands only allow 0 to indicate invalid tiles, not INVALID_TILE */
	return ScriptObject::Command<CMD_CHANGE_BANK_BALANCE>::Do(tile == INVALID_TILE ? (TileIndex)0U : tile, delta, ScriptCompany::FromScriptCompanyID(company), (::ExpensesType)expenses_type);
}

/* static */ bool ScriptCompany::BuildCompanyHQ(TileIndex tile)
{
	EnforceCompanyModeValid(false);
	EnforcePrecondition(false, ::IsValidTile(tile));

	return ScriptObject::Command<CMD_BUILD_OBJECT>::Do(tile, OBJECT_HQ, 0);
}

/* static */ TileIndex ScriptCompany::GetCompanyHQ(ScriptCompany::CompanyID company)
{
	company = ResolveCompanyID(company);
	if (company == ScriptCompany::COMPANY_INVALID) return INVALID_TILE;

	TileIndex loc = ::Company::Get(ScriptCompany::FromScriptCompanyID(company))->location_of_HQ;
	return (loc == 0) ? INVALID_TILE : loc;
}

/* static */ bool ScriptCompany::SetAutoRenewStatus(bool autorenew)
{
	EnforceCompanyModeValid(false);
	return ScriptObject::Command<CMD_CHANGE_COMPANY_SETTING>::Do("company.engine_renew", autorenew ? 1 : 0);
}

/* static */ bool ScriptCompany::GetAutoRenewStatus(ScriptCompany::CompanyID company)
{
	company = ResolveCompanyID(company);
	if (company == ScriptCompany::COMPANY_INVALID) return false;

	return ::Company::Get(ScriptCompany::FromScriptCompanyID(company))->settings.engine_renew;
}

/* static */ bool ScriptCompany::SetAutoRenewMonths(SQInteger months)
{
	EnforceCompanyModeValid(false);
	months = Clamp<SQInteger>(months, INT16_MIN, INT16_MAX);

	return ScriptObject::Command<CMD_CHANGE_COMPANY_SETTING>::Do("company.engine_renew_months", months);
}

/* static */ SQInteger ScriptCompany::GetAutoRenewMonths(ScriptCompany::CompanyID company)
{
	company = ResolveCompanyID(company);
	if (company == ScriptCompany::COMPANY_INVALID) return 0;

	return ::Company::Get(ScriptCompany::FromScriptCompanyID(company))->settings.engine_renew_months;
}

/* static */ bool ScriptCompany::SetAutoRenewMoney(Money money)
{
	EnforceCompanyModeValid(false);
	EnforcePrecondition(false, money >= 0);
	EnforcePrecondition(false, (int64_t)money <= UINT32_MAX);
	return ScriptObject::Command<CMD_CHANGE_COMPANY_SETTING>::Do("company.engine_renew_money", money);
}

/* static */ Money ScriptCompany::GetAutoRenewMoney(ScriptCompany::CompanyID company)
{
	company = ResolveCompanyID(company);
	if (company == ScriptCompany::COMPANY_INVALID) return 0;

	return ::Company::Get(ScriptCompany::FromScriptCompanyID(company))->settings.engine_renew_money;
}

/* static */ bool ScriptCompany::SetPrimaryLiveryColour(LiveryScheme scheme, Colours colour)
{
	EnforceCompanyModeValid(false);
	return ScriptObject::Command<CMD_SET_COMPANY_COLOUR>::Do((::LiveryScheme)scheme, true, (::Colours)colour);
}

/* static */ bool ScriptCompany::SetSecondaryLiveryColour(LiveryScheme scheme, Colours colour)
{
	EnforceCompanyModeValid(false);
	return ScriptObject::Command<CMD_SET_COMPANY_COLOUR>::Do((::LiveryScheme)scheme, false, (::Colours)colour);
}

/* static */ ScriptCompany::Colours ScriptCompany::GetPrimaryLiveryColour(ScriptCompany::LiveryScheme scheme)
{
	if ((::LiveryScheme)scheme < LS_BEGIN || (::LiveryScheme)scheme >= LS_END) return COLOUR_INVALID;

	const Company *c = ::Company::GetIfValid(_current_company);
	if (c == nullptr) return COLOUR_INVALID;

	return (ScriptCompany::Colours)c->livery[scheme].colour1;
}

/* static */ ScriptCompany::Colours ScriptCompany::GetSecondaryLiveryColour(ScriptCompany::LiveryScheme scheme)
{
	if ((::LiveryScheme)scheme < LS_BEGIN || (::LiveryScheme)scheme >= LS_END) return COLOUR_INVALID;

	const Company *c = ::Company::GetIfValid(_current_company);
	if (c == nullptr) return COLOUR_INVALID;

	return (ScriptCompany::Colours)c->livery[scheme].colour2;
}
