#include "equity_option_parser.h"

#include <algorithm>
#include <vector>

#include "date_convert.h"
#include "enum_convert.h"
#include "error.h"

namespace quantra {

ParsedEquityOption EquityOptionParser::parse(const quantra::EquityOption* option) const {
    if (!option) {
        QUANTRA_INVALID_ARGUMENT("PriceEquityOption.option is required");
    }
    if (!option->trade_id()) {
        QUANTRA_INVALID_ARGUMENT("EquityOption.trade_id is required");
    }
    if (!option->underlying_id()) {
        QUANTRA_INVALID_ARGUMENT("EquityOption.underlying_id is required");
    }

    ParsedEquityOption parsed;
    parsed.tradeId = option->trade_id()->str();
    parsed.underlyingId = option->underlying_id()->str();
    parsed.quantity = option->quantity();
    parsed.settlement = option->settlement();

    // --- Exercise style. Presence of a union member decides the style; an
    // unset union (NONE) is a 400, never a silent default.
    switch (option->exercise_type()) {
        case quantra::EquityExercise_EquityEuropeanExercise: {
            const auto* ex = option->exercise_as_EquityEuropeanExercise();
            if (!ex || !ex->expiry_date()) {
                QUANTRA_INVALID_ARGUMENT("EquityEuropeanExercise.expiry_date is required");
            }
            parsed.exercise =
                std::make_shared<QuantLib::EuropeanExercise>(DateToQL(ex->expiry_date()->str()));
            break;
        }
        case quantra::EquityExercise_EquityAmericanExercise: {
            const auto* ex = option->exercise_as_EquityAmericanExercise();
            if (!ex || !ex->end_date()) {
                QUANTRA_INVALID_ARGUMENT("EquityAmericanExercise.end_date is required");
            }
            const QuantLib::Date end = DateToQL(ex->end_date()->str());
            if (ex->start_date() && !ex->start_date()->str().empty()) {
                const QuantLib::Date start = DateToQL(ex->start_date()->str());
                if (start >= end) {
                    QUANTRA_INVALID_ARGUMENT(
                        "EquityAmericanExercise.start_date must be before end_date");
                }
                parsed.exercise = std::make_shared<QuantLib::AmericanExercise>(start, end);
            } else {
                parsed.exercise = std::make_shared<QuantLib::AmericanExercise>(end);
            }
            break;
        }
        case quantra::EquityExercise_EquityBermudanExercise: {
            const auto* ex = option->exercise_as_EquityBermudanExercise();
            if (!ex || !ex->exercise_dates() || ex->exercise_dates()->size() == 0) {
                QUANTRA_INVALID_ARGUMENT(
                    "EquityBermudanExercise.exercise_dates is required and must be non-empty");
            }
            std::vector<QuantLib::Date> dates;
            dates.reserve(ex->exercise_dates()->size());
            for (auto it = ex->exercise_dates()->begin(); it != ex->exercise_dates()->end(); ++it) {
                dates.push_back(DateToQL(it->str()));
            }
            std::sort(dates.begin(), dates.end());
            parsed.exercise = std::make_shared<QuantLib::BermudanExercise>(dates);
            break;
        }
        default:
            QUANTRA_INVALID_ARGUMENT("EquityOption.exercise is required (exercise style is unset)");
    }

    // --- Payoff. Presence of a union member decides the payoff; NONE is a 400.
    switch (option->payoff_type()) {
        case quantra::EquityPayoff_EquityPlainVanillaPayoff: {
            const auto* po = option->payoff_as_EquityPlainVanillaPayoff();
            if (!po) {
                QUANTRA_INVALID_ARGUMENT("EquityPlainVanillaPayoff is required");
            }
            parsed.payoffKind = EquityPayoffKind::PlainVanilla;
            parsed.optionType = EquityOptionTypeToQL(po->option_type());
            parsed.strike = po->strike();
            break;
        }
        case quantra::EquityPayoff_EquityCashOrNothingPayoff: {
            const auto* po = option->payoff_as_EquityCashOrNothingPayoff();
            if (!po) {
                QUANTRA_INVALID_ARGUMENT("EquityCashOrNothingPayoff is required");
            }
            parsed.payoffKind = EquityPayoffKind::CashOrNothing;
            parsed.optionType = EquityOptionTypeToQL(po->option_type());
            parsed.strike = po->strike();
            parsed.cash = po->cash();
            break;
        }
        case quantra::EquityPayoff_EquityAssetOrNothingPayoff: {
            const auto* po = option->payoff_as_EquityAssetOrNothingPayoff();
            if (!po) {
                QUANTRA_INVALID_ARGUMENT("EquityAssetOrNothingPayoff is required");
            }
            parsed.payoffKind = EquityPayoffKind::AssetOrNothing;
            parsed.optionType = EquityOptionTypeToQL(po->option_type());
            parsed.strike = po->strike();
            break;
        }
        default:
            QUANTRA_INVALID_ARGUMENT("EquityOption.payoff is required (payoff is unset)");
    }

    if (option->barrier()) {
        const auto* b = option->barrier();
        if (b->monitoring() != quantra::enums::EquityBarrierMonitoring_Continuous) {
            QUANTRA_INVALID_ARGUMENT("Equity barrier option currently supports only Continuous monitoring");
        }
        parsed.hasBarrier = true;
        parsed.barrierType = EquityBarrierTypeToQL(b->barrier_type());
        parsed.barrierLevel = b->level();
        parsed.rebate = b->rebate();
    }

    return parsed;
}

} // namespace quantra
