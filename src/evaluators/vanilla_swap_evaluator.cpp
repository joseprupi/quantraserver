#include "vanilla_swap_evaluator.h"

#include <cmath>
#include <iostream>
#include <limits>
#include <sstream>

#include <ql/cashflow.hpp>
#include <ql/cashflows/cmscoupon.hpp>
#include <ql/cashflows/conundrumpricer.hpp>
#include <ql/cashflows/coupon.hpp>
#include <ql/cashflows/couponpricer.hpp>
#include <ql/cashflows/fixedratecoupon.hpp>
#include <ql/cashflows/floatingratecoupon.hpp>
#include <ql/cashflows/iborcoupon.hpp>
#include <ql/cashflows/lineartsrpricer.hpp>
#include <ql/handle.hpp>
#include <ql/indexes/iborindex.hpp>
#include <ql/indexes/swapindex.hpp>
#include <ql/instruments/swap.hpp>
#include <ql/pricingengines/swap/discountingswapengine.hpp>
#include <ql/quote.hpp>
#include <ql/quotes/simplequote.hpp>
#include <ql/termstructures/volatility/optionlet/constantoptionletvol.hpp>
#include <ql/termstructures/volatility/optionlet/optionletvolatilitystructure.hpp>
#include <ql/time/daycounters/actual365fixed.hpp>
#include <ql/utilities/dataformatters.hpp>

#include "error.h"
#include "date_convert.h"

namespace quantra {

namespace {

/// Look up a curve in the registry and return its relinkable handle.
const QuantLib::RelinkableHandle<QuantLib::YieldTermStructure>& findCurve(
    const PricingRegistry& reg, const std::string& id) {
    auto it = reg.rates.curves.find(id);
    if (it == reg.rates.curves.end()) {
        QUANTRA_NOT_FOUND("Curve not found: " + id);
    }
    return *it->second;
}

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

/**
 * Build a CMS coupon pricer from plain params. Mirrors
 * cms_leg_parser.makeCouponPricer verbatim — kept inline here so the pricer
 * stays FB-free (cms_leg_parser's only entry point takes a SwapCmsLeg* and
 * is intended for the mapper).
 */
CmsPricerBuildResult buildCmsPricer(
    const VanillaSwapCmsPricerParams& params,
    const SwaptionVolEntry& volEntry,
    const QuantLib::Handle<QuantLib::YieldTermStructure>& discountCurve) {
    if (volEntry.handle.empty()) {
        QUANTRA_ERROR("CMS leg swaption vol handle is empty");
    }
    const auto qlYcModel = toYieldCurveModel(params.yieldCurveModel);
    if (!(params.meanReversion >= 0.0) || !std::isfinite(params.meanReversion)) {
        QUANTRA_INVALID_ARGUMENT("CMS leg mean_reversion must be non-negative");
    }
    auto meanReversion = QuantLib::Handle<QuantLib::Quote>(
        QuantLib::ext::make_shared<QuantLib::SimpleQuote>(params.meanReversion));

    CmsPricerBuildResult result;
    result.used.pricerType = params.pricerType;
    result.used.yieldCurveModel = params.yieldCurveModel;
    result.used.meanReversion = params.meanReversion;

    switch (params.pricerType) {
        case quantra::enums::CmsPricerType_LinearTsr:
            result.pricer = std::make_shared<QuantLib::LinearTsrPricer>(
                volEntry.handle, meanReversion, discountCurve);
            return result;
        case quantra::enums::CmsPricerType_HaganAnalytic:
            result.pricer = std::make_shared<QuantLib::AnalyticHaganPricer>(
                volEntry.handle, qlYcModel, meanReversion);
            return result;
        case quantra::enums::CmsPricerType_HaganNumeric: {
            const double lowerLimit = params.haganLowerLimit;
            const double upperLimit = params.haganUpperLimit;
            const double precision = params.haganPrecision;
            const double hardUpperLimitRaw = params.haganHardUpperLimit;

            if (!std::isfinite(lowerLimit) || !std::isfinite(upperLimit) ||
                !(upperLimit > lowerLimit)) {
                QUANTRA_INVALID_ARGUMENT("CMS leg Hagan numeric requires upper_limit > lower_limit");
            }
            if (!std::isfinite(precision) || !(precision > 0.0)) {
                QUANTRA_INVALID_ARGUMENT("CMS leg hagan_precision must be positive");
            }

            const double hardUpperLimit = hardUpperLimitRaw > 0.0
                ? hardUpperLimitRaw
                : std::numeric_limits<double>::max();
            if (!std::isfinite(hardUpperLimit) || hardUpperLimit <= upperLimit) {
                QUANTRA_INVALID_ARGUMENT("CMS leg hagan_hard_upper_limit must be > hagan_upper_limit");
            }
            result.used.haganLowerLimit = lowerLimit;
            result.used.haganUpperLimit = upperLimit;
            result.used.haganPrecision = precision;
            result.used.haganHardUpperLimit = hardUpperLimit;
            result.pricer = std::make_shared<QuantLib::NumericHaganPricer>(
                volEntry.handle, qlYcModel, meanReversion,
                lowerLimit, upperLimit, precision, hardUpperLimit);
            return result;
        }
    }
    QUANTRA_INVALID_ARGUMENT("Unsupported CMS pricer type");
    return result;
}

/**
 * Pre-serialize a fixed-leg coupon to a plain flow record. Mirrors the
 * vanilla_swap_flow_builder fixed-leg path verbatim, including the gearing=1.0
 * default the FB schema bakes in for fixed coupons.
 */
void extractFixedLegFlows(
    const QuantLib::Leg& leg,
    const QuantLib::YieldTermStructure& discountCurve,
    const QuantLib::Date& asOf,
    std::vector<VanillaSwapFlowPlain>& out) {
    for (const auto& cf : leg) {
        auto coupon = std::dynamic_pointer_cast<QuantLib::FixedRateCoupon>(cf);
        if (!coupon || coupon->hasOccurred(asOf)) {
            continue;
        }
        VanillaSwapFlowPlain f;
        f.isFloating = false;
        f.paymentDate = DateToIso(coupon->date());
        f.accrualStartDate = DateToIso(coupon->accrualStartDate());
        f.accrualEndDate = DateToIso(coupon->accrualEndDate());
        f.amount = coupon->amount();
        f.accrualYearFraction = coupon->accrualPeriod();
        f.gearing = 1.0;
        f.discount = discountCurve.discount(coupon->date());
        f.presentValue = f.amount * f.discount;
        f.rate = coupon->rate();
        out.push_back(std::move(f));
    }
}

/**
 * Pre-serialize a floating-leg coupon (IBOR or CMS). Mirrors the
 * vanilla_swap_flow_builder floating-leg path: indexFixing/spread/rate plus
 * the CmsCoupon-specific cms_swap_rate field. The fixing
 * call is wrapped in try/catch to preserve the legacy NaN-on-fixing-error
 * fallback.
 */
void extractFloatingLegFlows(
    const QuantLib::Leg& leg,
    const QuantLib::YieldTermStructure& discountCurve,
    const QuantLib::Date& asOf,
    std::vector<VanillaSwapFlowPlain>& out) {
    for (const auto& cf : leg) {
        auto coupon = std::dynamic_pointer_cast<QuantLib::FloatingRateCoupon>(cf);
        if (!coupon || coupon->hasOccurred(asOf)) {
            continue;
        }
        VanillaSwapFlowPlain f;
        f.isFloating = true;
        f.paymentDate = DateToIso(coupon->date());
        f.accrualStartDate = DateToIso(coupon->accrualStartDate());
        f.accrualEndDate = DateToIso(coupon->accrualEndDate());
        f.amount = coupon->amount();
        f.accrualYearFraction = coupon->accrualPeriod();
        f.gearing = coupon->gearing();
        f.discount = discountCurve.discount(coupon->date());
        f.presentValue = f.amount * f.discount;
        f.fixingDate = DateToIso(coupon->fixingDate());

        double fixing = std::numeric_limits<double>::quiet_NaN();
        bool hasCmsSwapRate = false;
        double cmsSwapRate = 0.0;
        try {
            fixing = coupon->indexFixing();
            if (std::dynamic_pointer_cast<QuantLib::CmsCoupon>(coupon)) {
                hasCmsSwapRate = true;
                cmsSwapRate = fixing;
            }
        } catch (...) {
        }

        f.indexFixing = fixing;
        f.hasCmsSwapRate = hasCmsSwapRate;
        f.cmsSwapRate = cmsSwapRate;
        f.spread = coupon->spread();
        f.rate = coupon->rate();
        out.push_back(std::move(f));
    }
}

/// Price the constant-notional IBOR branch: build the QL::VanillaSwap, set
/// engine, populate fairRate/fairSpread which only exist on VanillaSwap.
VanillaSwapPerSwap priceIborConstant(
    const VanillaSwapTrade& trade,
    const std::shared_ptr<QuantLib::IborIndex>& iborIndex,
    const QuantLib::RelinkableHandle<QuantLib::YieldTermStructure>& discHandle,
    const PricingContext& ctx,
    bool includeFlows) {
    if (trade.fixed.notional != trade.ibor.notional) {
        // Mismatched notionals are not an error: the fixed leg's notional wins.
        std::cout << "Warning: Fixed and floating notionals differ. Using fixed notional."
                  << std::endl;
    }

    auto swap = std::make_shared<QuantLib::VanillaSwap>(
        trade.swapType,
        trade.fixed.notional,
        trade.fixed.schedule,
        trade.fixed.rate,
        trade.fixed.dayCounter,
        trade.ibor.schedule,
        iborIndex,
        trade.ibor.spread,
        trade.ibor.dayCounter);
    swap->setPricingEngine(
        std::make_shared<QuantLib::DiscountingSwapEngine>(discHandle));

    VanillaSwapPerSwap out;
    out.npv = swap->NPV();
    out.fairRate = swap->fairRate();
    out.fairSpread = swap->fairSpread();
    out.fixedLegNpv = swap->fixedLegNPV();
    out.floatingLegNpv = swap->floatingLegNPV();
    out.fixedLegBps = swap->fixedLegBPS();
    out.floatingLegBps = swap->floatingLegBPS();
    out.hasCmsLeg = false;
    out.includeFlows = includeFlows;

    if (includeFlows) {
        auto discountCurve = discHandle.currentLink();
        extractFixedLegFlows(swap->fixedLeg(), *discountCurve, ctx.asOf, out.fixedFlows);
        extractFloatingLegFlows(swap->floatingLeg(), *discountCurve, ctx.asOf, out.floatingFlows);
    }
    return out;
}

/// Price the amortizing/step-up IBOR branch. QuantLib::VanillaSwap cannot carry
/// per-period notionals, so both legs are assembled explicitly with
/// .withNotionals() and priced as a generic QuantLib::Swap — exactly how
/// QuantLib composes amortizing swaps. The fixed leg, floating leg and payment
/// adjustments mirror what VanillaSwap builds internally (both legs use the
/// floating schedule's business-day convention), so an all-equal notionals
/// vector reproduces the VanillaSwap result bit-for-bit. fairRate/fairSpread
/// are recomputed with VanillaSwap's own formula from the leg BPS values.
VanillaSwapPerSwap priceIborAmortizing(
    const VanillaSwapTrade& trade,
    const std::shared_ptr<QuantLib::IborIndex>& iborIndex,
    const QuantLib::RelinkableHandle<QuantLib::YieldTermStructure>& discHandle,
    const PricingContext& ctx,
    bool includeFlows) {
    // A leg with an explicit per-period notionals vector uses it; a leg without
    // one falls back to a single-element vector = its constant scalar notional.
    const std::vector<double> fixedNotionals =
        trade.fixed.notionals.empty()
            ? std::vector<double>{trade.fixed.notional}
            : trade.fixed.notionals;
    const std::vector<double> iborNotionals =
        trade.ibor.notionals.empty()
            ? std::vector<double>{trade.ibor.notional}
            : trade.ibor.notionals;

    // VanillaSwap adjusts BOTH legs with the floating schedule's convention when
    // no explicit payment convention is passed (as the constant path does).
    const QuantLib::BusinessDayConvention paymentConvention =
        trade.ibor.schedule.businessDayConvention();

    QuantLib::Leg fixedLeg = QuantLib::FixedRateLeg(trade.fixed.schedule)
        .withNotionals(fixedNotionals)
        .withCouponRates(trade.fixed.rate, trade.fixed.dayCounter)
        .withPaymentAdjustment(paymentConvention);

    QuantLib::Leg iborLeg = QuantLib::IborLeg(trade.ibor.schedule, iborIndex)
        .withNotionals(iborNotionals)
        .withPaymentDayCounter(trade.ibor.dayCounter)
        .withPaymentAdjustment(paymentConvention)
        .withSpreads(trade.ibor.spread);

    // Plain (non-capped, non-in-arrears) IBOR coupons need a pricer to compute
    // their forward rate; a zero-vol Black pricer reproduces VanillaSwap's own
    // internal pricing (no convexity/timing adjustment is triggered).
    auto optionletVol = std::make_shared<QuantLib::ConstantOptionletVolatility>(
        0, iborIndex->fixingCalendar(), QuantLib::ModifiedFollowing, 0.0,
        QuantLib::Actual365Fixed());
    auto iborPricer = std::make_shared<QuantLib::BlackIborCouponPricer>();
    iborPricer->setCapletVolatility(
        QuantLib::Handle<QuantLib::OptionletVolatilityStructure>(optionletVol));
    QuantLib::setCouponPricer(iborLeg, iborPricer);

    const bool payerFixed = (trade.swapType == QuantLib::VanillaSwap::Payer);
    std::vector<QuantLib::Leg> legs{fixedLeg, iborLeg};
    std::vector<bool> payer{payerFixed, !payerFixed};
    auto swap = std::make_shared<QuantLib::Swap>(legs, payer);
    swap->setPricingEngine(
        std::make_shared<QuantLib::DiscountingSwapEngine>(discHandle));

    VanillaSwapPerSwap out;
    out.npv = swap->NPV();
    out.fixedLegNpv = swap->legNPV(0);
    out.floatingLegNpv = swap->legNPV(1);
    out.fixedLegBps = swap->legBPS(0);
    out.floatingLegBps = swap->legBPS(1);

    // fairRate/fairSpread via VanillaSwap's own formula (basisPoint = 1e-4):
    //   fairRate   = fixedRate - NPV / (fixedLegBPS / basisPoint)
    //   fairSpread = spread    - NPV / (floatLegBPS / basisPoint)
    // The leg BPS values already carry the payer sign, so this matches
    // VanillaSwap exactly for an all-equal notionals vector.
    constexpr double basisPoint = 1.0e-4;
    out.fairRate = (out.fixedLegBps != 0.0)
        ? trade.fixed.rate - out.npv / (out.fixedLegBps / basisPoint)
        : 0.0;
    out.fairSpread = (out.floatingLegBps != 0.0)
        ? trade.ibor.spread - out.npv / (out.floatingLegBps / basisPoint)
        : 0.0;

    out.hasCmsLeg = false;
    out.includeFlows = includeFlows;

    if (includeFlows) {
        auto discountCurve = discHandle.currentLink();
        extractFixedLegFlows(fixedLeg, *discountCurve, ctx.asOf, out.fixedFlows);
        extractFloatingLegFlows(iborLeg, *discountCurve, ctx.asOf, out.floatingFlows);
    }
    return out;
}

/// Price the IBOR branch, dispatching to the constant-notional VanillaSwap path
/// or the amortizing generic-Swap path based on the presence of per-period
/// notionals on either leg.
VanillaSwapPerSwap priceIborBranch(
    const VanillaSwapTrade& trade,
    const PricingRegistry& reg,
    const PricingContext& ctx,
    bool includeFlows) {
    const auto& discHandle = findCurve(reg, trade.discountingCurveId);
    const auto& fwdHandle = findCurve(reg, trade.forwardingCurveId);

    auto iborIndex = reg.rates.indices.getIborWithCurve(
        trade.ibor.indexId,
        QuantLib::Handle<QuantLib::YieldTermStructure>(fwdHandle.currentLink()));

    const bool amortizing =
        !trade.fixed.notionals.empty() || !trade.ibor.notionals.empty();
    if (amortizing) {
        return priceIborAmortizing(trade, iborIndex, discHandle, ctx, includeFlows);
    }
    return priceIborConstant(trade, iborIndex, discHandle, ctx, includeFlows);
}

/// Price the CMS branch: assemble both legs manually, attach a CMS coupon
/// evaluator, then a generic Swap (CMS lacks fairRate/fairSpread, by design).
VanillaSwapPerSwap priceCmsBranch(
    const VanillaSwapTrade& trade,
    const PricingRegistry& reg,
    const PricingContext& ctx,
    bool includeFlows) {
    const auto& discHandle = findCurve(reg, trade.discountingCurveId);
    const auto& fwdHandle = findCurve(reg, trade.forwardingCurveId);

    auto volIt = reg.volatility.swaptionVols.find(trade.cms.swaptionVolId);
    if (volIt == reg.volatility.swaptionVols.end()) {
        QUANTRA_NOT_FOUND("CMS leg swaption vol not found: " + trade.cms.swaptionVolId);
    }
    if (volIt->second.referenceDate != ctx.asOf) {
        QUANTRA_INVALID_ARGUMENT(
            "Strict mode: pricing.as_of_date must equal CMS swaption vol referenceDate for vol '" +
            trade.cms.swaptionVolId + "'");
    }

    QuantLib::Leg fixedLeg = QuantLib::FixedRateLeg(trade.fixed.schedule)
        .withNotionals(trade.fixed.notional)
        .withCouponRates(trade.fixed.rate, trade.fixed.dayCounter)
        .withPaymentAdjustment(trade.fixed.paymentConvention);

    QuantLib::Handle<QuantLib::YieldTermStructure> fwdH(fwdHandle.currentLink());
    QuantLib::Handle<QuantLib::YieldTermStructure> discH(discHandle.currentLink());

    auto swapIndex = reg.rates.swapIndices.getIborSwapIndexWithCurves(
        trade.cms.swapIndexId, trade.cms.swapTenor,
        reg.rates.indices, fwdH, discH);

    QuantLib::CmsLeg cmsBuilder(trade.cms.schedule, swapIndex);
    cmsBuilder.withNotionals(trade.cms.notional);
    cmsBuilder.withPaymentDayCounter(trade.cms.dayCounter);
    cmsBuilder.withPaymentAdjustment(trade.cms.paymentConvention);
    cmsBuilder.withGearings(trade.cms.gear);
    cmsBuilder.withSpreads(trade.cms.spread);
    if (trade.cms.fixingDays >= 0) {
        cmsBuilder.withFixingDays(static_cast<QuantLib::Natural>(trade.cms.fixingDays));
    } else {
        const auto& runtime = reg.rates.swapIndices.get(trade.cms.swapIndexId);
        cmsBuilder.withFixingDays(static_cast<QuantLib::Natural>(runtime.spotDays));
    }
    // Optional per-coupon cap/floor. When present, QuantLib emits
    // CappedFlooredCmsCoupons which price through the same coupon pricer set below.
    if (trade.cms.cap.has_value()) {
        cmsBuilder.withCaps(trade.cms.cap.value());
    }
    if (trade.cms.floor.has_value()) {
        cmsBuilder.withFloors(trade.cms.floor.value());
    }
    QuantLib::Leg cmsLeg = cmsBuilder;

    auto pricerBuild = buildCmsPricer(trade.cms.pricerParams, volIt->second, discH);
    QuantLib::setCouponPricer(cmsLeg, pricerBuild.pricer);

    const bool payerFixed = (trade.swapType == QuantLib::VanillaSwap::Payer);
    std::vector<QuantLib::Leg> legs{fixedLeg, cmsLeg};
    std::vector<bool> payer{payerFixed, !payerFixed};
    auto swap = std::make_shared<QuantLib::Swap>(legs, payer);
    swap->setPricingEngine(
        std::make_shared<QuantLib::DiscountingSwapEngine>(discHandle));

    VanillaSwapPerSwap out;
    out.npv = swap->NPV();
    // legNPV(0)/legBPS(0) is fixed leg, (1) is CMS leg — matches the original
    // pricing service's mapping.
    out.fixedLegNpv = swap->legNPV(0);
    out.floatingLegNpv = swap->legNPV(1);
    out.fixedLegBps = swap->legBPS(0);
    out.floatingLegBps = swap->legBPS(1);
    // fair_rate/fair_spread are not defined for a CMS swap; the legacy
    // pricing service emitted 0.0 here, so keep parity.
    out.fairRate = 0.0;
    out.fairSpread = 0.0;
    out.hasCmsLeg = true;
    out.cmsUsed = pricerBuild.used;
    out.includeFlows = includeFlows;

    if (includeFlows) {
        auto discountCurve = discHandle.currentLink();
        extractFixedLegFlows(fixedLeg, *discountCurve, ctx.asOf, out.fixedFlows);
        extractFloatingLegFlows(cmsLeg, *discountCurve, ctx.asOf, out.floatingFlows);
    }
    return out;
}

} // namespace

VanillaSwapResult VanillaSwapEvaluator::evaluate(
    const VanillaSwapInputs& inputs,
    const PricingRegistry& reg,
    const PricingContext& ctx) const {

    VanillaSwapResult result;
    result.swaps.reserve(inputs.trades.size());
    for (const auto& trade : inputs.trades) {
        ctx.budget.check();  // honor the per-request deadline before each trade
        if (trade.branch == VanillaSwapTrade::Branch::Ibor) {
            result.swaps.push_back(priceIborBranch(trade, reg, ctx, inputs.includeFlows));
        } else {
            result.swaps.push_back(priceCmsBranch(trade, reg, ctx, inputs.includeFlows));
        }
    }
    return result;
}

} // namespace quantra
