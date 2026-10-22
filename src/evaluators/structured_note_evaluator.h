#ifndef QUANTRA_STRUCTURED_NOTE_EVALUATOR_H
#define QUANTRA_STRUCTURED_NOTE_EVALUATOR_H

/**
 * StructuredNoteEvaluator — pure QuantLib pricing core for structured notes
 * on equity-index baskets (autocallables, Phoenix / Athena coupons, low
 * hurdles, step-downs, geared puts, upside participation): one declarative
 * payoff walk over the note's observation events, priced by Monte Carlo on
 * the shared core in src/mc (basket_model, time_grid, path_engine).
 *
 * INVARIANT: this file (and its .cpp) must NEVER include any *_generated.h or
 * mention FlatBuffers/gRPC namespaces. Suite 0 (scripts/check_evaluator_boundary.sh)
 * enforces this with a literal grep. All wire-format conversion lives in
 * structured_note_mapper.{h,cpp}. The evaluator reads market data exclusively
 * from the registry (reg.rates.curves, reg.equity.equityUnderlyings,
 * reg.volatility.blackVols, reg.fx.spots, reg.correlations).
 *
 * Model. Per underlying k a GeneralizedBlackScholesProcess
 * (spot_k, dividend'_k, r_note, vol_k) where r_note is the note's
 * `discounting_curve` WITHOUT the funding spread (the measure every asset is
 * simulated in) and dividend'_k is the underlying's own dividend curve for a
 * note-currency underlying or, for a quanto underlying, the
 * QuantoTermStructure of src/market/quanto_builder.h (riskFree = r_note,
 * foreign = the quanto block's `discounting_curve` = r_local), so that the
 * drift is r_local - q - rho * sigma_S * sigma_FX. rho is the quanto block's
 * `fx_underlying_correlation` in QuantLib's convention: correlation of the
 * underlying with the FX rate quoted as UNDERLYING-currency units per
 * NOTE-currency unit (CHF per USD for a CHF index in a USD note); the
 * response's model echo reports the resulting drift adjustment. The FX vol
 * is read at the FX spot level, the equity vol at `initial_level`. The
 * processes are correlated with CorrelationRegistry::subMatrix(correlation_id,
 * underlying ids) (StochasticProcessArray, spectral pseudo-square-root).
 *
 * Discounting: every cash flow is discounted on
 * ZeroSpreadedTermStructure(discounting_curve, funding_spread_bp / 1e4) — a
 * flat continuously compounded zero spread on an Actual/365 (Fixed) time axis
 * — at its PAYMENT date (event `payment_date`, or `maturity_payment_date`
 * for the final redemption). The time axis of the simulation is
 * Actual/365 (Fixed) from the valuation date (src/mc/time_grid.h).
 *
 * Grid: the observation dates of the future dated events (accrual-only
 * events are not observed), refined to `steps_per_year` when any underlying's
 * vol is a strike x expiry surface (Dupire local vol, log-Euler steps) or the
 * capital barrier is DailyClose; otherwise the observation dates only (exact
 * lognormal steps). A DailyClose barrier is tested on the basket performance
 * at every grid point after the valuation date (no continuity correction;
 * the barrier is discrete by contract) and `knocked_in` seeds the state.
 *
 * Payoff walk (one path): state {memory (sum of unpaid coupon
 * amounts, fraction of notional), called, knocked_in}. Per event in order:
 * accrual-only: memory += coupon. Otherwise perf = rule(S_k / initial_k)
 * (WorstOf: min, BestOf: max, Average: mean); coupon test (perf >=
 * coupon_trigger, or with an autocall_trigger only the call test itself):
 * earned -> amount = coupon + memory, memory = 0, paid on payment_date unless
 * pay_only_on_call and the event neither calls nor is final (then held:
 * memory = amount); not earned -> memory += coupon when CouponRule.memory
 * (lost otherwise). Call test: perf >= autocall_trigger -> notional plus
 * everything held paid on payment_date, the note ends. Final event, not
 * called: breach = perf < barrier (European) or the DailyClose state;
 * redemption = notional x (1 - participation x max(put_strike - perf, 0))
 * when breached, else notional, plus the `upside` participation when
 * present, on maturity_payment_date. Buckets: bond_redemption (notional
 * returned on a call or at maturity, plus any upside), coupons (memory
 * included), capital_loss (<= 0).
 *
 * Seasoning: events with observation_date <= as_of are resolved from
 * `historical_levels` through the SAME walk before anything is simulated;
 * the resulting state seeds every path. A note the past already decided
 * (Called, or Matured = the final event is past) is not simulated: npv = PV
 * of the known payments dated after as_of (0 when none), paths = 0, no
 * standard error. Coupons earned in the past but paid after as_of (Phoenix
 * with a payment lag) are added to the NPV of an Alive note with zero
 * standard error.
 *
 * Probabilities and expected life come from the same paths: p_call /
 * p_coupon per simulated event, p_barrier_breach, expected_life_years =
 * E[t(end)] with t the Actual/365 (Fixed) time from as_of to the observation
 * date of the call (or the final observation).
 *
 * Standard errors are the path engine's: the sample standard error of the
 * (antithetic) estimator; with Sobol it is an indication, not a confidence
 * bound (path_engine.h).
 *
 * Correlation: CorrelationRegistry::subMatrix(correlation_id, underlying ids)
 * for two or more underlyings; a single underlying is simulated on the 1 x 1
 * unit matrix and `correlation_id` must be absent (400 either way).
 *
 * Diagnostics (StructuredNoteInputs::diagnostics, each block only when its
 * flag is set; the definitions are the fbs doc comments of
 * structured_note_response.fbs): `market` is read off the curves and vols
 * the processes use (forward = spot x D_div(t) / D_note(t) on the grid's
 * time axis, implied yields between observation dates, Black vols at the
 * forward and at barrier x initial, quanto drift in bp at the final
 * observation, discount factors with and without the funding spread, the
 * correlation matrix as used); `decomposition` splits the bucket PVs by
 * event from the main run's accumulators and adds E[loss | breach]
 * (undiscounted); `checks` reports the path engine's running means with
 * their standard errors, walks the payoff once along the forward path
 * (every underlying at its forward on every grid point, zero vol), prices
 * the note on the all-ones and the identity correlation matrices with the
 * same stream (skipped for a single underlying: both equal the headline) and
 * the bond-only value notional x DF_funding(maturity); `distribution`
 * records every individual path (basket performance per observed event,
 * undiscounted and discounted payout, call event, breach) and reports
 * quantiles (linear interpolation between order statistics), a 20-bin
 * histogram of the payout and the first `sample_paths` records; `timing`
 * is wall-clock per stage.
 */

#include "pricing_context.h"
#include "pricing_registry.h"
#include "structured_note_data.h"

namespace quantra {

class StructuredNoteEvaluator {
public:
    NoteResult evaluate(
        const StructuredNoteInputs& inputs,
        const PricingRegistry& reg,
        const PricingContext& ctx) const;
};

} // namespace quantra

#endif // QUANTRA_STRUCTURED_NOTE_EVALUATOR_H
