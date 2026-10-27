#ifndef QUANTRA_INFLATION_CURVE_PARSERS_H
#define QUANTRA_INFLATION_CURVE_PARSERS_H

#include "pricing_registry.h"
#include "quote_registry.h"

#include "inflation_generated.h"

#include <ql/handle.hpp>
#include <ql/termstructures/inflationtermstructure.hpp>
#include <ql/time/businessdayconvention.hpp>
#include <ql/time/calendar.hpp>
#include <ql/time/date.hpp>
#include <ql/time/daycounter.hpp>

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace quantra {

/**
 * Build inflation curves and populate PricingRegistry maps.
 *
 * Curves are stored as relinkable handles in:
 *  - reg.inflation.zeroInflationCurves
 *  - reg.inflation.yoyInflationCurves
 *
 * Returns metadata entries keyed by curve id for downstream query endpoints.
 */
std::map<std::string, InflationCurveEntry> buildInflationCurves(
    const flatbuffers::Vector<flatbuffers::Offset<quantra::InflationCurveSpec>>* curves,
    const flatbuffers::Vector<flatbuffers::Offset<quantra::InflationIndexSpec>>* indices,
    const QuoteRegistry* quotes,
    PricingRegistry& reg);

/**
 * Build inflation indices and populate PricingRegistry.inflationIndices.
 *
 * Each index carries its spec's fixings and is linked to one matching-kind
 * inflation curve on it, if any (the first by curve id). Several curves can
 * reference the same index_id, so trades are not priced through that link:
 * each trade uses a clone of the index linked to the curve it references
 * (clones share the index's fixings).
 */
void buildInflationIndices(
    const flatbuffers::Vector<flatbuffers::Offset<quantra::InflationIndexSpec>>* indices,
    const std::map<std::string, InflationCurveEntry>& builtCurves,
    PricingRegistry& reg);

} // namespace quantra

#endif // QUANTRA_INFLATION_CURVE_PARSERS_H
