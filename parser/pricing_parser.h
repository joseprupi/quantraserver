// #ifndef QUANTRASERVER_COMMONPARSER_H
// #define QUANTRASERVER_COMMONPARSER_H

// #include <ql/qldefines.hpp>
// #include <ql/termstructures/yield/piecewiseyieldcurve.hpp>
// #include <ql/termstructures/yieldtermstructure.hpp>
// #include <ql/termstructures/yield/ratehelpers.hpp>
// #include <ql/termstructures/yield/oisratehelper.hpp>
// #include <ql/pricingengines/swap/discountingswapengine.hpp>
// #include <ql/indexes/ibor/eonia.hpp>
// #include <ql/indexes/ibor/euribor.hpp>
// #include <ql/time/imm.hpp>
// #include <ql/time/calendars/target.hpp>
// #include <ql/time/daycounters/actual360.hpp>
// #include <ql/time/daycounters/thirty360.hpp>
// #include <ql/time/daycounters/actualactual.hpp>
// #include <ql/math/interpolations/cubicinterpolation.hpp>
// #include <ql/math/interpolations/loginterpolation.hpp>

// #include "term_structure_parser.h"
// //#include "quantra_structs.h"
// #include "common_generated.h"
// #include "enums.h"
// #include "common.h"
// #include "schedule_generated.h"

// using namespace QuantLib;
// using namespace quantra;

// struct PricingStruct
// {
//     std::string as_of_date;
//     std::string settlement_date;
//     const flatbuffers::Vector<flatbuffers::Offset<quantra::TermStructure>> *term_structures;
//     bool bond_pricing_details;
//     bool bond_pricing_flows;
//     const flatbuffers::Vector<flatbuffers::Offset<quantra::CouponPricer>> *coupon_pricers;
// };

// class PricingParser
// {

// private:
//     std::map<std::string, std::shared_ptr<YieldTermStructure>> term_structures;
//     std::map<std::string, std::shared_ptr<std::shared_ptr<QuantLib::IborCouponPricer>>> pricers;

// public:
//     void parse(const quantra::Pricing *pricing);
//     void parse_term_structure(const flatbuffers::Vector<flatbuffers::Offset<quantra::TermStructure>> *curves);
//     void parse_pricer(const flatbuffers::Vector<flatbuffers::Offset<quantra::Pricer>> *pricers);
// };

// #endif // QUANTRASERVER_COMMONPARSER_H