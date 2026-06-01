#ifndef QUANTRA_INFLATION_CURVE_PARSERS_H
#define QUANTRA_INFLATION_CURVE_PARSERS_H

#include <map>
#include <memory>
#include <string>
#include <vector>

#include <ql/handle.hpp>
#include <ql/termstructures/inflationtermstructure.hpp>
#include <ql/time/calendar.hpp>
#include <ql/time/businessdayconvention.hpp>
#include <ql/time/daycounter.hpp>
#include <ql/time/date.hpp>

#include "inflation_generated.h"
#include "pricing_registry.h"
#include "quote_registry.h"

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
 * Each InflationIndexSpec is linked to a matching-kind inflation curve when
 * available. Multiple curves can reference the same index_id.
 */
void buildInflationIndices(
    const flatbuffers::Vector<flatbuffers::Offset<quantra::InflationIndexSpec>>* indices,
    const std::map<std::string, InflationCurveEntry>& builtCurves,
    PricingRegistry& reg);

} // namespace quantra

#endif // QUANTRA_INFLATION_CURVE_PARSERS_H
