#ifndef QUANTRA_SSVI_VOL_SURFACE_H
#define QUANTRA_SSVI_VOL_SURFACE_H

/**
 * QuantLib term-structure wrappers around the eSSVI math in ssvi.h.
 *
 * SsviBlackVol is a BlackVarianceTermStructure whose total variance at expiry t
 * and strike K is
 *
 *     w(node(t), ln(K / F_ref(t))),     F_ref(t) = spot0 D_q(t) / D_r(t),
 *
 * with D_q / D_r the dividend / risk-free discount factors of the reference
 * curves. spot0 is a snapshot double, not a Quote: the surface is sticky-strike,
 * a later bump of the process spot or rates does not move it. Slice times are
 * T_i = dayCounter.yearFraction(referenceDate, expiry_i). The parametrization
 * extrapolates analytically in t and K, so maxDate() is Date::maxDate(),
 * minStrike() is QL_EPSILON and maxStrike() is QL_MAX_REAL.
 *
 * SsviLocalVol is the analytic Dupire local volatility of that surface for a
 * process market (spot, dividend, riskFree) that may differ from the reference
 * one:
 *
 *     k = ln(S / F_ref(t)),  D(t) = ln(F_proc(t) / F_ref(t)),  y = k - D(t)
 *     dDrift = (f_r,proc - f_q,proc)(t) - (f_r,ref - f_q,ref)(t)
 *     sigma_loc^2 = (w_t + w_k dDrift) / g(k, y)
 *
 * with f the instantaneous forward rates (see ssvi.h for the derivation). When
 * the process market is the reference one, D = 0, dDrift = 0 and y = k exactly.
 * It throws QuantLib::Error when g <= 0 or the numerator is negative (the
 * surface admits arbitrage there), as QuantLib's LocalVolSurface does for a
 * negative local variance; the structured-note pricer reads it through
 * mc::EdgeClampedLocalVol (src/mc/basket_model.h), which applies the strike
 * edges, the FlatLocalVol time clamp and the optional floor to it as to the
 * finite-difference local vol of the other surfaces.
 * It registers with the process handles and the Black surface so a relink or
 * bump notifies observers like any QuantLib term structure.
 */

#include "ssvi.h"

#include <ql/handle.hpp>
#include <ql/quote.hpp>
#include <ql/termstructures/volatility/equityfx/blackvoltermstructure.hpp>
#include <ql/termstructures/volatility/equityfx/localvoltermstructure.hpp>
#include <ql/termstructures/yieldtermstructure.hpp>
#include <ql/time/calendar.hpp>
#include <ql/time/date.hpp>
#include <ql/time/daycounter.hpp>
#include <ql/types.hpp>

#include <memory>
#include <vector>

namespace quantra {

struct SsviSliceParams {
    double theta; // ATM total implied variance
    double rho;
    double psi; // theta * phi
};

/**
 * Implied dividend discount curve for a surface whose reference forwards are given at its
 * expiries: Dq(T_i) = F_i Dr(T_i) / S0 with Dr(T_i) = riskFree->discount(T_i, true) and T_i =
 * dayCounter.yearFraction(referenceDate, expiry_i), Dq(0) = 1, log-linear in the discount factor
 * between the nodes and flat-forward beyond the last one. SsviBlackVol built on it has
 * forward(T_i) == F_i exactly (up to rounding). Throws QUANTRA_INVALID_ARGUMENT for a
 * non-positive forward or spot, unsorted expiries, or mismatched sizes.
 */
QuantLib::Handle<QuantLib::YieldTermStructure> buildImpliedDividendCurve(
    const QuantLib::Date& referenceDate,
    const QuantLib::DayCounter& dayCounter,
    const std::vector<QuantLib::Date>& expiries,
    const std::vector<double>& forwards,
    double spot0,
    const QuantLib::Handle<QuantLib::YieldTermStructure>& riskFree);

/**
 * The self-contained surface of an `SsviVolSpec` payload: the reference forward is
 * ln F_ref linear in time through ln spot0 at the reference date and ln forwards[i] at
 * expiries[i], continuing the last slope beyond (SsviBlackVol on a zero-rate FlatForward
 * risk-free curve and the implied dividend curve of buildImpliedDividendCurve on it), so that
 * F_ref(T_i) == forwards[i] and no curve of the request is needed. Throws
 * QUANTRA_INVALID_ARGUMENT as the pieces do.
 */
std::shared_ptr<class SsviBlackVol> makeSsviSurface(
    const QuantLib::Date& referenceDate,
    const QuantLib::Calendar& calendar,
    QuantLib::BusinessDayConvention bdc,
    const QuantLib::DayCounter& dayCounter,
    const std::vector<QuantLib::Date>& expiries,
    const std::vector<SsviSliceParams>& params,
    const std::vector<double>& forwards,
    double spot0);

class SsviBlackVol : public QuantLib::BlackVarianceTermStructure {
public:
    SsviBlackVol(
        const QuantLib::Date& referenceDate,
        const QuantLib::Calendar& calendar,
        QuantLib::BusinessDayConvention bdc,
        const QuantLib::DayCounter& dayCounter,
        const std::vector<QuantLib::Date>& expiries,
        const std::vector<SsviSliceParams>& params,
        double spot0,
        QuantLib::Handle<QuantLib::YieldTermStructure> dividend,
        QuantLib::Handle<QuantLib::YieldTermStructure> riskFree);

    QuantLib::Date maxDate() const override;
    QuantLib::Real minStrike() const override;
    QuantLib::Real maxStrike() const override;

    /// Year fraction of the last slice (the surface itself has no last date).
    QuantLib::Time lastSliceTime() const { return surface_.slices().back().t; }
    const std::vector<QuantLib::Date>& expiries() const { return expiries_; }
    const ssvi::Surface& surface() const { return surface_; }
    double spot0() const { return spot0_; }
    const QuantLib::Handle<QuantLib::YieldTermStructure>& dividend() const { return dividend_; }
    const QuantLib::Handle<QuantLib::YieldTermStructure>& riskFree() const { return riskFree_; }

    // Reference forward spot0 D_q(t) / D_r(t).
    QuantLib::Real forward(QuantLib::Time t) const;

protected:
    QuantLib::Real blackVarianceImpl(QuantLib::Time t, QuantLib::Real strike) const override;

private:
    ssvi::Surface surface_;
    std::vector<QuantLib::Date> expiries_;
    double spot0_;
    QuantLib::Handle<QuantLib::YieldTermStructure> dividend_;
    QuantLib::Handle<QuantLib::YieldTermStructure> riskFree_;
};

class SsviLocalVol : public QuantLib::LocalVolTermStructure {
public:
    SsviLocalVol(
        std::shared_ptr<SsviBlackVol> blackVol,
        QuantLib::Handle<QuantLib::Quote> spot,
        QuantLib::Handle<QuantLib::YieldTermStructure> dividend,
        QuantLib::Handle<QuantLib::YieldTermStructure> riskFree);

    QuantLib::Date maxDate() const override;
    QuantLib::Real minStrike() const override;
    QuantLib::Real maxStrike() const override;

    const std::shared_ptr<SsviBlackVol>& blackVol() const { return blackVol_; }

protected:
    QuantLib::Volatility localVolImpl(QuantLib::Time t, QuantLib::Real strike) const override;

private:
    std::shared_ptr<SsviBlackVol> blackVol_;
    QuantLib::Handle<QuantLib::Quote> spot_;
    QuantLib::Handle<QuantLib::YieldTermStructure> dividend_;
    QuantLib::Handle<QuantLib::YieldTermStructure> riskFree_;
};

} // namespace quantra

#endif // QUANTRA_SSVI_VOL_SURFACE_H
