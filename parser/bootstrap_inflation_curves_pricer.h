#ifndef QUANTRA_BOOTSTRAP_INFLATION_CURVES_PRICER_H
#define QUANTRA_BOOTSTRAP_INFLATION_CURVES_PRICER_H

/**
 * BootstrapInflationCurvesPricer — pure QuantLib core for the inflation
 * curve sampling endpoint.
 *
 * Bootstrapping itself happens transparently inside PricingRegistryBuilder
 * (shared infrastructure); this pricer's only job is to sample each
 * requested inflation curve at the grid dates the mapper resolved up-front
 * and return the per-measure values. The kind (zero vs YoY) is inferred
 * from which inflation registry map the curve id lives in, matching the
 * legacy behaviour.
 *
 * INVARIANT: this file (and its .cpp) must NEVER include any *_generated.h or
 * mention FlatBuffers/gRPC namespaces. Suite 0 (scripts/check_pricer_boundary.sh)
 * enforces this with a literal grep. All wire-format conversion lives in
 * bootstrap_inflation_curves_mapper.{h,cpp}. The pricer reads market data
 * exclusively from reg.inflation.{zero,yoy}InflationCurves and curveMetadata.
 */

#include <cstdint>
#include <string>
#include <vector>

#include <ql/time/date.hpp>

#include "pricing_context.h"
#include "pricing_registry.h"

namespace quantra {

/// Sample measure identifier. Values mirror the wire-format
/// enums::InflationCurveMeasure (ZeroRate=0, YoYRate=1); the mapper
/// translates between the two so the pricer can stay free of any
/// *_generated.h header.
enum class InflationCurveSampleMeasure : std::int8_t {
    ZeroRate = 0,
    YoYRate = 1,
};

/// One inflation curve to sample, lifted out of the FlatBuffers request by
/// the mapper. `gridDates` is pre-resolved relative to the curve's
/// reference date (taken from the FB InflationCurveSpec) so the pricer
/// never needs to inspect the FB grid spec.
struct BootstrapInflationCurvesQuery {
    std::string curveId;
    std::vector<InflationCurveSampleMeasure> measures;
    std::vector<QuantLib::Date> gridDates;
    bool allowExtrapolation = true;
    bool strict = true;
    QuantLib::Date asOfDate;
    /// When non-empty the mapper rejected the query before pricing could
    /// run; the pricer emits a per-curve error entry verbatim and skips
    /// registry lookup. Preserves the legacy per-query error embedding.
    std::string prebuiltError;
};

struct BootstrapInflationCurvesInputs {
    std::vector<BootstrapInflationCurvesQuery> queries;
};

struct BootstrapInflationCurvesSeries {
    InflationCurveSampleMeasure measure = InflationCurveSampleMeasure::ZeroRate;
    std::vector<double> values;
};

struct BootstrapInflationCurvesPerCurve {
    std::string id;
    QuantLib::Date referenceDate;
    std::vector<QuantLib::Date> gridDates;
    std::vector<BootstrapInflationCurvesSeries> series;
    std::vector<QuantLib::Date> pillarDates;
    /// Empty on success; mapper translates a non-empty value into the
    /// response's Error table.
    std::string error;
};

struct BootstrapInflationCurvesResult {
    std::vector<BootstrapInflationCurvesPerCurve> curves;
};

class BootstrapInflationCurvesPricer {
public:
    BootstrapInflationCurvesResult price(
        const BootstrapInflationCurvesInputs& inputs,
        const PricingRegistry& reg,
        const PricingContext& ctx) const;
};

} // namespace quantra

#endif // QUANTRA_BOOTSTRAP_INFLATION_CURVES_PRICER_H
