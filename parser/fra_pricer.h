#ifndef QUANTRA_FRA_PRICER_H
#define QUANTRA_FRA_PRICER_H

/**
 * FraPricer — pure QuantLib pricing core for Forward Rate Agreements
 * (single IBOR index, single forward period, fixed strike, long/short).
 *
 * INVARIANT: this file (and its .cpp) must NEVER include any *_generated.h or
 * mention FlatBuffers/gRPC namespaces. Suite 0 (scripts/check_pricer_boundary.sh)
 * enforces this with a literal grep. All wire-format conversion lives in
 * fra_mapper.{h,cpp}. The pricer reads market data exclusively from the
 * QL-typed registry fields (reg.rates.curves, reg.rates.indices).
 */

#include <string>
#include <vector>

#include <ql/position.hpp>
#include <ql/time/date.hpp>

#include "pricing_context.h"
#include "pricing_registry.h"

namespace quantra {

/// One FRA trade lifted out of the FB request by the mapper.
struct FraTrade {
    QuantLib::Date startDate;
    QuantLib::Date maturityDate;
    QuantLib::Position::Type position = QuantLib::Position::Long;
    double notional = 0.0;
    double strike = 0.0;
    std::string discountingCurveId;
    std::string forwardingCurveId;
    std::string indexId;
};

struct FraInputs {
    std::vector<FraTrade> trades;
};

/// Per-FRA result. Mirrors the FRAResponse FB table.
struct FraPerTrade {
    double npv = 0.0;
    double forwardRate = 0.0;
    double spotValue = 0.0;
    /// ISO string of the FRA start (settlement) date.
    std::string settlementDate;
};

struct FraResult {
    std::vector<FraPerTrade> trades;
};

class FraPricer {
public:
    FraResult price(const FraInputs& inputs,
                    const PricingRegistry& reg,
                    const PricingContext& ctx) const;
};

} // namespace quantra

#endif // QUANTRA_FRA_PRICER_H
