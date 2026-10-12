#ifndef QUANTRA_CROSS_CURRENCY_SWAP_EVALUATOR_H
#define QUANTRA_CROSS_CURRENCY_SWAP_EVALUATOR_H

/**
 * CrossCurrencySwapEvaluator — pure QuantLib pricing core for cross-currency
 * swaps: two coupon legs (fixed, IBOR or compounded overnight) in two
 * currencies, each with its own discounting curve and its own principal
 * exchanges, converted to a reporting currency at the as-of exchange rate
 * implied by an FX spot and the two discount curves.
 *
 * INVARIANT: this file (and its .cpp) must NEVER include any *_generated.h or
 * mention FlatBuffers/gRPC namespaces. Suite 0 (scripts/check_evaluator_boundary.sh)
 * enforces this with a literal grep. All wire-format conversion lives in
 * cross_currency_swap_mapper.{h,cpp}. The evaluator reads market data
 * exclusively from the registry (reg.fx.spots, reg.rates.curves,
 * reg.rates.indices).
 *
 * Pricing (QuantLib has no cross-currency swap instrument: leg cash flows and
 * CashFlows::npv / bps only). For leg i with notional N_i, coupon leg C_i
 * (built by swap_leg_builder.h exactly as the vanilla / basis swaps build
 * theirs) and discounting curve D_i, the leg in its own frame is
 *
 *   L_i = C_i  +  [initial_exchange] SimpleCashFlow(-N_i, schedule start)
 *              +  [final_exchange]   SimpleCashFlow(+N_i, last coupon payment date)
 *
 * i.e. the party that receives the coupons pays the notional at the start and
 * receives it at the end. With s_1 = -1 for Payer (the reported party pays
 * leg1), +1 for Receiver, and s_2 = -s_1:
 *
 *   leg_i_npv           = s_i * CashFlows::npv(L_i, D_i)          [currency i]
 *   X                   = S * DF_quote(t_s) / DF_base(t_s)        [FxAsOfExchangeRate]
 *   leg_i_npv_reporting = leg_i_npv converted with X (or 1/X)     [reporting ccy]
 *   npv                 = leg1_npv_reporting + leg2_npv_reporting
 *
 * where DF_base / DF_quote are the discounting curves of the legs whose
 * currency is the spot's base / quote currency. Fair quantities hold the
 * other leg and every exchange fixed; with the annuity of leg i's coupons
 * A_i = CashFlows::bps(C_i, D_i) / 1e-4 (exchanges excluded), the NPV is
 * linear in the fixed rate r_f / the spread q_2, so
 *
 *   fair_rate   = r_f - NPV[fixed-leg ccy] / (s_f * A_f)   (exactly one fixed leg)
 *   fair_spread = q_2 - NPV[leg-2 ccy]     / (s_2 * A_2)   (both legs floating)
 *
 * and repricing at the fair value gives npv = 0 identically (the tests pin
 * it through the API).
 */

#include "pricing_context.h"
#include "pricing_registry.h"
#include "swap_leg_builder.h"

#include <ql/instruments/vanillaswap.hpp>

#include <optional>
#include <string>
#include <vector>

namespace quantra {

/// One leg of a cross-currency swap lifted out of the FB request by the
/// mapper (ids resolved against the registry at evaluation time).
struct XccyLegData {
    enum class Kind { Fixed, Floating };
    Kind kind = Kind::Fixed;
    std::string currency;
    double notional = 0.0;
    /// Populated when kind == Fixed.
    FixedRateLegSpec fixed;
    /// Populated when kind == Floating, together with `indexId`.
    FloatingLegSpec floating;
    std::string indexId;
    bool initialExchange = false;
    bool finalExchange = false;
    std::string discountingCurveId;
    /// Empty for a fixed leg (the mapper rejects a present value there).
    std::string forwardingCurveId;
};

/// One swap. `swapType` follows the basis-swap convention: Payer = the
/// reported party pays leg1 and receives leg2.
struct CrossCurrencySwapTrade {
    QuantLib::VanillaSwap::Type swapType = QuantLib::VanillaSwap::Payer;
    XccyLegData leg1;
    XccyLegData leg2;
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
    double leg1Npv = 0.0;
    double leg2Npv = 0.0;
    double leg1NpvReporting = 0.0;
    double leg2NpvReporting = 0.0;
    /// Present iff exactly one leg is fixed.
    std::optional<double> fairRate;
    /// Present iff both legs float (spread on leg2).
    std::optional<double> fairSpread;
    double fxSpotUsed = 0.0;
    double fxAsOfRate = 0.0;
    std::string spotDate;
    bool includeFlows = false;
    std::vector<SwapLegFlowPlain> leg1Flows;
    std::vector<SwapLegFlowPlain> leg2Flows;
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
