#ifndef QUANTRA_CDS_EVALUATOR_H
#define QUANTRA_CDS_EVALUATOR_H

/**
 * CdsEvaluator — pure QuantLib pricing core for credit default swaps.
 *
 * INVARIANT: this file (and its .cpp) must NEVER include any *_generated.h or
 * mention FlatBuffers/gRPC namespaces. Suite 0 (scripts/check_evaluator_boundary.sh)
 * enforces this with a literal grep. All wire-format conversion lives in
 * cds_mapper.{h,cpp}. The evaluator reads market data exclusively from the
 * plain-domain registry fields (reg.credit.creditCurves,
 * reg.volatility.modelDomains).
 */

#include <memory>
#include <string>
#include <vector>

#include <ql/instruments/creditdefaultswap.hpp>
#include <ql/time/businessdayconvention.hpp>
#include <ql/time/daycounter.hpp>
#include <ql/time/schedule.hpp>

#include "pricing_context.h"
#include "pricing_registry.h"

namespace quantra {

/// One CDS trade lifted out of the FB request by the mapper. The mapper has
/// already converted enums to QL types and parsed the schedule; the evaluator
/// never touches FlatBuffers.
struct CdsTrade {
    QuantLib::Protection::Side side = QuantLib::Protection::Buyer;
    double notional = 0.0;
    double runningCoupon = 0.0;
    double upfront = 0.0;
    QuantLib::Schedule schedule;
    QuantLib::DayCounter dayCounter;
    QuantLib::BusinessDayConvention businessDayConvention = QuantLib::Following;
    bool settlesAccrual = true;
    bool paysAtDefaultTime = true;
    bool rebatesAccrual = true;
    QuantLib::Date protectionStart;
    QuantLib::Date upfrontDate;
    QuantLib::DayCounter lastPeriodDayCounter;
    QuantLib::Date tradeDate;
    QuantLib::Natural cashSettlementDays = 3;
    /// True when the request supplied an upfront date string; the legacy
    /// parser selects the upfront-bearing CreditDefaultSwap constructor when
    /// either upfront != 0.0 OR an upfront date is present.
    bool hasUpfrontDate = false;

    std::string discountingCurveId;
    std::string creditCurveId;
    std::string modelId;
};

struct CdsInputs {
    std::vector<CdsTrade> trades;
};

/// Per-trade pricing result. Mirrors the legacy CdsPriceResult fields.
struct CdsPerTrade {
    double npv = 0.0;
    double fairSpread = 0.0;
    double fairUpfront = 0.0;
    double defaultLegNpv = 0.0;
    double premiumLegNpv = 0.0;
};

struct CdsResult {
    std::vector<CdsPerTrade> values;
};

class CdsEvaluator {
public:
    CdsResult evaluate(const CdsInputs& inputs,
                    const PricingRegistry& reg,
                    const PricingContext& ctx) const;
};

} // namespace quantra

#endif // QUANTRA_CDS_EVALUATOR_H
