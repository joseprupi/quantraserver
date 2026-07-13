#ifndef QUANTRA_YEAR_ON_YEAR_INFLATION_CAP_FLOOR_EVALUATOR_H
#define QUANTRA_YEAR_ON_YEAR_INFLATION_CAP_FLOOR_EVALUATOR_H

/**
 * YearOnYearInflationCapFloorEvaluator — pure QuantLib pricing core for
 * year-on-year inflation caps / floors / collars (QuantLib::YoYInflationCapFloor).
 *
 * INVARIANT: this file (and its .cpp) must NEVER include any *_generated.h or
 * mention FlatBuffers/gRPC namespaces. Suite 0 (scripts/check_evaluator_boundary.sh)
 * enforces this with a literal grep. All wire-format conversion lives in
 * year_on_year_inflation_cap_floor_mapper.{h,cpp}. The evaluator reads market
 * data exclusively from the QL-typed registry.
 */

#include <memory>
#include <string>
#include <vector>

#include <ql/instruments/inflationcapfloor.hpp>
#include <ql/time/businessdayconvention.hpp>
#include <ql/time/daycounter.hpp>
#include <ql/time/period.hpp>
#include <ql/time/schedule.hpp>

#include "pricing_context.h"
#include "pricing_registry.h"

namespace quantra {

/// One YoY inflation cap/floor trade lifted out of the FB request by the
/// mapper. The mapper has validated type-vs-strike coherence and converted
/// enums to QL types; the evaluator never touches FlatBuffers.
struct YoYInflationCapFloorTrade {
    QuantLib::YoYInflationCapFloor::Type capFloorType =
        QuantLib::YoYInflationCapFloor::Cap;
    double notional = 0.0;
    QuantLib::Schedule schedule;
    std::string inflationIndexId;
    QuantLib::Period observationLag;
    QuantLib::DayCounter dayCounter;
    QuantLib::BusinessDayConvention paymentConvention = QuantLib::ModifiedFollowing;

    /// Strikes. `hasCapRate` is true for Cap and Collar; `hasFloorRate` for
    /// Floor and Collar. The mapper guarantees the type/strike coherence.
    bool hasCapRate = false;
    double capRate = 0.0;
    bool hasFloorRate = false;
    double floorRate = 0.0;

    /// Optional per-optionlet gearing/spread (absent => 1.0 / 0.0).
    bool hasGearing = false;
    double gearing = 1.0;
    bool hasSpread = false;
    double spread = 0.0;

    std::string discountingCurveId;
    std::string inflationCurveId;
    std::string volatilityId;
};

struct YoYInflationCapFloorInputs {
    std::vector<YoYInflationCapFloorTrade> trades;
};

/// Per-instrument result. Mirrors the YoYInflationCapFloorResponse FB table.
struct YoYInflationCapFloorPerTrade {
    double npv = 0.0;
    double atmRate = 0.0;
};

struct YoYInflationCapFloorResult {
    std::vector<YoYInflationCapFloorPerTrade> capFloors;
};

class YearOnYearInflationCapFloorEvaluator {
public:
    YoYInflationCapFloorResult evaluate(const YoYInflationCapFloorInputs& inputs,
                                        const PricingRegistry& reg,
                                        const PricingContext& ctx) const;
};

} // namespace quantra

#endif // QUANTRA_YEAR_ON_YEAR_INFLATION_CAP_FLOOR_EVALUATOR_H
