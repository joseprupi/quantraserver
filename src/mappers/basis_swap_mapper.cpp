#include "basis_swap_mapper.h"

#include "schedule_parser.h"
#include "enum_convert.h"
#include "error.h"
#include "leg_notionals.h"

namespace quantra {

namespace {

BasisSwapFloatingLegData extractLeg(const quantra::SwapFloatingLeg* fb,
                                    const char* legName,
                                    ScheduleParser& scheduleParser) {
    BasisSwapFloatingLegData leg;
    if (!fb->day_counter().has_value()) {
        QUANTRA_INVALID_ARGUMENT("SwapFloatingLeg.day_counter is required");
    }
    if (!fb->payment_convention().has_value()) {
        QUANTRA_INVALID_ARGUMENT("SwapFloatingLeg.payment_convention is required");
    }
    // The basis swap path does not support per-period notionals yet.
    rejectUnsupportedNotionals(fb->notionals(),
                               std::string("BasisSwap ") + legName);
    auto schedule = scheduleParser.parse(fb->schedule());
    leg.schedule = *schedule;
    leg.notional = fb->notional();
    leg.indexId = fb->index()->id()->str();
    leg.spread = fb->spread();
    leg.dayCounter = DayCounterToQL(fb->day_counter().value());
    leg.paymentConvention = ConventionToQL(fb->payment_convention().value());
    leg.fixingDays = fb->fixing_days();
    leg.inArrears = fb->in_arrears();
    (void)legName;
    return leg;
}

BasisSwapTrade extractTrade(const quantra::PriceBasisSwap* pricing) {
    if (pricing == nullptr) {
        QUANTRA_INVALID_ARGUMENT("PriceBasisSwap entry is null");
    }
    if (!pricing->basis_swap()) {
        QUANTRA_INVALID_ARGUMENT("PriceBasisSwap.basis_swap is required");
    }
    if (!pricing->discounting_curve() || pricing->discounting_curve()->str().empty()) {
        QUANTRA_INVALID_ARGUMENT("PriceBasisSwap.discounting_curve is required");
    }
    if (!pricing->forwarding_curve_leg1() || pricing->forwarding_curve_leg1()->str().empty()) {
        QUANTRA_INVALID_ARGUMENT("PriceBasisSwap.forwarding_curve_leg1 is required");
    }
    if (!pricing->forwarding_curve_leg2() || pricing->forwarding_curve_leg2()->str().empty()) {
        QUANTRA_INVALID_ARGUMENT("PriceBasisSwap.forwarding_curve_leg2 is required");
    }

    const auto* swap = pricing->basis_swap();
    if (!swap->leg1()) {
        QUANTRA_INVALID_ARGUMENT("BasisSwap.leg1 is required");
    }
    if (!swap->leg2()) {
        QUANTRA_INVALID_ARGUMENT("BasisSwap.leg2 is required");
    }

    const auto* leg1Fb = swap->leg1();
    if (!leg1Fb->schedule()) {
        QUANTRA_INVALID_ARGUMENT("BasisSwap.leg1.schedule is required");
    }
    if (!leg1Fb->index() || !leg1Fb->index()->id()) {
        QUANTRA_INVALID_ARGUMENT("BasisSwap.leg1.index.id is required");
    }

    const auto* leg2Fb = swap->leg2();
    if (!leg2Fb->schedule()) {
        QUANTRA_INVALID_ARGUMENT("BasisSwap.leg2.schedule is required");
    }
    if (!leg2Fb->index() || !leg2Fb->index()->id()) {
        QUANTRA_INVALID_ARGUMENT("BasisSwap.leg2.index.id is required");
    }

    ScheduleParser scheduleParser;

    BasisSwapTrade trade;
    trade.swapType = SwapTypeToQL(swap->swap_type());
    trade.discountingCurveId = pricing->discounting_curve()->str();
    trade.forwardingCurveLeg1Id = pricing->forwarding_curve_leg1()->str();
    trade.forwardingCurveLeg2Id = pricing->forwarding_curve_leg2()->str();
    trade.leg1 = extractLeg(leg1Fb, "leg1", scheduleParser);
    trade.leg2 = extractLeg(leg2Fb, "leg2", scheduleParser);
    return trade;
}

flatbuffers::Offset<quantra::SwapLegFlow> serializeFlow(
    flatbuffers::grpc::MessageBuilder& builder,
    const BasisSwapFlowPlain& f) {
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
    fb.add_accrual_year_fraction(f.accrualYearFraction);
    fb.add_amount(f.amount);
    fb.add_discount(f.discount);
    fb.add_present_value(f.presentValue);
    fb.add_rate(f.rate);
    if (f.isFloating) {
        fb.add_fixing_date(fixingDate);
        fb.add_index_fixing(f.indexFixing);
        fb.add_spread(f.spread);
    }
    return fb.Finish();
}

} // namespace

BasisSwapInputs BasisSwapMapper::toInputs(const quantra::PriceBasisSwapRequest* req) const {
    if (req == nullptr) {
        QUANTRA_INVALID_ARGUMENT("PriceBasisSwapRequest is null");
    }
    const auto* swaps = req->swaps();
    if (swaps == nullptr || swaps->size() == 0) {
        QUANTRA_INVALID_ARGUMENT(
            "PriceBasisSwapRequest.swaps is required and must be non-empty");
    }

    BasisSwapInputs inputs;
    inputs.includeFlows = req->include_flows();
    inputs.trades.reserve(swaps->size());
    for (auto it = swaps->begin(); it != swaps->end(); ++it) {
        inputs.trades.push_back(extractTrade(*it));
    }
    return inputs;
}

flatbuffers::Offset<quantra::PriceBasisSwapResponse> BasisSwapMapper::toResponse(
    flatbuffers::grpc::MessageBuilder& builder,
    const BasisSwapResult& result) const {

    std::vector<flatbuffers::Offset<quantra::BasisSwapResponse>> swapsVector;
    swapsVector.reserve(result.swaps.size());

    for (const auto& swap : result.swaps) {
        std::vector<flatbuffers::Offset<quantra::SwapLegFlow>> leg1FlowOffsets;
        std::vector<flatbuffers::Offset<quantra::SwapLegFlow>> leg2FlowOffsets;
        flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<quantra::SwapLegFlow>>>
            leg1Flows = 0;
        flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<quantra::SwapLegFlow>>>
            leg2Flows = 0;
        if (swap.includeFlows) {
            leg1FlowOffsets.reserve(swap.leg1Flows.size());
            for (const auto& f : swap.leg1Flows) {
                leg1FlowOffsets.push_back(serializeFlow(builder, f));
            }
            leg2FlowOffsets.reserve(swap.leg2Flows.size());
            for (const auto& f : swap.leg2Flows) {
                leg2FlowOffsets.push_back(serializeFlow(builder, f));
            }
            leg1Flows = builder.CreateVector(leg1FlowOffsets);
            leg2Flows = builder.CreateVector(leg2FlowOffsets);
        }

        quantra::BasisSwapResponseBuilder rb(builder);
        rb.add_npv(swap.npv);
        rb.add_leg1_bps(swap.leg1Bps);
        rb.add_leg2_bps(swap.leg2Bps);
        rb.add_leg1_npv(swap.leg1Npv);
        rb.add_leg2_npv(swap.leg2Npv);
        rb.add_fair_spread_leg1(swap.fairSpreadLeg1);
        rb.add_fair_spread_leg2(swap.fairSpreadLeg2);
        if (swap.includeFlows) {
            rb.add_leg1_flows(leg1Flows);
            rb.add_leg2_flows(leg2Flows);
        }
        swapsVector.push_back(rb.Finish());
    }

    auto swapsVec = builder.CreateVector(swapsVector);
    quantra::PriceBasisSwapResponseBuilder rb(builder);
    rb.add_swaps(swapsVec);
    return rb.Finish();
}

} // namespace quantra
