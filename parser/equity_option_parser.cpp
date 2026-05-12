#include "equity_option_parser.h"

#include "common.h"
#include "error.h"

namespace quantra {

namespace {

QuantLib::Option::Type toQlOptionType(quantra::enums::EquityOptionType t) {
    return t == quantra::enums::EquityOptionType_Put ? QuantLib::Option::Put : QuantLib::Option::Call;
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
    QUANTRA_INVALID_ARGUMENT("Unsupported EquityBarrierType");
    return QuantLib::Barrier::DownOut;
}

} // namespace

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

    if (option->exercise_type() != quantra::EquityExercise_EquityEuropeanExercise) {
        QUANTRA_INVALID_ARGUMENT("EquityOption currently supports only EquityEuropeanExercise");
    }
    const auto* ex = option->exercise_as_EquityEuropeanExercise();
    if (!ex || !ex->expiry_date()) {
        QUANTRA_INVALID_ARGUMENT("EquityEuropeanExercise.expiry_date is required");
    }

    if (option->payoff_type() != quantra::EquityPayoff_EquityPlainVanillaPayoff) {
        QUANTRA_INVALID_ARGUMENT("EquityOption currently supports only EquityPlainVanillaPayoff");
    }
    const auto* po = option->payoff_as_EquityPlainVanillaPayoff();
    if (!po) {
        QUANTRA_INVALID_ARGUMENT("EquityPlainVanillaPayoff is required");
    }

    ParsedEquityOption parsed;
    parsed.tradeId = option->trade_id()->str();
    parsed.underlyingId = option->underlying_id()->str();
    parsed.quantity = option->quantity();
    parsed.settlement = option->settlement();
    parsed.optionType = toQlOptionType(po->option_type());
    parsed.strike = po->strike();
    parsed.exercise = std::make_shared<QuantLib::EuropeanExercise>(DateToQL(ex->expiry_date()->str()));

    if (option->barrier()) {
        const auto* b = option->barrier();
        if (b->monitoring() != quantra::enums::EquityBarrierMonitoring_Continuous) {
            QUANTRA_INVALID_ARGUMENT("Equity barrier option currently supports only Continuous monitoring");
        }
        parsed.hasBarrier = true;
        parsed.barrierType = toQlBarrierType(b->barrier_type());
        parsed.barrierLevel = b->level();
        parsed.rebate = b->rebate();
    }

    return parsed;
}

} // namespace quantra
