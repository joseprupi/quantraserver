#include "swap_leg_pricer.h"

#include "error.h"
#include "swaption_vol_runtime.h"

#include <ql/cashflows/cashflows.hpp>
#include <ql/cashflows/cmscoupon.hpp>
#include <ql/cashflows/conundrumpricer.hpp>
#include <ql/cashflows/lineartsrpricer.hpp>
#include <ql/cashflows/simplecashflow.hpp>
#include <ql/indexes/iborindex.hpp>
#include <ql/indexes/swapindex.hpp>
#include <ql/quotes/simplequote.hpp>

#include <cmath>
#include <limits>

namespace quantra {

namespace {

const QuantLib::RelinkableHandle<QuantLib::YieldTermStructure>& curveOrThrow(
    const PricingRegistry& reg, const std::string& id, const std::string& role) {
    auto it = reg.rates.curves.find(id);
    if (it == reg.rates.curves.end()) {
        QUANTRA_NOT_FOUND(role + " not found: " + id);
    }
    const auto& handle = *it->second;
    if (handle.empty()) {
        QUANTRA_ERROR(role + " '" + id + "' is not linked to a term structure");
    }
    return handle;
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

/// The coupon leg of a CMS `SwapLeg`: QuantLib::CmsLeg on the swap index
/// (family from `pricing.rates.swap_indices`, IBOR leg projected off the
/// leg's forwarding curve, discounted on the leg's discounting curve) with
/// the requested pricer attached. The swaption vol must be as of the
/// evaluation date (strict mode, as every vol surface). A SpreadFromATM cube
/// or a SABR surface is built here, as the swaption pricer builds it: on the
/// ATM forwards of the vol's own swap index, from the leg's two curves.
QuantLib::Leg buildCmsCoupons(
    const SwapLegData& leg,
    const std::string& name,
    const PricingRegistry& reg,
    const PricingContext& ctx,
    const QuantLib::Handle<QuantLib::YieldTermStructure>& forwarding,
    const QuantLib::Handle<QuantLib::YieldTermStructure>& discounting,
    CmsPricerUsed& used) {
    const CmsLegSpec& cms = leg.cms;
    auto volIt = reg.volatility.swaptionVols.find(cms.swaptionVolId);
    if (volIt == reg.volatility.swaptionVols.end()) {
        QUANTRA_NOT_FOUND(name + ".coupons.swaption_vol_id not found: " + cms.swaptionVolId);
    }
    if (volIt->second.referenceDate != ctx.asOf) {
        QUANTRA_INVALID_ARGUMENT(
            "Strict mode: pricing.as_of_date must equal CMS swaption vol referenceDate for vol '" +
            cms.swaptionVolId + "'");
    }

    auto swapIndex = reg.rates.swapIndices.getIborSwapIndexWithCurves(
        cms.swapIndexId, cms.swapTenor, reg.rates.indices, forwarding, discounting);

    QuantLib::CmsLeg builder(cms.schedule, swapIndex);
    builder.withNotionals(cms.notional);
    builder.withPaymentDayCounter(cms.dayCounter);
    builder.withPaymentAdjustment(cms.paymentConvention);
    builder.withGearings(cms.gear);
    builder.withSpreads(cms.spread);
    if (cms.fixingDays >= 0) {
        builder.withFixingDays(static_cast<QuantLib::Natural>(cms.fixingDays));
    } else {
        const auto& runtime = reg.rates.swapIndices.get(cms.swapIndexId);
        builder.withFixingDays(static_cast<QuantLib::Natural>(runtime.spotDays));
    }
    // Optional per-coupon cap / floor: QuantLib emits CappedFlooredCmsCoupons
    // that price through the same coupon pricer.
    if (cms.cap.has_value()) builder.withCaps(cms.cap.value());
    if (cms.floor.has_value()) builder.withFloors(cms.floor.value());
    QuantLib::Leg coupons = builder;

    // Builds the handle of a SpreadFromATM cube or a SABR surface; any other
    // kind comes back unchanged (its handle is built at parse time).
    const SwaptionVolEntry volEntry = finalizeSwaptionVolEntryForPricing(
        volIt->second, reg, discounting, forwarding, /*forceRecomputeAtm=*/false,
        leg.discountingCurveId, leg.forwardingCurveId);
    if (volEntry.handle.empty()) {
        QUANTRA_INVALID_ARGUMENT(
            name + ".coupons.swaption_vol_id '" + cms.swaptionVolId + "' is a " +
            quantra::enums::EnumNameSwaptionVolKind(volEntry.volKind) +
            " swaption vol, which CMS coupons do not support");
    }
    auto pricerBuild = BuildCmsPricer(cms.pricerParams, volEntry, discounting);
    QuantLib::setCouponPricer(coupons, pricerBuild.pricer);
    used = pricerBuild.used;
    return coupons;
}

/// The coupon leg of `leg` on its forwarding index. The index kind must match
/// the coupon kind: a compounded overnight coupon on an IBOR index (or an
/// IBOR coupon on an overnight index) is a request error naming the table to
/// use, never a silently different instrument.
QuantLib::Leg buildCoupons(
    const SwapLegData& leg,
    const std::string& name,
    const PricingRegistry& reg,
    const PricingContext& ctx,
    const QuantLib::Handle<QuantLib::YieldTermStructure>& discounting,
    BuiltSwapLeg& out) {
    if (leg.kind == SwapLegData::Kind::Fixed) {
        return BuildFixedRateLeg(leg.fixed);
    }
    const auto& fwdHandle = curveOrThrow(reg, leg.forwardingCurveId, name + " forwarding curve");
    QuantLib::Handle<QuantLib::YieldTermStructure> fwd(fwdHandle.currentLink());

    if (leg.kind == SwapLegData::Kind::Cms) {
        CmsPricerUsed used;
        QuantLib::Leg coupons = buildCmsCoupons(leg, name, reg, ctx, fwd, discounting, used);
        out.cmsUsed = used;
        return coupons;
    }

    if (!reg.rates.indices.has(leg.indexId)) {
        QUANTRA_NOT_FOUND("Index not found: " + leg.indexId);
    }
    auto base = reg.rates.indices.get(leg.indexId);
    // The leg currency is explicit on the wire and must be the index's own:
    // the engine never infers a currency from an index id.
    if (base->currency().code() != leg.currency) {
        QUANTRA_INVALID_ARGUMENT(
            name + ".currency '" + leg.currency + "' does not match the currency " +
            base->currency().code() + " of its index '" + leg.indexId + "' (IndexDef.currency)");
    }
    const bool overnightIndex =
        std::dynamic_pointer_cast<QuantLib::OvernightIndex>(base) != nullptr;
    if (leg.kind == SwapLegData::Kind::Ibor) {
        if (overnightIndex) {
            QUANTRA_INVALID_ARGUMENT(
                name + ".coupons is IborCoupons but its index '" + leg.indexId +
                "' is an overnight index: use OvernightCoupons (daily fixings compounded over "
                "each period), IborCoupons takes an IBOR index");
        }
        return BuildIborLeg(leg.ibor, reg.rates.indices.getIborWithCurve(leg.indexId, fwd));
    }
    if (!overnightIndex) {
        QUANTRA_INVALID_ARGUMENT(
            name + ".coupons is OvernightCoupons but its index '" + leg.indexId +
            "' is not an overnight index: use IborCoupons (one fixing per period), "
            "OvernightCoupons takes an overnight index");
    }
    return BuildOvernightLeg(
        leg.overnight, reg.rates.indices.getOvernightWithCurve(leg.indexId, fwd));
}

} // namespace

CmsPricerBuildResult BuildCmsPricer(
    const CmsPricerParams& params,
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
                QUANTRA_INVALID_ARGUMENT(
                    "CMS leg Hagan numeric requires upper_limit > lower_limit");
            }
            if (!std::isfinite(precision) || !(precision > 0.0)) {
                QUANTRA_INVALID_ARGUMENT("CMS leg hagan_precision must be positive");
            }

            const double hardUpperLimit =
                hardUpperLimitRaw > 0.0 ? hardUpperLimitRaw : std::numeric_limits<double>::max();
            if (!std::isfinite(hardUpperLimit) || hardUpperLimit <= upperLimit) {
                QUANTRA_INVALID_ARGUMENT(
                    "CMS leg hagan_hard_upper_limit must be > hagan_upper_limit");
            }
            result.used.haganLowerLimit = lowerLimit;
            result.used.haganUpperLimit = upperLimit;
            result.used.haganPrecision = precision;
            result.used.haganHardUpperLimit = hardUpperLimit;
            result.pricer = std::make_shared<QuantLib::NumericHaganPricer>(
                volEntry.handle, qlYcModel, meanReversion, lowerLimit, upperLimit, precision,
                hardUpperLimit);
            return result;
        }
    }
    QUANTRA_INVALID_ARGUMENT("Unsupported CMS pricer type");
    return result;
}

BuiltSwapLeg BuildSwapLeg(
    const SwapLegData& leg,
    const std::string& name,
    const PricingRegistry& reg,
    const PricingContext& ctx) {
    BuiltSwapLeg out;
    const auto& discHandle = curveOrThrow(reg, leg.discountingCurveId, name + " discounting curve");
    out.discount = discHandle.currentLink().get();
    QuantLib::Handle<QuantLib::YieldTermStructure> discounting(discHandle.currentLink());
    out.coupons = buildCoupons(leg, name, reg, ctx, discounting, out);
    if (out.coupons.empty()) {
        QUANTRA_INVALID_ARGUMENT(name + " schedule produces no coupon");
    }

    // Principal exchanges in the leg's own frame: the receiver of the coupons
    // pays the notional on the schedule's start date and gets it back with
    // the last coupon.
    out.full = out.coupons;
    if (leg.initialExchange) {
        out.full.push_back(
            std::make_shared<QuantLib::SimpleCashFlow>(
                -leg.notional, leg.schedule().dates().front()));
    }
    if (leg.finalExchange) {
        out.full.push_back(
            std::make_shared<QuantLib::SimpleCashFlow>(leg.notional, out.coupons.back()->date()));
    }

    // Valued exactly as QuantLib's DiscountingSwapEngine values a leg: as of
    // the discount curve's reference date (= the evaluation date), flows on
    // that date excluded (includeSettlementDateFlows = false).
    const QuantLib::Date valueDate = out.discount->referenceDate();
    out.npvOwn = QuantLib::CashFlows::npv(out.full, *out.discount, false, valueDate, valueDate);
    out.bpsOwn = QuantLib::CashFlows::bps(out.coupons, *out.discount, false, valueDate, valueDate);
    return out;
}

SwapLegPerLeg SignedLegResult(
    const SwapLegData& leg,
    const BuiltSwapLeg& built,
    const PricingContext& ctx,
    bool includeFlows) {
    SwapLegPerLeg out;
    out.npv = leg.sign() * built.npvOwn;
    out.npvReporting = out.npv;
    if (includeFlows) {
        ExtractSwapLegFlows(built.full, *built.discount, ctx.asOf, out.flows);
    }
    return out;
}

} // namespace quantra
