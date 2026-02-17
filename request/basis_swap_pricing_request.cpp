#include "basis_swap_pricing_request.h"

#include <ql/cashflow.hpp>
#include <ql/cashflows/coupon.hpp>
#include <ql/cashflows/floatingratecoupon.hpp>
#include <ql/pricingengines/swap/discountingswapengine.hpp>
#include <cmath>
#include <limits>

#include "basis_swap_parser.h"
#include "pricing_registry.h"

using namespace QuantLib;
using namespace quantra;

namespace {
flatbuffers::Offset<SwapLegFlow> BuildFloatingFlow(
    const std::shared_ptr<CashFlow>& cf,
    const std::shared_ptr<YieldTermStructure>& discountCurve,
    flatbuffers::grpc::MessageBuilder& builder,
    Date asOf) {
    auto coupon = std::dynamic_pointer_cast<Coupon>(cf);
    if (!coupon || coupon->hasOccurred(asOf)) {
        return 0;
    }

    std::ostringstream osPayment, osStart, osEnd;
    osPayment << QuantLib::io::iso_date(coupon->date());
    osStart << QuantLib::io::iso_date(coupon->accrualStartDate());
    osEnd << QuantLib::io::iso_date(coupon->accrualEndDate());

    double discount = discountCurve->discount(coupon->date());
    double amount = coupon->amount();

    SwapLegFlowBuilder fb(builder);
    fb.add_payment_date(builder.CreateString(osPayment.str()));
    fb.add_accrual_start_date(builder.CreateString(osStart.str()));
    fb.add_accrual_end_date(builder.CreateString(osEnd.str()));
    fb.add_amount(amount);
    fb.add_discount(discount);
    fb.add_present_value(amount * discount);
    fb.add_rate(coupon->rate());

    auto frc = std::dynamic_pointer_cast<FloatingRateCoupon>(coupon);
    if (frc) {
        std::ostringstream osFix;
        osFix << QuantLib::io::iso_date(frc->fixingDate());
        fb.add_fixing_date(builder.CreateString(osFix.str()));
        fb.add_index_fixing(frc->indexFixing());
        fb.add_spread(frc->spread());
    }
    return fb.Finish();
}
} // namespace

flatbuffers::Offset<PriceBasisSwapResponse> BasisSwapPricingRequest::request(
    std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder,
    const PriceBasisSwapRequest* request) const {
    PricingRegistryBuilder regBuilder;
    PricingRegistry reg = regBuilder.build(request->pricing());

    BasisSwapParser parser;
    Date asOf = Settings::instance().evaluationDate();
    bool includeFlows = request->include_flows();

    std::vector<flatbuffers::Offset<BasisSwapResponse>> out;
    for (auto it = request->swaps()->begin(); it != request->swaps()->end(); ++it) {
        auto discIt = reg.curves.find(it->discounting_curve()->str());
        if (discIt == reg.curves.end()) {
            QUANTRA_ERROR("Discounting curve not found: " + it->discounting_curve()->str());
        }
        auto fwd1It = reg.curves.find(it->forwarding_curve_leg1()->str());
        if (fwd1It == reg.curves.end()) {
            QUANTRA_ERROR("Forwarding curve leg1 not found: " + it->forwarding_curve_leg1()->str());
        }
        auto fwd2It = reg.curves.find(it->forwarding_curve_leg2()->str());
        if (fwd2It == reg.curves.end()) {
            QUANTRA_ERROR("Forwarding curve leg2 not found: " + it->forwarding_curve_leg2()->str());
        }

        parser.linkForwardingTermStructures(
            fwd1It->second->currentLink(),
            fwd2It->second->currentLink());
        auto swap = parser.parse(it->basis_swap(), reg.indices);
        swap->setPricingEngine(std::make_shared<DiscountingSwapEngine>(*discIt->second));

        std::vector<flatbuffers::Offset<SwapLegFlow>> leg1Flows;
        std::vector<flatbuffers::Offset<SwapLegFlow>> leg2Flows;
        flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<SwapLegFlow>>> leg1FlowsOff = 0;
        flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<SwapLegFlow>>> leg2FlowsOff = 0;
        if (includeFlows) {
            auto discountCurve = discIt->second->currentLink();
            for (const auto& cf : swap->leg(0)) {
                auto flow = BuildFloatingFlow(cf, discountCurve, *builder, asOf);
                if (flow.o != 0) leg1Flows.push_back(flow);
            }
            for (const auto& cf : swap->leg(1)) {
                auto flow = BuildFloatingFlow(cf, discountCurve, *builder, asOf);
                if (flow.o != 0) leg2Flows.push_back(flow);
            }
            leg1FlowsOff = builder->CreateVector(leg1Flows);
            leg2FlowsOff = builder->CreateVector(leg2Flows);
        }

        BasisSwapResponseBuilder rb(*builder);
        const double npv = swap->NPV();
        const double leg1Bps = swap->legBPS(0);
        const double leg2Bps = swap->legBPS(1);
        rb.add_npv(npv);
        rb.add_leg1_bps(leg1Bps);
        rb.add_leg2_bps(leg2Bps);
        rb.add_leg1_npv(swap->legNPV(0));
        rb.add_leg2_npv(swap->legNPV(1));
        const double currentSpread1 = it->basis_swap()->leg1()->spread();
        const double currentSpread2 = it->basis_swap()->leg2()->spread();
        const double fairSpreadLeg1 = std::fabs(leg1Bps) > 1e-16
            ? (currentSpread1 - npv * 1e-4 / leg1Bps)
            : std::numeric_limits<double>::quiet_NaN();
        const double fairSpreadLeg2 = std::fabs(leg2Bps) > 1e-16
            ? (currentSpread2 - npv * 1e-4 / leg2Bps)
            : std::numeric_limits<double>::quiet_NaN();
        rb.add_fair_spread_leg1(fairSpreadLeg1);
        rb.add_fair_spread_leg2(fairSpreadLeg2);
        if (includeFlows) {
            rb.add_leg1_flows(leg1FlowsOff);
            rb.add_leg2_flows(leg2FlowsOff);
        }
        out.push_back(rb.Finish());
    }

    auto swapsOff = builder->CreateVector(out);
    PriceBasisSwapResponseBuilder resp(*builder);
    resp.add_swaps(swapsOff);
    return resp.Finish();
}
