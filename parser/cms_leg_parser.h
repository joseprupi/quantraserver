#ifndef QUANTRASERVER_CMS_LEG_PARSER_H
#define QUANTRASERVER_CMS_LEG_PARSER_H

#include <memory>

#include <ql/cashflows/couponpricer.hpp>
#include <ql/handle.hpp>
#include <ql/termstructures/yieldtermstructure.hpp>

#include "common.h"
#include "common_parser.h"
#include "error.h"
#include "index_registry.h"
#include "swap_index_registry.h"
#include "vol_surface_parsers.h"

namespace quantra {

class SwapCmsLeg;

struct CmsPricerUsed {
    quantra::enums::CmsPricerType pricerType = quantra::enums::CmsPricerType_LinearTsr;
    quantra::enums::CmsYieldCurveModel yieldCurveModel =
        quantra::enums::CmsYieldCurveModel_Standard;
    double meanReversion = 0.03;
    double haganLowerLimit = -1.0;
    double haganUpperLimit = -1.0;
    double haganPrecision = -1.0;
    double haganHardUpperLimit = -1.0;
};

struct CmsPricerBuildResult {
    std::shared_ptr<QuantLib::FloatingRateCouponPricer> pricer = nullptr;
    CmsPricerUsed used{};
};

class CmsLegParser {
public:
    QuantLib::Leg parse(
        const quantra::SwapCmsLeg* leg,
        const quantra::IndexRegistry& indices,
        const quantra::SwapIndexRegistry& swapIndices,
        const QuantLib::Handle<QuantLib::YieldTermStructure>& forwardingCurve,
        const QuantLib::Handle<QuantLib::YieldTermStructure>& discountCurve) const;

    CmsPricerBuildResult makeCouponPricer(
        const quantra::SwapCmsLeg* leg,
        const quantra::SwaptionVolEntry& volEntry,
        const QuantLib::Handle<QuantLib::YieldTermStructure>& discountCurve) const;
};

} // namespace quantra

#endif // QUANTRASERVER_CMS_LEG_PARSER_H
