#ifndef QUANTRA_SWAPTION_VOL_RUNTIME_H
#define QUANTRA_SWAPTION_VOL_RUNTIME_H

#include "pricing_registry.h"
#include "vol_surface_parsers.h"

#include <ql/handle.hpp>
#include <ql/termstructures/yieldtermstructure.hpp>

#include <vector>

namespace quantra {

std::vector<double> computeServerAtmForwards(
    const quantra::SwaptionVolEntry& volEntry,
    const quantra::SwapIndexRuntime& sidx,
    const quantra::IndexRegistry& indices,
    const QuantLib::Handle<QuantLib::YieldTermStructure>& discountCurve,
    const QuantLib::Handle<QuantLib::YieldTermStructure>& forwardingCurve);

SwaptionVolEntry finalizeSwaptionVolEntryForPricing(
    const SwaptionVolEntry& raw,
    const PricingRegistry& reg,
    const QuantLib::Handle<QuantLib::YieldTermStructure>& discountCurve,
    const QuantLib::Handle<QuantLib::YieldTermStructure>& forwardingCurve,
    bool forceRecomputeAtm,
    // Curve ids used for content-keyed caches (e.g. the SABR calibrate cube
    // cache). Empty ids disable that caching for the call (safe: the cube is
    // rebuilt rather than served stale).
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
