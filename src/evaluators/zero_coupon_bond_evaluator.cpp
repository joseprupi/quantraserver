#include "zero_coupon_bond_evaluator.h"

#include <ql/interestrate.hpp>
#include <ql/pricingengines/bond/discountingbondengine.hpp>
#include <ql/pricingengines/bond/bondfunctions.hpp>

#include "error.h"
#include "date_convert.h"

namespace quantra {

ZeroCouponBondResult ZeroCouponBondEvaluator::evaluate(
    const ZeroCouponBondInputs& inputs,
    const PricingRegistry& reg,
    const PricingContext& ctx) const {

    ZeroCouponBondResult result;
    result.bonds.reserve(inputs.trades.size());

    for (const auto& trade : inputs.trades) {
        ctx.budget.check();  // honor the per-request deadline before each trade
        auto curveIt = reg.rates.curves.find(trade.discountingCurveId);
        if (curveIt == reg.rates.curves.end()) {
            QUANTRA_NOT_FOUND("Discounting curve not found: " + trade.discountingCurveId);
        }
        const auto& curveHandle = *curveIt->second;            // RelinkableHandle
        auto discountCurve = curveHandle.currentLink();        // shared_ptr<YieldTermStructure>

        auto engine = std::make_shared<QuantLib::DiscountingBondEngine>(curveHandle);
        trade.bond->setPricingEngine(engine);

        ZeroCouponBondPerBond out;
        out.npv = trade.bond->NPV();
        out.settlementDate = DateToIso(trade.bond->settlementDate());
        out.hasDetails = reg.options.bondPricingDetails;

        if (out.hasDetails) {
            out.yield = trade.bond->yield(trade.yieldDc, trade.yieldComp, trade.yieldFreq);
            out.cleanPrice = trade.bond->cleanPrice();
            out.dirtyPrice = trade.bond->dirtyPrice();
            // A zero-coupon bond has no coupons, so accrued interest is 0 by
            // construction; QuantLib::BondFunctions confirm this.
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

        result.bonds.push_back(std::move(out));
    }

    return result;
}

} // namespace quantra
