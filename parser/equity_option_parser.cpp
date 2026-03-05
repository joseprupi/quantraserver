#include "equity_option_parser.h"

#include "common.h"
#include "error.h"

namespace quantra {

namespace {

QuantLib::Option::Type toQlOptionType(quantra::enums::EquityOptionType t) {
    return t == quantra::enums::EquityOptionType_Put ? QuantLib::Option::Put : QuantLib::Option::Call;
}

std::shared_ptr<QuantLib::Exercise> parseExercise(const quantra::EquityOption* option) {
    if (!option) {
        QUANTRA_ERROR("PriceEquityOption.option is required");
    }
    switch (option->exercise_type()) {
        case quantra::EquityExercise_EquityEuropeanExercise: {
            const auto* ex = option->exercise_as_EquityEuropeanExercise();
            if (!ex || !ex->expiry_date()) {
                QUANTRA_ERROR("EquityEuropeanExercise.expiry_date is required");
            }
            return std::make_shared<QuantLib::EuropeanExercise>(DateToQL(ex->expiry_date()->str()));
        }
        case quantra::EquityExercise_EquityAmericanExercise: {
            const auto* ex = option->exercise_as_EquityAmericanExercise();
            if (!ex || !ex->start_date() || !ex->end_date()) {
                QUANTRA_ERROR("EquityAmericanExercise.start_date and end_date are required");
            }
            return std::make_shared<QuantLib::AmericanExercise>(
                DateToQL(ex->start_date()->str()),
                DateToQL(ex->end_date()->str()));
        }
        case quantra::EquityExercise_EquityBermudanExercise: {
            const auto* ex = option->exercise_as_EquityBermudanExercise();
            if (!ex || !ex->exercise_dates()) {
                QUANTRA_ERROR("EquityBermudanExercise.exercise_dates is required");
            }
            std::vector<QuantLib::Date> dates;
            dates.reserve(ex->exercise_dates()->size());
            for (const auto* d : *ex->exercise_dates()) {
                if (!d) continue;
                dates.push_back(DateToQL(d->str()));
            }
            if (dates.empty()) {
                QUANTRA_ERROR("EquityBermudanExercise.exercise_dates must not be empty");
            }
            return std::make_shared<QuantLib::BermudanExercise>(dates);
        }
        default:
            QUANTRA_ERROR("Unsupported EquityExercise type");
            return nullptr;
    }
}

std::shared_ptr<QuantLib::Payoff> parsePayoff(const quantra::EquityOption* option, ParsedEquityOption& parsed) {
    if (!option) {
        QUANTRA_ERROR("PriceEquityOption.option is required");
    }
    switch (option->payoff_type()) {
        case quantra::EquityPayoff_EquityPlainVanillaPayoff: {
            const auto* po = option->payoff_as_EquityPlainVanillaPayoff();
            if (!po) {
                QUANTRA_ERROR("EquityPlainVanillaPayoff is required");
            }
            parsed.optionType = toQlOptionType(po->option_type());
            parsed.strike = po->strike();
            return std::make_shared<QuantLib::PlainVanillaPayoff>(parsed.optionType, parsed.strike);
        }
        case quantra::EquityPayoff_EquityCashOrNothingPayoff: {
            const auto* po = option->payoff_as_EquityCashOrNothingPayoff();
            if (!po) {
                QUANTRA_ERROR("EquityCashOrNothingPayoff is required");
            }
            parsed.optionType = toQlOptionType(po->option_type());
            parsed.strike = po->strike();
            return std::make_shared<QuantLib::CashOrNothingPayoff>(parsed.optionType, parsed.strike, po->cash());
        }
        case quantra::EquityPayoff_EquityAssetOrNothingPayoff: {
            const auto* po = option->payoff_as_EquityAssetOrNothingPayoff();
            if (!po) {
                QUANTRA_ERROR("EquityAssetOrNothingPayoff is required");
            }
            parsed.optionType = toQlOptionType(po->option_type());
            parsed.strike = po->strike();
            return std::make_shared<QuantLib::AssetOrNothingPayoff>(parsed.optionType, parsed.strike);
        }
        default:
            QUANTRA_ERROR("Unsupported EquityPayoff type");
            return nullptr;
    }
}

QuantLib::Barrier::Type toQlBarrierType(quantra::enums::EquityBarrierType t) {
    switch (t) {
        case quantra::enums::EquityBarrierType_DownIn:
            return QuantLib::Barrier::DownIn;
        case quantra::enums::EquityBarrierType_UpIn:
            return QuantLib::Barrier::UpIn;
        case quantra::enums::EquityBarrierType_DownOut:
            return QuantLib::Barrier::DownOut;
        case quantra::enums::EquityBarrierType_UpOut:
            return QuantLib::Barrier::UpOut;
    }
    QUANTRA_ERROR("Unsupported EquityBarrierType");
    return QuantLib::Barrier::DownOut;
}

QuantLib::Average::Type toQlAverageType(quantra::enums::EquityAsianAverageType t) {
    switch (t) {
        case quantra::enums::EquityAsianAverageType_Geometric:
            return QuantLib::Average::Geometric;
        case quantra::enums::EquityAsianAverageType_Arithmetic:
            return QuantLib::Average::Arithmetic;
    }
    QUANTRA_ERROR("Unsupported EquityAsianAverageType");
    return QuantLib::Average::Geometric;
}

} // namespace

ParsedEquityOption EquityOptionParser::parse(const quantra::EquityOption* option) const {
    if (!option) {
        QUANTRA_ERROR("PriceEquityOption.option is required");
    }
    if (!option->trade_id()) {
        QUANTRA_ERROR("EquityOption.trade_id is required");
    }
    if (!option->underlying_id()) {
        QUANTRA_ERROR("EquityOption.underlying_id is required");
    }

    ParsedEquityOption parsed;
    parsed.tradeId = option->trade_id()->str();
    parsed.underlyingId = option->underlying_id()->str();
    parsed.quantity = option->quantity();
    parsed.settlement = option->settlement();
    parsed.exercise = parseExercise(option);
    parsed.payoff = parsePayoff(option, parsed);

    if (option->barrier()) {
        const auto* b = option->barrier();
        if (b->monitoring() != quantra::enums::EquityBarrierMonitoring_Continuous) {
            QUANTRA_ERROR("Equity barrier option currently supports only Continuous monitoring");
        }
        parsed.hasBarrier = true;
        parsed.barrierType = toQlBarrierType(b->barrier_type());
        parsed.barrierLevel = b->level();
        parsed.rebate = b->rebate();
    }

    if (option->asian()) {
        const auto* a = option->asian();
        if (!a->fixing_dates() || a->fixing_dates()->empty()) {
            QUANTRA_ERROR("EquityAsianFeature.fixing_dates must not be empty");
        }
        parsed.hasAsian = true;
        parsed.asianAverageType = toQlAverageType(a->average_type());
        parsed.asianRunningAccumulator = a->running_accumulator();
        parsed.asianPastFixings = a->past_fixings();
        if (parsed.asianPastFixings < 0) {
            QUANTRA_ERROR("EquityAsianFeature.past_fixings must be >= 0");
        }
        parsed.asianFixingDates.reserve(a->fixing_dates()->size());
        for (const auto* d : *a->fixing_dates()) {
            if (!d) continue;
            parsed.asianFixingDates.push_back(DateToQL(d->str()));
        }
        if (parsed.asianFixingDates.empty()) {
            QUANTRA_ERROR("EquityAsianFeature.fixing_dates must contain valid dates");
        }
    }

    if (parsed.hasBarrier && parsed.hasAsian) {
        QUANTRA_ERROR("Combining EquityBarrierFeature and EquityAsianFeature is not supported");
    }

    return parsed;
}

} // namespace quantra
