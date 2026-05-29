#include "calibrate_swaption_vol_pricer.h"

#include <ql/quantlib.hpp>

#include "error.h"
#include "swaption_vol_runtime.h"

namespace quantra {

CalibrateSwaptionVolResult CalibrateSwaptionVolPricer::price(
    const CalibrateSwaptionVolInputs& inputs,
    const PricingRegistry& reg,
    const PricingContext& ctx) const {
    (void)ctx; // evaluation date already installed during registry build.

    auto vIt = reg.volatility.swaptionVols.find(inputs.volId);
    if (vIt == reg.volatility.swaptionVols.end()) {
        QUANTRA_NOT_FOUND("Swaption vol surface not found: " + inputs.volId);
    }
    const quantra::SwaptionVolEntry& rawEntry = vIt->second;
    if (rawEntry.volKind != quantra::enums::SwaptionVolKind_SabrCalibrate) {
        // Reject all non-calibrate kinds. SabrParams introspection is covered by
        // /sample-vol-surfaces with include_diagnostics=true.
        QUANTRA_INVALID_ARGUMENT(
            "Endpoint /calibrate-swaption-vol only operates on SabrCalibrate "
            "surfaces; vol_id '" + inputs.volId +
            "' is not a SwaptionSabrCalibrateSpec surface");
    }

    auto dIt = reg.rates.curves.find(inputs.discountingCurveId);
    if (dIt == reg.rates.curves.end()) {
        QUANTRA_INVALID_ARGUMENT(
            "Discounting curve not found in pricing.rates.curves: " +
            inputs.discountingCurveId);
    }
    auto fIt = reg.rates.curves.find(inputs.forwardingCurveId);
    if (fIt == reg.rates.curves.end()) {
        QUANTRA_INVALID_ARGUMENT(
            "Forwarding curve not found in pricing.rates.curves: " +
            inputs.forwardingCurveId);
    }

    // Run the existing finalize path. This computes server ATM forwards,
    // calls withSwaptionSabrCalibrateAtm (which consults the SABR cube cache
    // before invoking QuantLib's per-node Levenberg-Marquardt fit), and
    // populates per-node parameters and fit diagnostics on the entry. Same
    // helper that /sample-vol-surfaces and /price-swaption already exercise —
    // calibration logic is not duplicated here.
    quantra::SwaptionVolEntry finalized;
    try {
        finalized = quantra::finalizeSwaptionVolEntryForPricing(
            rawEntry,
            /*trade=*/nullptr,
            reg,
            QuantLib::Handle<QuantLib::YieldTermStructure>(dIt->second->currentLink()),
            QuantLib::Handle<QuantLib::YieldTermStructure>(fIt->second->currentLink()),
            /*forceRecomputeAtm=*/false,
            inputs.discountingCurveId,
            inputs.forwardingCurveId);
    } catch (const QuantraError& e) {
        // Validation/configuration failures (bad inputs, OIS swap index slipped
        // past registry, etc.) are still client errors; calibration runtime
        // failures from QL throw QuantLib::Error and propagate to the
        // CallDataGeneric base catch, surfacing as 500.
        QUANTRA_INVALID_ARGUMENT(
            std::string("SABR finalize failed for vol_id '") + inputs.volId +
            "': " + e.what());
    }

    CalibrateSwaptionVolResult out;
    out.volId = inputs.volId;
    out.finalized = std::move(finalized);
    return out;
}

} // namespace quantra
