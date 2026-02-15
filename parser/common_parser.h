#ifndef QUANTRASERVER_COMMONPARSER_H
#define QUANTRASERVER_COMMONPARSER_H

/**
 * Common Parser
 * 
 * Contains shared parsing utilities used across all request handlers:
 *   - ScheduleParser: Parses FlatBuffers Schedule -> QuantLib Schedule
 *   - YieldParser: Parses yield specification for bond analytics
 *   - PricingParser: Parses common Pricing block (curves, flags)
 * 
 * NOTE: For instruments requiring vols/models (caps, swaptions, equity options),
 * use PricingRegistryBuilder instead of PricingParser. The registry provides
 * typed vol maps and model specs.
 */

#include <ql/qldefines.hpp>
#include <ql/termstructures/yield/piecewiseyieldcurve.hpp>
#include <ql/termstructures/yieldtermstructure.hpp>
#include <ql/termstructures/yield/ratehelpers.hpp>
#include <ql/termstructures/yield/oisratehelper.hpp>
#include <ql/pricingengines/swap/discountingswapengine.hpp>
#include <ql/indexes/ibor/eonia.hpp>
#include <ql/indexes/ibor/euribor.hpp>
#include <ql/time/imm.hpp>
#include <ql/time/calendars/target.hpp>
#include <ql/time/daycounters/actual360.hpp>
#include <ql/time/daycounters/thirty360.hpp>
#include <ql/time/daycounters/actualactual.hpp>
#include <ql/math/interpolations/cubicinterpolation.hpp>
#include <ql/math/interpolations/loginterpolation.hpp>

#include "common_generated.h"
#include "pricing_generated.h"
#include "term_structure_generated.h"
#include "coupon_pricer_generated.h"
#include "enums.h"
#include "common.h"
#include "schedule_generated.h"

using namespace QuantLib;
using namespace quantra;

// =============================================================================
// Yield Struct - for bond analytics
// =============================================================================
struct YieldStruct
{
    QuantLib::DayCounter day_counter;
    QuantLib::Compounding compounding;
    QuantLib::Frequency frequency;
};

// =============================================================================
// Pricing Struct - legacy struct for non-vol instruments
// =============================================================================
struct PricingStruct
{
    std::string as_of_date;
    std::string settlement_date;
    const flatbuffers::Vector<flatbuffers::Offset<quantra::TermStructure>> *curves;
    bool bond_pricing_details;
    bool bond_pricing_flows;
    const flatbuffers::Vector<flatbuffers::Offset<quantra::CouponPricer>> *coupon_pricers;
};

// =============================================================================
// Schedule Parser
// =============================================================================
class ScheduleParser
{
public:
    std::shared_ptr<QuantLib::Schedule> parse(const quantra::Schedule *schedule);
};

// =============================================================================
// Yield Parser
// =============================================================================
class YieldParser
{
public:
    std::shared_ptr<YieldStruct> parse(const quantra::Yield *yield);
};

// =============================================================================
// Pricing Parser (legacy - for non-vol instruments)
// 
// Use this for: bonds, swaps, FRAs, CDS
// Use PricingRegistryBuilder for: caps, floors, swaptions, equity options
// =============================================================================
class PricingParser
{
private:
    std::map<std::string, std::shared_ptr<YieldTermStructure>> term_structures;

public:
    std::shared_ptr<PricingStruct> parse(const quantra::Pricing *pricing);
    std::shared_ptr<PricingStruct> parse_term_structure(const quantra::Pricing *pricing);
};

#endif // QUANTRASERVER_COMMONPARSER_H
