#include "cms_leg_parser.h"

#include <cmath>
#include <limits>

#include "require_scalar.h"
#include "require_period.h"

#include "vanilla_swap_generated.h"

#include <ql/cashflows/cmscoupon.hpp>
#include <ql/cashflows/conundrumpricer.hpp>
#include <ql/cashflows/lineartsrpricer.hpp>
#include <ql/quotes/simplequote.hpp>

namespace quantra {
namespace {

QuantLib::GFunctionFactory::YieldCurveModel toYieldCurveModel(
    quantra::enums::CmsYieldCurveModel model) {
    switch (model) {
        case quantra::enums::CmsYieldCurveModel_Standard:
            return QuantLib::GFunctionFactory::Standard;
        case quantra::enums::CmsYieldCurveModel_ExactYield:
            return QuantLib::GFunctionFactory::ExactYield;
        case quantra::enums::CmsYieldCurveModel_ParallelShifts:
            return QuantLib::GFunctionFactory::ParallelShifts;
        case quantra::enums::CmsYieldCurveModel_NonParallelShifts:
            return QuantLib::GFunctionFactory::NonParallelShifts;
    }
    QUANTRA_INVALID_ARGUMENT("Unsupported CMS yield_curve_model");
    return QuantLib::GFunctionFactory::Standard;
}

} // namespace

QuantLib::Leg CmsLegParser::parse(
    const quantra::SwapCmsLeg* leg,
    const quantra::IndexRegistry& indices,
    const quantra::SwapIndexRegistry& swapIndices,
    const QuantLib::Handle<QuantLib::YieldTermStructure>& forwardingCurve,
    const QuantLib::Handle<QuantLib::YieldTermStructure>& discountCurve) const {
    if (!leg) {
        QUANTRA_INVALID_ARGUMENT("CMS leg not found");
    }
    if (!leg->schedule()) {
        QUANTRA_INVALID_ARGUMENT("CMS leg schedule not found");
    }
    if (!leg->swap_index_id()) {
        QUANTRA_INVALID_ARGUMENT("CMS leg swap_index_id is required");
    }
    if (!leg->swap_tenor()) {
        QUANTRA_INVALID_ARGUMENT("CMS leg swap_tenor is required");
    }
    if (!leg->swaption_vol_id()) {
        QUANTRA_INVALID_ARGUMENT("CMS leg swaption_vol_id is required");
    }
    if (leg->cap().has_value() && !std::isfinite(leg->cap().value())) {
        QUANTRA_INVALID_ARGUMENT("SwapCmsLeg.cap must be a finite number");
    }
    if (leg->floor().has_value() && !std::isfinite(leg->floor().value())) {
        QUANTRA_INVALID_ARGUMENT("SwapCmsLeg.floor must be a finite number");
    }
    if (leg->cap().has_value() && leg->floor().has_value() &&
        leg->cap().value() < leg->floor().value()) {
        QUANTRA_INVALID_ARGUMENT("SwapCmsLeg.cap must be >= floor");
    }
    if (!leg->day_counter().has_value()) {
        QUANTRA_INVALID_ARGUMENT("SwapCmsLeg.day_counter is required");
    }
    if (!leg->payment_convention().has_value()) {
        QUANTRA_INVALID_ARGUMENT("SwapCmsLeg.payment_convention is required");
    }

    ScheduleParser scheduleParser;
    auto schedule = scheduleParser.parse(leg->schedule());
    QuantLib::Period cmsTenor = requirePeriod(leg->swap_tenor(), "SwapCmsLeg.swap_tenor");
    const std::string swapIndexId = leg->swap_index_id()->str();
    auto swapIndex = swapIndices.getIborSwapIndexWithCurves(
        swapIndexId, cmsTenor, indices, forwardingCurve, discountCurve);

    QuantLib::CmsLeg cmsBuilder(*schedule, swapIndex);
    cmsBuilder.withNotionals(requirePositive(leg->notional(), "SwapCmsLeg.notional"));
    cmsBuilder.withPaymentDayCounter(DayCounterToQL(leg->day_counter().value()));
    cmsBuilder.withPaymentAdjustment(ConventionToQL(leg->payment_convention().value()));
    cmsBuilder.withGearings(leg->gear());
    cmsBuilder.withSpreads(leg->spread());

    if (leg->fixing_days() >= 0) {
        cmsBuilder.withFixingDays(static_cast<QuantLib::Natural>(leg->fixing_days()));
    } else {
        const auto& runtime = swapIndices.get(swapIndexId);
        cmsBuilder.withFixingDays(static_cast<QuantLib::Natural>(runtime.spotDays));
    }
    if (leg->cap().has_value()) {
        cmsBuilder.withCaps(leg->cap().value());
    }
    if (leg->floor().has_value()) {
        cmsBuilder.withFloors(leg->floor().value());
    }

    return cmsBuilder;
}

CmsPricerBuildResult CmsLegParser::makeCouponPricer(
    const quantra::SwapCmsLeg* leg,
    const quantra::SwaptionVolEntry& volEntry,
    const QuantLib::Handle<QuantLib::YieldTermStructure>& discountCurve) const {
    if (!leg || !leg->swaption_vol_id()) {
        QUANTRA_INVALID_ARGUMENT("CMS leg swaption_vol_id is required");
    }
    if (volEntry.handle.empty()) {
        QUANTRA_ERROR("CMS leg swaption vol handle is empty");
    }
    const auto* ps = leg->pricer();
    const auto pricerType = ps ? ps->pricer_type() : quantra::enums::CmsPricerType_LinearTsr;
    const auto ycModel =
        ps ? ps->yield_curve_model() : quantra::enums::CmsYieldCurveModel_Standard;
    const auto qlYcModel = toYieldCurveModel(ycModel);
    const double mr = ps ? ps->mean_reversion() : 0.03;
    if (!(mr >= 0.0) || !std::isfinite(mr)) {
        QUANTRA_INVALID_ARGUMENT("CMS leg mean_reversion must be non-negative");
    }
    auto meanReversion = QuantLib::Handle<QuantLib::Quote>(
        QuantLib::ext::make_shared<QuantLib::SimpleQuote>(mr));

    CmsPricerBuildResult result;
    result.used.pricerType = pricerType;
    result.used.yieldCurveModel = ycModel;
    result.used.meanReversion = mr;

    switch (pricerType) {
        case quantra::enums::CmsPricerType_LinearTsr:
            result.pricer = std::make_shared<QuantLib::LinearTsrPricer>(
                volEntry.handle,
                meanReversion,
                discountCurve);
            return result;
        case quantra::enums::CmsPricerType_HaganAnalytic:
            result.pricer = std::make_shared<QuantLib::AnalyticHaganPricer>(
                volEntry.handle,
                qlYcModel,
                meanReversion);
            return result;
        case quantra::enums::CmsPricerType_HaganNumeric: {
            const double lowerLimit = ps ? ps->hagan_lower_limit() : 0.0;
            const double upperLimit = ps ? ps->hagan_upper_limit() : 1.0;
            const double precision = ps ? ps->hagan_precision() : 1.0e-6;
            const double hardUpperLimitRaw = ps ? ps->hagan_hard_upper_limit() : -1.0;

            if (!std::isfinite(lowerLimit) || !std::isfinite(upperLimit) ||
                !(upperLimit > lowerLimit)) {
                QUANTRA_INVALID_ARGUMENT("CMS leg Hagan numeric requires upper_limit > lower_limit");
            }
            if (!std::isfinite(precision) || !(precision > 0.0)) {
                QUANTRA_INVALID_ARGUMENT("CMS leg hagan_precision must be positive");
            }

            const double hardUpperLimit =
                hardUpperLimitRaw > 0.0 ? hardUpperLimitRaw : std::numeric_limits<double>::max();
            if (!std::isfinite(hardUpperLimit) || hardUpperLimit <= upperLimit) {
                QUANTRA_INVALID_ARGUMENT("CMS leg hagan_hard_upper_limit must be > hagan_upper_limit");
            }
            result.used.haganLowerLimit = lowerLimit;
            result.used.haganUpperLimit = upperLimit;
            result.used.haganPrecision = precision;
            result.used.haganHardUpperLimit = hardUpperLimit;
            result.pricer = std::make_shared<QuantLib::NumericHaganPricer>(
                volEntry.handle,
                qlYcModel,
                meanReversion,
                lowerLimit,
                upperLimit,
                precision,
                hardUpperLimit);
            return result;
        }
    }
    QUANTRA_INVALID_ARGUMENT("Unsupported CMS pricer type");
    return result;
}

} // namespace quantra
