#include "basis_swap_parser.h"

#include <ql/cashflows/iborcoupon.hpp>

std::shared_ptr<QuantLib::Swap> BasisSwapParser::parse(
    const quantra::BasisSwap* swap,
    const quantra::IndexRegistry& indices)
{
    if (!swap) {
        QUANTRA_ERROR("BasisSwap not found");
    }
    if (!swap->leg1()) {
        QUANTRA_ERROR("BasisSwap leg1 not found");
    }
    if (!swap->leg2()) {
        QUANTRA_ERROR("BasisSwap leg2 not found");
    }
    if (!swap->leg1()->schedule() || !swap->leg2()->schedule()) {
        QUANTRA_ERROR("BasisSwap leg schedule not found");
    }
    if (!swap->leg1()->index() || !swap->leg1()->index()->id()) {
        QUANTRA_ERROR("BasisSwap leg1 index.id is required");
    }
    if (!swap->leg2()->index() || !swap->leg2()->index()->id()) {
        QUANTRA_ERROR("BasisSwap leg2 index.id is required");
    }

    ScheduleParser scheduleParser;
    auto sch1 = scheduleParser.parse(swap->leg1()->schedule());
    auto sch2 = scheduleParser.parse(swap->leg2()->schedule());

    const auto* leg1 = swap->leg1();
    const auto* leg2 = swap->leg2();

    auto idx1 = indices.getIborWithCurve(leg1->index()->id()->str(), forwarding_leg1_);
    auto idx2 = indices.getIborWithCurve(leg2->index()->id()->str(), forwarding_leg2_);

    Leg qlLeg1 = IborLeg(*sch1, idx1)
        .withNotionals(leg1->notional())
        .withPaymentDayCounter(DayCounterToQL(leg1->day_counter()))
        .withPaymentAdjustment(ConventionToQL(leg1->payment_convention()))
        .withSpreads(leg1->spread())
        .withFixingDays(leg1->fixing_days())
        .inArrears(leg1->in_arrears());

    Leg qlLeg2 = IborLeg(*sch2, idx2)
        .withNotionals(leg2->notional())
        .withPaymentDayCounter(DayCounterToQL(leg2->day_counter()))
        .withPaymentAdjustment(ConventionToQL(leg2->payment_convention()))
        .withSpreads(leg2->spread())
        .withFixingDays(leg2->fixing_days())
        .inArrears(leg2->in_arrears());

    std::vector<Leg> legs = {qlLeg1, qlLeg2};
    std::vector<bool> payer = {true, false}; // pay leg1 / receive leg2
    if (swap->swap_type() == quantra::enums::SwapType_Receiver) {
        payer = {false, true}; // receive leg1 / pay leg2
    } else if (swap->swap_type() != quantra::enums::SwapType_Payer) {
        QUANTRA_ERROR("Invalid basis swap type");
    }

    return std::make_shared<QuantLib::Swap>(legs, payer);
}

void BasisSwapParser::linkForwardingTermStructures(
    std::shared_ptr<YieldTermStructure> leg1,
    std::shared_ptr<YieldTermStructure> leg2)
{
    forwarding_leg1_.linkTo(leg1);
    forwarding_leg2_.linkTo(leg2);
}
