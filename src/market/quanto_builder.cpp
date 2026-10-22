#include "quanto_builder.h"

#include "error.h"

#include <ql/termstructures/yield/quantotermstructure.hpp>

#include <cmath>
#include <memory>

namespace quantra {

QuantLib::Handle<QuantLib::YieldTermStructure> buildQuantoDividendCurve(
    const QuantLib::Handle<QuantLib::YieldTermStructure>& dividend,
    const QuantLib::Handle<QuantLib::YieldTermStructure>& localRiskFree,
    const QuantLib::Handle<QuantLib::YieldTermStructure>& noteRiskFree,
    const QuantLib::Handle<QuantLib::BlackVolTermStructure>& equityBlackVol,
    const QuantLib::Handle<QuantLib::BlackVolTermStructure>& fxBlackVol,
    QuantLib::Real fxSpot,
    QuantLib::Real rho,
    QuantLib::Real strike) {
    // The handles are server-resolved registry entries; an empty one is a
    // wiring error, not a client input error.
    if (dividend.empty() || localRiskFree.empty() || noteRiskFree.empty() ||
        equityBlackVol.empty() || fxBlackVol.empty()) {
        QUANTRA_ERROR("buildQuantoDividendCurve: every curve and vol handle must be linked");
    }
    if (!std::isfinite(rho) || rho < -1.0 || rho > 1.0) {
        QUANTRA_INVALID_ARGUMENT("quanto fx_underlying_correlation must be in [-1, 1]");
    }
    if (!std::isfinite(fxSpot) || !(fxSpot > 0.0)) {
        QUANTRA_INVALID_ARGUMENT("quanto FX spot must be positive");
    }
    if (!std::isfinite(strike) || !(strike > 0.0)) {
        QUANTRA_INVALID_ARGUMENT("quanto strike (initial level) must be positive");
    }
    // QuantoTermStructure(underlyingDividendTS, riskFreeTS, foreignRiskFreeTS,
    //                     underlyingBlackVolTS, strike,
    //                     exchRateBlackVolTS, exchRateATMlevel, correlation):
    // zero yield = q + r_rf - r_foreign + rho * sigma_S(strike) * sigma_FX(fxSpot).
    // riskFreeTS = note curve, foreignRiskFreeTS = underlying's curve, so a
    // process on the note curve drifts at r_local - q - rho * sigma_S * sigma_FX.
    auto quanto = std::make_shared<QuantLib::QuantoTermStructure>(
        dividend, noteRiskFree, localRiskFree, equityBlackVol, strike, fxBlackVol, fxSpot, rho);
    return QuantLib::Handle<QuantLib::YieldTermStructure>(quanto);
}

} // namespace quantra
