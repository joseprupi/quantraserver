#ifndef QUANTRA_CAP_FLOOR_PRICER_H
#define QUANTRA_CAP_FLOOR_PRICER_H

/**
 * CapFloorPricer — pure QuantLib pricing core for caps, floors, and
 * (eventually) collars on IBOR coupons.
 *
 * INVARIANT: this file (and its .cpp) must NEVER include any *_generated.h or
 * mention FlatBuffers/gRPC namespaces. Suite 0 (scripts/check_pricer_boundary.sh)
 * enforces this with a literal grep. All wire-format conversion lives in
 * cap_floor_mapper.{h,cpp}. The pricer reads market data exclusively from the
 * plain-domain registry fields (reg.rates.curves, reg.rates.indices,
 * reg.volatility.optionletVols, reg.volatility.modelDomains).
 */

#include <string>
#include <vector>

#include <ql/instruments/capfloor.hpp>
#include <ql/time/businessdayconvention.hpp>
#include <ql/time/daycounter.hpp>
#include <ql/time/schedule.hpp>

#include "pricing_context.h"
#include "pricing_registry.h"

namespace quantra {

/// One cap/floor trade lifted out of the FB request by the mapper. The mapper
/// has already converted enums to QL types and parsed the schedule; the pricer
/// never touches FlatBuffers.
struct CapFloorTrade {
    QuantLib::CapFloor::Type capFloorType = QuantLib::CapFloor::Cap;
    QuantLib::Schedule schedule;
    double notional = 0.0;
    double strike = 0.0;
    QuantLib::DayCounter dayCounter;
    QuantLib::BusinessDayConvention businessDayConvention = QuantLib::Following;
    bool includeDetails = false;

    std::string discountingCurveId;
    std::string forwardingCurveId;
    std::string indexId;
    std::string volatilityId;
    std::string modelId;
};

struct CapFloorInputs {
    std::vector<CapFloorTrade> trades;
};

/// Per-caplet/floorlet detail. Mirrors the CapFloorLet FB schema. Populated
/// only when CapFloorTrade::includeDetails is true; fields not set by the
/// legacy buildCapFloorDetails (strike, price) are left at their default
/// values so the mapper can copy them verbatim.
struct CapFloorLetDetail {
    std::string paymentDate;
    std::string accrualStartDate;
    std::string accrualEndDate;
    std::string fixingDate;
    double strike = 0.0;
    double forwardRate = 0.0;
    double discount = 0.0;
    double price = 0.0;
};

/// Per-trade pricing result. Mirrors the CapFloorResponse FB schema.
struct CapFloorPerTrade {
    double npv = 0.0;
    double atmRate = 0.0;
    /// Left at 0.0 to match the legacy path (never populated there).
    double impliedVolatility = 0.0;
    std::vector<CapFloorLetDetail> details;
};

struct CapFloorResult {
    std::vector<CapFloorPerTrade> trades;
};

class CapFloorPricer {
public:
    CapFloorResult price(const CapFloorInputs& inputs,
                         const PricingRegistry& reg,
                         const PricingContext& ctx) const;
};

} // namespace quantra

#endif // QUANTRA_CAP_FLOOR_PRICER_H
