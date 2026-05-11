#include "calibrate_swaption_vol_pricing_request.h"

#include <ql/quantlib.hpp>

#include "common.h"
#include "error.h"
#include "pricing_registry.h"
#include "swaption_vol_diagnostics.h"
#include "swaption_vol_runtime.h"

flatbuffers::Offset<quantra::CalibrateSwaptionVolResponse>
CalibrateSwaptionVolPricingRequest::request(
    std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder,
    const quantra::CalibrateSwaptionVolRequest* request) const {
    if (!request || !request->pricing() || !request->vol_id() ||
        !request->discounting_curve_id() || !request->forwarding_curve_id()) {
        QUANTRA_INVALID_ARGUMENT(
            "CalibrateSwaptionVolRequest requires pricing, vol_id, "
            "discounting_curve_id, and forwarding_curve_id");
    }

    const std::string volId = request->vol_id()->str();
    const std::string discountingCurveId = request->discounting_curve_id()->str();
    const std::string forwardingCurveId = request->forwarding_curve_id()->str();
    if (volId.empty() || discountingCurveId.empty() || forwardingCurveId.empty()) {
        QUANTRA_INVALID_ARGUMENT(
            "CalibrateSwaptionVolRequest fields vol_id, discounting_curve_id, "
            "forwarding_curve_id must be non-empty");
    }

    // Build the pricing registry. The registry builder enforces SABR
    // calibrate validation for free: required swap_index_id, OIS swap-index
    // rejection, grid-size constraints, vol-type rejection. Any QuantraError
    // raised here is a malformed-input error, so re-throw as 4xx.
    quantra::PricingRegistry reg;
    try {
        reg = quantra::PricingRegistryBuilder().build(request->pricing());
    } catch (const QuantraError& e) {
        QUANTRA_INVALID_ARGUMENT(
            std::string("Failed to build pricing registry: ") + e.what());
    }

    const QuantLib::Date asOf = DateToQL(request->pricing()->as_of_date()->str());
    QuantLib::Settings::instance().evaluationDate() = asOf;

    auto vIt = reg.volatility.swaptionVols.find(volId);
    if (vIt == reg.volatility.swaptionVols.end()) {
        QUANTRA_NOT_FOUND("Swaption vol surface not found: " + volId);
    }
    const quantra::SwaptionVolEntry& rawEntry = vIt->second;
    if (rawEntry.volKind != quantra::enums::SwaptionVolKind_SabrCalibrate) {
        // Reject all non-calibrate kinds. SabrParams introspection is covered by
        // /sample-vol-surfaces with include_diagnostics=true.
        QUANTRA_INVALID_ARGUMENT(
            "Endpoint /calibrate-swaption-vol only operates on SabrCalibrate "
            "surfaces; vol_id '" + volId +
            "' is not a SwaptionSabrCalibrateSpec surface");
    }

    auto dIt = reg.rates.curves.find(discountingCurveId);
    if (dIt == reg.rates.curves.end()) {
        QUANTRA_INVALID_ARGUMENT(
            "Discounting curve not found in pricing.rates.curves: " +
            discountingCurveId);
    }
    auto fIt = reg.rates.curves.find(forwardingCurveId);
    if (fIt == reg.rates.curves.end()) {
        QUANTRA_INVALID_ARGUMENT(
            "Forwarding curve not found in pricing.rates.curves: " +
            forwardingCurveId);
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
            discountingCurveId,
            forwardingCurveId);
    } catch (const QuantraError& e) {
        // Validation/configuration failures (bad inputs, OIS swap index slipped
        // past registry, etc.) are still client errors; calibration runtime
        // failures from QL throw QuantLib::Error and propagate to the
        // CallDataGeneric base catch, surfacing as 500.
        QUANTRA_INVALID_ARGUMENT(
            std::string("SABR finalize failed for vol_id '") + volId + "': " + e.what());
    }

    auto diagnosticsOff = quantra::buildSwaptionVolDiagnostics(
        *builder, volId, finalized, /*extraWarnings=*/{});

    auto volIdOff = builder->CreateString(volId);
    quantra::CalibrateSwaptionVolResponseBuilder rb(*builder);
    rb.add_vol_id(volIdOff);
    rb.add_diagnostics(diagnosticsOff);
    return rb.Finish();
}
