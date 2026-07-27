#include "vanilla_swap_mapper.h"

#include <cmath>

#include "schedule_parser.h"
#include "enum_convert.h"
#include "error.h"
#include "request_validation.h"

namespace quantra {

namespace {

/// Pull pricer params off the CMS leg. If the optional FB pricer table is
/// missing, the defaults baked into VanillaSwapCmsPricerParams stand in (which
/// match cms_leg_parser.makeCouponPricer's hand-coded defaults).
VanillaSwapCmsPricerParams extractCmsPricerParams(const quantra::SwapCmsLeg* leg) {
    VanillaSwapCmsPricerParams params;
    const auto* ps = leg->pricer();
    if (!ps) {
        return params;
    }
    params.pricerType = ps->pricer_type();
    params.yieldCurveModel = ps->yield_curve_model();
    params.meanReversion = ps->mean_reversion();
    params.haganLowerLimit = ps->hagan_lower_limit();
    params.haganUpperLimit = ps->hagan_upper_limit();
    params.haganPrecision = ps->hagan_precision();
    params.haganHardUpperLimit = ps->hagan_hard_upper_limit();
    return params;
}

VanillaSwapTrade extractTrade(const quantra::PriceVanillaSwap* pricing) {
    if (!pricing->vanilla_swap()) {
        QUANTRA_INVALID_ARGUMENT("PriceVanillaSwap.vanilla_swap is required");
    }
    if (!pricing->discounting_curve()) {
        QUANTRA_INVALID_ARGUMENT("PriceVanillaSwap.discounting_curve is required");
    }
    if (!pricing->forwarding_curve()) {
        QUANTRA_INVALID_ARGUMENT("PriceVanillaSwap.forwarding_curve is required");
    }

    const auto* swap = pricing->vanilla_swap();
    if (!swap->swap_type().has_value()) {
        QUANTRA_INVALID_ARGUMENT("VanillaSwap.swap_type is required");
    }
    if (!swap->fixed_leg()) {
        QUANTRA_INVALID_ARGUMENT("VanillaSwap.fixed_leg is required");
    }
    const auto* fixedFb = swap->fixed_leg();
    if (!fixedFb->schedule()) {
        QUANTRA_INVALID_ARGUMENT("VanillaSwap.fixed_leg.schedule is required");
    }
    if (!fixedFb->day_counter().has_value()) {
        QUANTRA_INVALID_ARGUMENT("SwapFixedLeg.day_counter is required");
    }
    if (!fixedFb->payment_convention().has_value()) {
        QUANTRA_INVALID_ARGUMENT("SwapFixedLeg.payment_convention is required");
    }

    const bool hasIbor = (swap->floating_leg() != nullptr);
    const bool hasCms = (swap->cms_leg() != nullptr);
    if (hasIbor == hasCms) {
        QUANTRA_INVALID_ARGUMENT(
            "VanillaSwap must contain exactly one of floating_leg or cms_leg");
    }

    ScheduleParser scheduleParser;

    VanillaSwapTrade trade;
    trade.swapType = SwapTypeToQL(swap->swap_type().value());
    trade.discountingCurveId = pricing->discounting_curve()->str();
    trade.forwardingCurveId = pricing->forwarding_curve()->str();

    auto fixedSchedule = scheduleParser.parse(fixedFb->schedule());
    trade.fixed.schedule = *fixedSchedule;
    trade.fixed.notional = requirePositive(fixedFb->notional(), "SwapFixedLeg.notional");
    trade.fixed.rate = fixedFb->rate();
    trade.fixed.dayCounter = DayCounterToQL(fixedFb->day_counter().value());
    trade.fixed.paymentConvention = ConventionToQL(fixedFb->payment_convention().value());

    if (hasIbor) {
        const auto* floatFb = swap->floating_leg();
        if (!floatFb->schedule()) {
            QUANTRA_INVALID_ARGUMENT("VanillaSwap.floating_leg.schedule is required");
        }
        if (!floatFb->index() || !floatFb->index()->id()) {
            QUANTRA_INVALID_ARGUMENT("VanillaSwap.floating_leg.index.id is required");
        }
        if (!floatFb->day_counter().has_value()) {
            QUANTRA_INVALID_ARGUMENT("SwapFloatingLeg.day_counter is required");
        }
        trade.branch = VanillaSwapTrade::Branch::Ibor;
        auto floatSchedule = scheduleParser.parse(floatFb->schedule());
        trade.ibor.schedule = *floatSchedule;
        trade.ibor.notional = requirePositive(floatFb->notional(), "SwapFloatingLeg.notional");
        trade.ibor.indexId = floatFb->index()->id()->str();
        trade.ibor.spread = floatFb->spread();
        trade.ibor.dayCounter = DayCounterToQL(floatFb->day_counter().value());
        // Honor the leg's fixing_days / in_arrears conventions (as the basis-swap
        // and floating-bond paths already do). in_arrears=true moves the fixing
        // to the end of the accrual period, which QuantLib::VanillaSwap cannot
        // express — the evaluator routes such trades to the manual-leg path.
        trade.ibor.fixingDays = floatFb->fixing_days();
        trade.ibor.inArrears = floatFb->in_arrears();
        // Optional amortizing/step-up notionals: one entry per coupon period on
        // each leg. Absent => the constant scalar notionals above stand.
        parseOptionalNotionals(fixedFb->notionals(),
                               trade.fixed.schedule.size() - 1,
                               "SwapFixedLeg", trade.fixed.notionals);
        parseOptionalNotionals(floatFb->notionals(),
                               trade.ibor.schedule.size() - 1,
                               "SwapFloatingLeg", trade.ibor.notionals);
        return trade;
    }

    // CMS branch does not support amortizing notionals yet: reject a present
    // notionals vector on the fixed leg rather than silently ignoring it.
    rejectUnsupportedNotionals(fixedFb->notionals(),
                               "VanillaSwap CMS leg fixed leg");

    // CMS branch.
    const auto* cmsFb = swap->cms_leg();
    if (!cmsFb->schedule()) {
        QUANTRA_INVALID_ARGUMENT("VanillaSwap.cms_leg.schedule is required");
    }
    if (!cmsFb->swap_index_id()) {
        QUANTRA_INVALID_ARGUMENT("CMS leg swap_index_id is required");
    }
    if (!cmsFb->swap_tenor()) {
        QUANTRA_INVALID_ARGUMENT("CMS leg swap_tenor is required");
    }
    if (!cmsFb->swaption_vol_id()) {
        QUANTRA_INVALID_ARGUMENT("CMS leg swaption_vol_id is required");
    }
    if (!cmsFb->day_counter().has_value()) {
        QUANTRA_INVALID_ARGUMENT("SwapCmsLeg.day_counter is required");
    }
    if (!cmsFb->payment_convention().has_value()) {
        QUANTRA_INVALID_ARGUMENT("SwapCmsLeg.payment_convention is required");
    }

    trade.branch = VanillaSwapTrade::Branch::Cms;
    auto cmsSchedule = scheduleParser.parse(cmsFb->schedule());
    trade.cms.schedule = *cmsSchedule;
    trade.cms.notional = requirePositive(cmsFb->notional(), "SwapCmsLeg.notional");
    trade.cms.swapIndexId = cmsFb->swap_index_id()->str();
    trade.cms.swapTenor = requirePeriod(cmsFb->swap_tenor(), "SwapCmsLeg.swap_tenor");
    trade.cms.swaptionVolId = cmsFb->swaption_vol_id()->str();
    trade.cms.fixingDays = cmsFb->fixing_days();
    trade.cms.dayCounter = DayCounterToQL(cmsFb->day_counter().value());
    trade.cms.paymentConvention = ConventionToQL(cmsFb->payment_convention().value());
    trade.cms.gear = cmsFb->gear();
    trade.cms.spread = cmsFb->spread();
    // Optional cap/floor strikes. Presence (not sign) decides: a floor of 0 is a
    // legitimate value. Applied to every coupon downstream via withCaps/withFloors.
    if (cmsFb->cap().has_value()) {
        const double cap = cmsFb->cap().value();
        if (!std::isfinite(cap)) {
            QUANTRA_INVALID_ARGUMENT("SwapCmsLeg.cap must be a finite number");
        }
        trade.cms.cap = cap;
    }
    if (cmsFb->floor().has_value()) {
        const double floor = cmsFb->floor().value();
        if (!std::isfinite(floor)) {
            QUANTRA_INVALID_ARGUMENT("SwapCmsLeg.floor must be a finite number");
        }
        trade.cms.floor = floor;
    }
    if (trade.cms.cap.has_value() && trade.cms.floor.has_value() &&
        trade.cms.cap.value() < trade.cms.floor.value()) {
        QUANTRA_INVALID_ARGUMENT("SwapCmsLeg.cap must be >= floor");
    }
    trade.cms.pricerParams = extractCmsPricerParams(cmsFb);
    return trade;
}

flatbuffers::Offset<quantra::SwapLegFlow> serializeFlow(
    flatbuffers::grpc::MessageBuilder& builder,
    const VanillaSwapFlowPlain& f) {
    auto paymentDate = builder.CreateString(f.paymentDate);
    auto accrualStart = builder.CreateString(f.accrualStartDate);
    auto accrualEnd = builder.CreateString(f.accrualEndDate);
    flatbuffers::Offset<flatbuffers::String> fixingDate = 0;
    if (f.isFloating) {
        fixingDate = builder.CreateString(f.fixingDate);
    }

    quantra::SwapLegFlowBuilder fb(builder);
    fb.add_payment_date(paymentDate);
    fb.add_accrual_start_date(accrualStart);
    fb.add_accrual_end_date(accrualEnd);
    fb.add_amount(f.amount);
    fb.add_accrual_year_fraction(f.accrualYearFraction);
    fb.add_gearing(f.gearing);
    fb.add_discount(f.discount);
    fb.add_present_value(f.presentValue);
    fb.add_rate(f.rate);
    if (f.isFloating) {
        fb.add_fixing_date(fixingDate);
        if (std::isfinite(f.indexFixing)) fb.add_index_fixing(f.indexFixing);
        // CMS swap-rate fixing is added only for CMS coupons; absent otherwise.
        if (f.hasCmsSwapRate && std::isfinite(f.cmsSwapRate)) {
            fb.add_cms_swap_rate(f.cmsSwapRate);
        }
        fb.add_spread(f.spread);
    }
    return fb.Finish();
}

} // namespace

VanillaSwapInputs VanillaSwapMapper::toInputs(
    const quantra::PriceVanillaSwapRequest* req) const {
    if (req == nullptr) {
        QUANTRA_INVALID_ARGUMENT("PriceVanillaSwapRequest is null");
    }
    const auto* swaps = req->swaps();
    if (swaps == nullptr || swaps->size() == 0) {
        QUANTRA_INVALID_ARGUMENT(
            "PriceVanillaSwapRequest.swaps is required and must be non-empty");
    }

    VanillaSwapInputs inputs;
    inputs.includeFlows = req->include_flows();
    inputs.trades.reserve(swaps->size());
    for (auto it = swaps->begin(); it != swaps->end(); ++it) {
        inputs.trades.push_back(extractTrade(*it));
    }
    return inputs;
}

flatbuffers::Offset<quantra::PriceVanillaSwapResponse> VanillaSwapMapper::toResponse(
    flatbuffers::grpc::MessageBuilder& builder,
    const VanillaSwapResult& result) const {

    std::vector<flatbuffers::Offset<quantra::VanillaSwapResponse>> swapsVector;
    swapsVector.reserve(result.swaps.size());

    for (const auto& swap : result.swaps) {
        std::vector<flatbuffers::Offset<quantra::SwapLegFlow>> fixedFlowOffsets;
        std::vector<flatbuffers::Offset<quantra::SwapLegFlow>> floatingFlowOffsets;
        if (swap.includeFlows) {
            fixedFlowOffsets.reserve(swap.fixedFlows.size());
            for (const auto& f : swap.fixedFlows) {
                fixedFlowOffsets.push_back(serializeFlow(builder, f));
            }
            floatingFlowOffsets.reserve(swap.floatingFlows.size());
            for (const auto& f : swap.floatingFlows) {
                floatingFlowOffsets.push_back(serializeFlow(builder, f));
            }
        }
        auto fixedFlows = builder.CreateVector(fixedFlowOffsets);
        auto floatingFlows = builder.CreateVector(floatingFlowOffsets);

        quantra::VanillaSwapResponseBuilder rb(builder);
        rb.add_npv(swap.npv);
        // fairRate/fairSpread divide by leg BPS; a zero-BPS leg (e.g. notional 0)
        // yields NaN/inf, which is not valid JSON. Omit rather than emit `nan`.
        if (std::isfinite(swap.fairRate)) rb.add_fair_rate(swap.fairRate);
        if (std::isfinite(swap.fairSpread)) rb.add_fair_spread(swap.fairSpread);
        rb.add_fixed_leg_bps(swap.fixedLegBps);
        rb.add_floating_leg_bps(swap.floatingLegBps);
        rb.add_fixed_leg_npv(swap.fixedLegNpv);
        rb.add_floating_leg_npv(swap.floatingLegNpv);
        if (swap.hasCmsLeg) {
            rb.add_used_cms_pricer_type(swap.cmsUsed.pricerType);
            rb.add_used_cms_yield_curve_model(swap.cmsUsed.yieldCurveModel);
            rb.add_used_cms_mean_reversion(swap.cmsUsed.meanReversion);
            rb.add_used_cms_hagan_lower_limit(swap.cmsUsed.haganLowerLimit);
            rb.add_used_cms_hagan_upper_limit(swap.cmsUsed.haganUpperLimit);
            rb.add_used_cms_hagan_precision(swap.cmsUsed.haganPrecision);
            rb.add_used_cms_hagan_hard_upper_limit(swap.cmsUsed.haganHardUpperLimit);
        }
        if (swap.includeFlows) {
            rb.add_fixed_leg_flows(fixedFlows);
            rb.add_floating_leg_flows(floatingFlows);
        }
        swapsVector.push_back(rb.Finish());
    }

    auto swapsVec = builder.CreateVector(swapsVector);
    quantra::PriceVanillaSwapResponseBuilder rb(builder);
    rb.add_swaps(swapsVec);
    return rb.Finish();
}

} // namespace quantra
