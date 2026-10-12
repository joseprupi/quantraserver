#ifndef QUANTRA_CROSS_CURRENCY_SWAP_EVALUATOR_H
#define QUANTRA_CROSS_CURRENCY_SWAP_EVALUATOR_H

/**
 * CrossCurrencySwapEvaluator — pure QuantLib pricing core for cross-currency
 * swaps: two `SwapLeg`s (fixed, IBOR or overnight coupons) in two currencies,
 * each discounted on its own curve, with its own principal exchanges, paid or
 * received by the reported party, converted to a reporting currency at the
 * as-of exchange rate implied by an FX spot and the two discount curves.
 *
 * INVARIANT: this file (and its .cpp) must NEVER include any *_generated.h or
 * mention FlatBuffers/gRPC namespaces. Suite 0 (scripts/check_evaluator_boundary.sh)
 * enforces this with a literal grep. All wire-format conversion lives in
 * cross_currency_swap_mapper.{h,cpp}. The evaluator reads market data
 * exclusively from the registry (reg.fx.spots, reg.rates.curves,
 * reg.rates.indices).
 *
 * Pricing (QuantLib has no cross-currency swap instrument: leg cash flows and
 * CashFlows::npv / bps only, through swap_leg_pricer.h, the same code the
 * vanilla / OIS / basis swaps value their legs with). For leg i with notional
 * N_i, coupon leg C_i and discounting curve D_i, the leg in its own frame is
 *
 *   L_i = C_i  +  [initial_exchange] SimpleCashFlow(-N_i, schedule start)
 *              +  [final_exchange]   SimpleCashFlow(+N_i, last coupon payment date)
 *
 * i.e. the party that receives the coupons pays the notional at the start and
 * receives it at the end. With s_i = -1 for a paid leg (`Pay`), +1 for a
 * received one (`Receive`):
 *
 *   legs[i].npv           = s_i * CashFlows::npv(L_i, D_i)        [currency i]
 *   X                     = S * DF_quote(t_s) / DF_base(t_s)      [FxAsOfExchangeRate]
 *   legs[i].npv_reporting = legs[i].npv converted with X (or 1/X) [reporting ccy]
 *   npv                   = legs[0].npv_reporting + legs[1].npv_reporting
 *
 * where DF_base / DF_quote are the discounting curves of the legs whose
 * currency is the spot's base / quote currency. Fair quantities hold the
 * other leg and every exchange fixed; with the annuity of leg i's coupons
 * A_i = CashFlows::bps(C_i, D_i) / 1e-4 (exchanges excluded), the NPV is
 * linear in the fixed rate r_f / the spread q_1, so
 *
 *   fair_rate   = r_f - NPV[fixed-leg ccy] / (s_f * A_f)   (exactly one fixed leg)
 *   fair_spread = q_1 - NPV[legs[1] ccy]   / (s_1 * A_1)   (no fixed leg)
 *
 * and repricing at the fair value gives npv = 0 identically (the tests pin
 * it through the API). These are QuantLib::VanillaSwap's /
 * OvernightIndexedSwap's own fair-value formulas.
 */

#include "pricing_context.h"
#include "pricing_registry.h"
#include "swap_leg_data.h"
#include "swap_leg_pricer.h"

#include <optional>
#include <string>
#include <vector>

namespace quantra {

/// One swap: the two legs in request order (leg1, leg2), in two different
/// currencies, one paid and one received (the mapper enforces all of it).
struct CrossCurrencySwapTrade {
    std::vector<SwapLegData> legs;
    std::string fxSpotId;
    /// leg1 or leg2 currency (the mapper rejects anything else).
    std::string reportingCurrency;
};

struct CrossCurrencySwapInputs {
    std::vector<CrossCurrencySwapTrade> trades;
    bool includeFlows = false;
};

/// Per-swap result. Mirrors the CrossCurrencySwapResponse FB table.
struct CrossCurrencySwapPerSwap {
    double npv = 0.0;
    std::vector<SwapLegPerLeg> legs;
    /// Present iff exactly one leg is fixed.
    std::optional<double> fairRate;
    /// Present iff no leg is fixed (spread on legs[1]).
    std::optional<double> fairSpread;
    double fxSpotUsed = 0.0;
    double fxAsOfRate = 0.0;
    std::string spotDate;
    bool includeFlows = false;
};

struct CrossCurrencySwapResult {
    std::vector<CrossCurrencySwapPerSwap> swaps;
};

class CrossCurrencySwapEvaluator {
public:
    CrossCurrencySwapResult evaluate(
        const CrossCurrencySwapInputs& inputs,
        const PricingRegistry& reg,
        const PricingContext& ctx) const;
};

} // namespace quantra

#endif // QUANTRA_CROSS_CURRENCY_SWAP_EVALUATOR_H
