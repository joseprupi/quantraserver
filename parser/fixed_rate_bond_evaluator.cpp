#include "fixed_rate_bond_evaluator.h"

#include <sstream>

#include <ql/cashflow.hpp>
#include <ql/cashflows/fixedratecoupon.hpp>
#include <ql/interestrate.hpp>
#include <ql/pricingengines/bond/discountingbondengine.hpp>
#include <ql/pricingengine.hpp>
#include <ql/instruments/bond.hpp>
#include <ql/pricingengines/bond/bondfunctions.hpp>
#include <ql/utilities/dataformatters.hpp>

#include "error.h"
#include "common.h"

namespace quantra {

namespace {

std::vector<FixedRateBondFlowPlain> extractFlows(
    const QuantLib::Leg& cashflows,
    const std::shared_ptr<QuantLib::YieldTermStructure>& discountCurve,
    const QuantLib::Date& asOf) {
    std::vector<FixedRateBondFlowPlain> out;
    out.reserve(cashflows.size());
    for (const auto& cf : cashflows) {
        auto coupon = std::dynamic_pointer_cast<QuantLib::FixedRateCoupon>(cf);
        if (coupon) {
            FixedRateBondFlowPlain f;
            f.amount = coupon->amount();
            f.rate = coupon->rate();
            f.accrualStartDate = DateToIso(coupon->accrualStartDate());
            f.accrualEndDate = DateToIso(coupon->accrualEndDate());
            if (!coupon->hasOccurred(asOf)) {
                f.kind = FixedRateBondFlowPlain::Kind::Interest;
                f.discount = discountCurve->discount(coupon->date());
                f.price = f.amount * f.discount;
            } else {
                // Preserves the existing emit shape: amount/rate/accrual dates,
                // no discount or price for past coupons.
                f.kind = FixedRateBondFlowPlain::Kind::PastInterest;
            }
            out.push_back(std::move(f));
            continue;
        }

        auto cash = std::dynamic_pointer_cast<QuantLib::CashFlow>(cf);
        if (cash && !cash->hasOccurred(asOf)) {
            FixedRateBondFlowPlain f;
            f.kind = FixedRateBondFlowPlain::Kind::Notional;
            f.amount = cash->amount();
            f.discount = discountCurve->discount(cash->date());
            f.price = f.amount * f.discount;
            f.paymentDate = DateToIso(cash->date());
            out.push_back(std::move(f));
        }
    }
    return out;
}

} // namespace

FixedRateBondResult FixedRateBondEvaluator::evaluate(
    const FixedRateBondInputs& inputs,
    const PricingRegistry& reg,
    const PricingContext& ctx) const {

    FixedRateBondResult result;
    result.bonds.reserve(inputs.trades.size());

    for (const auto& trade : inputs.trades) {
        auto curveIt = reg.rates.curves.find(trade.discountingCurveId);
        if (curveIt == reg.rates.curves.end()) {
            QUANTRA_NOT_FOUND("Discounting curve not found: " + trade.discountingCurveId);
        }
        const auto& curveHandle = *curveIt->second;            // RelinkableHandle
        auto discountCurve = curveHandle.currentLink();        // shared_ptr<YieldTermStructure>

        auto engine = std::make_shared<QuantLib::DiscountingBondEngine>(curveHandle);
        trade.bond->setPricingEngine(engine);

        FixedRateBondPerBond out;
        out.npv = trade.bond->NPV();
        out.hasDetails = reg.options.bondPricingDetails;

        if (out.hasDetails) {
            out.yield = trade.bond->yield(trade.yieldDc, trade.yieldComp, trade.yieldFreq);
            out.cleanPrice = trade.bond->cleanPrice();
            out.dirtyPrice = trade.bond->dirtyPrice();
            out.accruedAmount = trade.bond->accruedAmount();
            out.accruedDays = QuantLib::BondFunctions::accruedDays(*trade.bond);

            QuantLib::InterestRate interestRate(
                out.yield, trade.yieldDc, trade.yieldComp, trade.yieldFreq);

            out.modifiedDuration = QuantLib::BondFunctions::duration(
                *trade.bond, interestRate, QuantLib::Duration::Modified, ctx.settlement);
            out.macaulayDuration = QuantLib::BondFunctions::duration(
                *trade.bond, interestRate, QuantLib::Duration::Macaulay, ctx.settlement);
            out.convexity = QuantLib::BondFunctions::convexity(
                *trade.bond, interestRate, ctx.settlement);
            out.bps = QuantLib::BondFunctions::bps(
                *trade.bond, *discountCurve, ctx.settlement);
        }

        if (reg.options.bondPricingFlows) {
            out.flows = extractFlows(trade.bond->cashflows(), discountCurve, ctx.asOf);
        }

        result.bonds.push_back(std::move(out));
    }

    return result;
}

} // namespace quantra
