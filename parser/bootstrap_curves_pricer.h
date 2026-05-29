#ifndef QUANTRA_BOOTSTRAP_CURVES_PRICER_H
#define QUANTRA_BOOTSTRAP_CURVES_PRICER_H

/**
 * BootstrapCurvesPricer — pure QuantLib core for the curve-sampling endpoint.
 *
 * The bootstrap itself happens transparently inside PricingRegistryBuilder
 * (shared infrastructure shared with every pricing product). This pricer's
 * only job is to sample each requested curve at the grid dates the mapper
 * resolved up-front and return the per-measure values; the mapper then
 * serializes those values back into the FlatBuffers response.
 *
 * INVARIANT: this file (and its .cpp) must NEVER include any *_generated.h or
 * mention FlatBuffers/gRPC namespaces. Suite 0 (scripts/check_pricer_boundary.sh)
 * enforces this with a literal grep. All wire-format conversion lives in
 * bootstrap_curves_mapper.{h,cpp}. The pricer reads market data exclusively
 * from reg.rates.curves.
 */

#include <cstdint>
#include <string>
#include <vector>

#include <ql/compounding.hpp>
#include <ql/time/businessdayconvention.hpp>
#include <ql/time/calendar.hpp>
#include <ql/time/date.hpp>
#include <ql/time/daycounter.hpp>
#include <ql/time/frequency.hpp>
#include <ql/time/period.hpp>
#include <ql/time/timeunit.hpp>

#include "pricing_context.h"
#include "pricing_registry.h"

namespace quantra {

/// Sample measure identifier. Values mirror the wire-format CurveMeasure
/// (DF=0, ZERO=1, FWD=2); the mapper translates between the two so the
/// pricer can stay free of any *_generated.h header.
enum class CurveSampleMeasure : std::int8_t {
    DiscountFactor = 0,
    ZeroRate = 1,
    ForwardRate = 2,
};

/// Per-measure config for zero-rate sampling, fully resolved by the mapper.
struct ZeroSampleSpec {
    bool useCurveDayCounter = true;
    QuantLib::DayCounter dayCounter;
    QuantLib::Compounding compounding = QuantLib::Continuous;
    QuantLib::Frequency frequency = QuantLib::Annual;
};

/// Per-measure config for forward-rate sampling. The mapper resolves the
/// calendar/BDC pair from `use_grid_calendar_for_advance` so the pricer
/// uses the values directly.
struct ForwardSampleSpec {
    bool useCurveDayCounter = true;
    QuantLib::DayCounter dayCounter;
    QuantLib::Compounding compounding = QuantLib::Simple;
    QuantLib::Frequency frequency = QuantLib::Annual;
    bool instantaneous = true;
    int epsNumber = 1;
    QuantLib::TimeUnit epsUnit = QuantLib::Days;
    QuantLib::Period tenor = QuantLib::Period(3, QuantLib::Months);
    QuantLib::Calendar calendar;
    QuantLib::BusinessDayConvention bdc = QuantLib::Following;
};

/// One curve to sample, lifted out of the FlatBuffers request by the mapper.
/// `gridDates` and `pillarDates` are pre-resolved so the pricer never needs
/// to inspect the FB grid spec or helper points.
struct BootstrapCurvesQuery {
    std::string curveId;
    std::vector<CurveSampleMeasure> measures;
    std::vector<QuantLib::Date> gridDates;
    std::vector<QuantLib::Date> pillarDates;
    bool allowExtrapolation = true;
    ZeroSampleSpec zero;
    ForwardSampleSpec fwd;
    /// When non-empty the mapper rejected the query before pricing could
    /// run; the pricer emits a per-curve error entry verbatim and skips
    /// registry lookup. Preserves the legacy per-query error embedding.
    std::string prebuiltError;
};

struct BootstrapCurvesInputs {
    std::vector<BootstrapCurvesQuery> queries;
};

struct BootstrapCurvesSeries {
    CurveSampleMeasure measure = CurveSampleMeasure::DiscountFactor;
    std::vector<double> values;
};

struct BootstrapCurvesPerCurve {
    std::string id;
    QuantLib::Date referenceDate;
    std::vector<QuantLib::Date> gridDates;
    std::vector<BootstrapCurvesSeries> series;
    std::vector<QuantLib::Date> pillarDates;
    /// Empty on success; mapper translates a non-empty value into the
    /// response's Error table.
    std::string error;
};

struct BootstrapCurvesResult {
    std::vector<BootstrapCurvesPerCurve> curves;
};

class BootstrapCurvesPricer {
public:
    BootstrapCurvesResult price(const BootstrapCurvesInputs& inputs,
                                const PricingRegistry& reg,
                                const PricingContext& ctx) const;
};

} // namespace quantra

#endif // QUANTRA_BOOTSTRAP_CURVES_PRICER_H
