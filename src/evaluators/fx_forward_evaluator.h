#ifndef QUANTRA_FX_FORWARD_EVALUATOR_H
#define QUANTRA_FX_FORWARD_EVALUATOR_H

/**
 * FxForwardEvaluator — pure QuantLib pricing core for outright FX forwards
 * (one base-currency notional against a fixed quote-currency amount at a
 * single delivery date, priced off an FX spot and one discount curve per
 * currency).
 *
 * INVARIANT: this file (and its .cpp) must NEVER include any *_generated.h or
 * mention FlatBuffers/gRPC namespaces. Suite 0 (scripts/check_evaluator_boundary.sh)
 * enforces this with a literal grep. All wire-format conversion lives in
 * fx_forward_mapper.{h,cpp}. The evaluator reads market data exclusively from
 * the registry (reg.fx.spots, reg.rates.curves).
 *
 * Pricing (no instrument class in QuantLib: discount-factor algebra only).
 * With S the spot rate (delivery on the spot date t_s), T the adjusted
 * maturity, N the base notional, K the contract rate, sign = +1 for BuyBase
 * and -1 for SellBase, DF_b / DF_q the two discount curves:
 *
 *   base_leg_pv  = sign * N * DF_b(T)                    [base currency]
 *   quote_leg_pv = -sign * N * K * DF_q(T)               [quote currency]
 *   X            = S * DF_q(t_s) / DF_b(t_s)             [quote per base, as of today]
 *   npv          = base_leg_pv * X + quote_leg_pv        [reporting = quote]
 *                = base_leg_pv + quote_leg_pv / X        [reporting = base]
 *   fair         = X * DF_b(T) / DF_q(T)
 *                = S * [DF_b(T) / DF_b(t_s)] / [DF_q(T) / DF_q(t_s)]
 *   forward_points = fair - S
 *
 * X is the as-of exchange rate implied by the spot and the two curves: one
 * base unit delivered at t_s is worth DF_b(t_s) base units today and S
 * quote units at t_s, i.e. S * DF_q(t_s) quote units today. It is the same
 * algebra QuantLib's FxSwapRateHelper rests on, so a forward whose maturity
 * is an FX swap pillar of the base curve reprices that pillar's
 * spot + points as its fair rate. Substituting K = fair into npv gives 0
 * identically, which the tests pin.
 */

#include "enums_domain.h"
#include "pricing_context.h"
#include "pricing_registry.h"

#include <ql/time/date.hpp>

#include <string>
#include <vector>

namespace quantra {

/// One FX forward lifted out of the FB request by the mapper. Market data is
/// referenced by id and resolved against the registry at evaluation time.
struct FxForwardTrade {
    std::string baseCurrency;
    std::string quoteCurrency;
    double baseNotional = 0.0;
    /// Contract rate, quote units per base unit.
    double forwardRate = 0.0;
    /// Unadjusted delivery date as given; the evaluator adjusts it with the
    /// spot's settlement calendar and convention.
    QuantLib::Date maturityDate;
    FxDirectionKind direction = FxDirectionKind::BuyBase;
    std::string fxSpotId;
    std::string baseDiscountingCurveId;
    std::string quoteDiscountingCurveId;
    /// Base or quote currency of the trade (the mapper rejects anything else).
    std::string reportingCurrency;
};

struct FxForwardInputs {
    std::vector<FxForwardTrade> trades;
};

/// Per-forward result. Mirrors the FxForwardResponse FB table.
struct FxForwardPerTrade {
    double npv = 0.0;
    double fairForwardRate = 0.0;
    double forwardPoints = 0.0;
    double baseLegPv = 0.0;
    double quoteLegPv = 0.0;
    std::string spotDate;
    std::string maturityDate;
    double fxSpotUsed = 0.0;
};

struct FxForwardResult {
    std::vector<FxForwardPerTrade> trades;
};

class FxForwardEvaluator {
public:
    FxForwardResult evaluate(
        const FxForwardInputs& inputs, const PricingRegistry& reg, const PricingContext& ctx) const;
};

/// Spot date of an FX spot for the given evaluation date: the date is first
/// adjusted on the joint settlement calendar, then advanced `spotDays`
/// business days (QuantLib FxSwapRateHelper::initializeDates does exactly
/// this). Shared with the cross-currency products.
QuantLib::Date FxSpotDate(const FxSpot& spot, const QuantLib::Date& asOf);

/// As-of exchange rate X = S * DF_q(t_s) / DF_b(t_s) (quote units per base
/// unit at the curves' reference date) for a spot delivered at t_s, with the
/// two currencies' discount curves. Shared with the cross-currency products,
/// whose leg NPVs are converted to the reporting currency with the same X.
double FxAsOfExchangeRate(
    double spotRate,
    const QuantLib::Date& spotDate,
    const QuantLib::YieldTermStructure& baseDiscount,
    const QuantLib::YieldTermStructure& quoteDiscount);

} // namespace quantra

#endif // QUANTRA_FX_FORWARD_EVALUATOR_H
