#ifndef QUANTRA_SWAPTION_VOL_RUNTIME_H
#define QUANTRA_SWAPTION_VOL_RUNTIME_H

#include <vector>

#include <ql/handle.hpp>
#include <ql/termstructures/yieldtermstructure.hpp>

#include "price_swaption_request_generated.h"
#include "pricing_registry.h"
#include "vol_surface_parsers.h"

namespace quantra {

std::vector<double> computeServerAtmForwards(
    const quantra::SwaptionVolEntry& volEntry,
    const quantra::SwapIndexRuntime& sidx,
    const quantra::IndexRegistry& indices,
    const QuantLib::Handle<QuantLib::YieldTermStructure>& discountCurve,
    const QuantLib::Handle<QuantLib::YieldTermStructure>& forwardingCurve);

SwaptionVolEntry finalizeSwaptionVolEntryForPricing(
    const SwaptionVolEntry& raw,
    const quantra::PriceSwaption* trade,
    const PricingRegistry& reg,
    const QuantLib::Handle<QuantLib::YieldTermStructure>& discountCurve,
    const QuantLib::Handle<QuantLib::YieldTermStructure>& forwardingCurve,
    bool forceRecomputeAtm,
    // Explicit curve ids used for content-keyed caches (e.g. the SABR
    // calibrate cube cache). When unset, falls back to ids inferred from
    // `trade`. Pass these from the sampler where no trade is in scope but
    // curve ids are explicit on the query.
    const std::string& discountCurveId = "",
    const std::string& forwardingCurveId = "");

std::vector<double> computeServerAtmForwardsForExerciseDates(
    const std::vector<QuantLib::Date>& exerciseDates,
    const std::vector<QuantLib::Period>& tenors,
    const quantra::SwapIndexRuntime& sidx,
    const quantra::IndexRegistry& indices,
    const QuantLib::Handle<QuantLib::YieldTermStructure>& discountCurve,
    const QuantLib::Handle<QuantLib::YieldTermStructure>& forwardingCurve);

} // namespace quantra

#endif // QUANTRA_SWAPTION_VOL_RUNTIME_H
