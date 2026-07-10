#ifndef QUANTRA_EQUITY_OPTION_EVALUATOR_H
#define QUANTRA_EQUITY_OPTION_EVALUATOR_H

/**
 * EquityOptionEvaluator — pure QuantLib pricing core for equity vanilla and
 * barrier options.
 *
 * INVARIANT: this file (and its .cpp) must NEVER include any *_generated.h or
 * mention FlatBuffers/gRPC namespaces. Suite 0 (scripts/check_evaluator_boundary.sh)
 * enforces this with a literal grep. All wire-format conversion lives in
 * equity_option_mapper.{h,cpp}. The evaluator reads market data exclusively from
 * the plain-domain registry fields (reg.rates.curves, reg.equity.equityUnderlyings,
 * reg.volatility.blackVols, reg.volatility.modelDomains).
 */

#include <memory>
#include <string>
#include <vector>

#include <ql/exercise.hpp>
#include <ql/instruments/barrieroption.hpp>
#include <ql/instruments/swaption.hpp>
#include <ql/option.hpp>

#include "pricing_context.h"
#include "pricing_registry.h"

namespace quantra {

/// Which QuantLib payoff the trade carries. Mirrors the EquityPayoff union on
/// the wire (plain vanilla vs the two digital payoffs). The exercise style is
/// not duplicated here — the evaluator reads it back from the QL exercise
/// object's type() (European / American / Bermudan).
enum class EquityPayoffKind {
    PlainVanilla,
    CashOrNothing,
    AssetOrNothing,
};

/// One equity option trade lifted out of the FB request by the mapper. The
/// mapper has already converted enums to QL types and built the QL exercise
/// object; the evaluator never touches FlatBuffers.
struct EquityOptionTrade {
    std::string tradeId;
    double quantity = 1.0;
    QuantLib::Settlement::Type settlement = QuantLib::Settlement::Physical;

    QuantLib::Option::Type optionType = QuantLib::Option::Call;
    double strike = 0.0;
    EquityPayoffKind payoffKind = EquityPayoffKind::PlainVanilla;
    double cash = 0.0;  // cash amount for a cash-or-nothing digital payoff
    std::shared_ptr<QuantLib::Exercise> exercise;

    bool hasBarrier = false;
    QuantLib::Barrier::Type barrierType = QuantLib::Barrier::DownOut;
    double barrierLevel = 0.0;
    double rebate = 0.0;

    std::string discountingCurveId;
    std::string underlyingId;
    std::string volatilityId;
    std::string modelId;
};

struct EquityOptionInputs {
    std::vector<EquityOptionTrade> trades;
};

/// Per-trade pricing result. Mirrors the EquityOptionResponse FB schema. Greek
/// fields default to NaN so the mapper can echo them unchanged when QL refuses
/// to compute one (e.g. American/barrier paths that don't expose vega/rho).
struct EquityOptionPerTrade {
    std::string tradeId;
    double npv = 0.0;
    double delta = 0.0;
    double gamma = 0.0;
    double vega = 0.0;
    double theta = 0.0;
    double rho = 0.0;
    double impliedVolatility = 0.0;
    double usedSpot = 0.0;
    double usedStrike = 0.0;
    QuantLib::Settlement::Type usedSettlement = QuantLib::Settlement::Physical;
};

struct EquityOptionResult {
    std::vector<EquityOptionPerTrade> trades;
};

class EquityOptionEvaluator {
public:
    EquityOptionResult evaluate(const EquityOptionInputs& inputs,
                             const PricingRegistry& reg,
                             const PricingContext& ctx) const;
};

} // namespace quantra

#endif // QUANTRA_EQUITY_OPTION_EVALUATOR_H
