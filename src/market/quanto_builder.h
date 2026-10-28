#ifndef QUANTRA_QUANTO_BUILDER_H
#define QUANTRA_QUANTO_BUILDER_H

/**
 * Quanto dividend curve for an underlying that is simulated in its own
 * currency but pays into another (a CHF index inside a USD note).
 *
 * Built on QuantLib::QuantoTermStructure (ql/termstructures/yield/
 * quantotermstructure.hpp), whose continuously compounded zero yield at
 * time t is
 *
 *     q_quanto(t) = q(t) + r_rf(t) - r_foreign(t) + rho * sigma_S(t, strike) * sigma_FX(t, fxSpot)
 *
 * with the constructor arguments (underlyingDividendTS = q, riskFreeTS = r_rf,
 * foreignRiskFreeTS = r_foreign). This builder wires
 *
 *     riskFreeTS        = noteRiskFree   (the note / payoff currency curve)
 *     foreignRiskFreeTS = localRiskFree  (the underlying's own currency curve)
 *
 * exactly as QuantLib's QuantoEngine does, so that a
 * GeneralizedBlackScholesProcess built with `riskFreeRate = noteRiskFree` and
 * `dividendYield = <this curve>` has the risk-neutral drift, in the note
 * currency's measure,
 *
 *     mu = r_note - q_quanto = r_local - q - rho * sigma_S * sigma_FX
 *
 * i.e. the underlying's own forward drift (r_local - q) minus the quanto
 * adjustment. With flat curves and vols the quanto forward at T is
 * S * exp((r_local - q - rho * sigma_S * sigma_FX) * T), and rho = 0 gives back
 * the plain forward in the underlying's own currency; r_note cancels out of
 * the drift and is only the discount curve. The process MUST be built with
 * noteRiskFree as its risk-free curve: with any other curve the r_rf term
 * does not cancel and the drift is wrong.
 *
 * Correlation sign: rho is the correlation between the underlying's returns
 * and the returns of the exchange rate X quoted as NOTE-currency units per one
 * UNDERLYING-currency unit (USD per CHF for a CHF index in a USD note). In the
 * note measure S * X (the underlying's value in the note currency, yield q)
 * drifts at r_note - q and X at r_note - r_local, so S drifts at
 * r_local - q - rho * sigma_S * sigma_FX: positive rho lowers the drift. rho
 * is passed to QuantoTermStructure unchanged; a correlation measured against
 * the inverse quote (CHF per USD) must come in with its sign flipped.
 *
 * Vol arguments: QuantoTermStructure reads sigma_S at `strike` and sigma_FX at
 * `fxSpot` for every t, so with a smile the adjustment is the one at those
 * levels. The structured-note evaluator passes the note's initial level
 * (strike fixing) as `strike`: the autocall and barrier tests are all struck
 * relative to it, so it is the level the quanto drift must be right at. The
 * FX vol is read at the FX spot (ATM) as quoted, never inverted; the
 * evaluator only accepts a strike-independent FX vol, so that level does not
 * change the result.
 *
 * The result stays linked to its inputs (handles, not copies): a bumped spot,
 * vol or curve flows through for bump-and-reprice Greeks.
 */

#include <ql/handle.hpp>
#include <ql/termstructures/volatility/equityfx/blackvoltermstructure.hpp>
#include <ql/termstructures/yieldtermstructure.hpp>
#include <ql/types.hpp>

namespace quantra {

/**
 * @param dividend        underlying's dividend-yield curve (its own currency)
 * @param localRiskFree   risk-free curve of the underlying's currency (r_local)
 * @param noteRiskFree    risk-free curve of the note / payoff currency (r_note);
 *                        the process must use this same handle as riskFreeRate
 * @param equityBlackVol  underlying's Black vol surface (read at `strike`)
 * @param fxBlackVol      Black vol of the exchange rate (read at `fxSpot`)
 * @param fxSpot          exchange-rate level the FX vol is read at (ATM), as quoted
 * @param rho             correlation of the underlying with the note-per-underlying
 *                        exchange rate (see sign above)
 * @param strike          underlying level the equity vol is read at (the note's
 *                        initial level)
 * @return a yield curve to use as the process's dividend yield
 */
QuantLib::Handle<QuantLib::YieldTermStructure> buildQuantoDividendCurve(
    const QuantLib::Handle<QuantLib::YieldTermStructure>& dividend,
    const QuantLib::Handle<QuantLib::YieldTermStructure>& localRiskFree,
    const QuantLib::Handle<QuantLib::YieldTermStructure>& noteRiskFree,
    const QuantLib::Handle<QuantLib::BlackVolTermStructure>& equityBlackVol,
    const QuantLib::Handle<QuantLib::BlackVolTermStructure>& fxBlackVol,
    QuantLib::Real fxSpot,
    QuantLib::Real rho,
    QuantLib::Real strike);

} // namespace quantra

#endif // QUANTRA_QUANTO_BUILDER_H
