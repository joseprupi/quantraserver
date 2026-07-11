#include "floating_rate_bond_evaluator.h"

#include <variant>

#include <ql/cashflow.hpp>
#include <ql/cashflows/couponpricer.hpp>
#include <ql/cashflows/floatingratecoupon.hpp>
#include <ql/handle.hpp>
#include <ql/indexes/iborindex.hpp>
#include <ql/instruments/bonds/amortizingfloatingratebond.hpp>
#include <ql/interestrate.hpp>
#include <ql/pricingengines/bond/bondfunctions.hpp>
#include <ql/pricingengines/bond/discountingbondengine.hpp>
#include <ql/quote.hpp>
#include <ql/termstructures/volatility/optionlet/constantoptionletvol.hpp>
#include <ql/termstructures/volatility/optionlet/optionletvolatilitystructure.hpp>
#include <ql/types.hpp>

#include "date_convert.h"
#include "coupon_pricer_domain.h"
#include "error.h"

namespace quantra {

namespace {

/**
 * Construct the QuantLib BlackIborCouponPricer from a plain-domain spec.
 * Mirrors PricerParser::parse exactly: a ConstantOptionletVolatility is wrapped
 * in a Handle and attached to a default-constructed BlackIborCouponPricer.
 */
std::shared_ptr<QuantLib::IborCouponPricer> buildBlackIborCouponPricer(
    const BlackIborCouponPricerDomain& spec) {
    auto vol = std::make_shared<QuantLib::ConstantOptionletVolatility>(
        spec.settlement_days,
        spec.calendar,
        spec.business_day_convention,
        spec.volatility,
        spec.day_counter);
    QuantLib::Handle<QuantLib::OptionletVolatilityStructure> volHandle(vol);
    auto pricer = std::make_shared<QuantLib::BlackIborCouponPricer>();
    pricer->setCapletVolatility(volHandle);
    return pricer;
}

std::vector<FloatingRateBondFlowPlain> extractFlows(
    const QuantLib::Leg& cashflows,
    const std::shared_ptr<QuantLib::YieldTermStructure>& discountCurve,
    const QuantLib::Date& asOf) {
    std::vector<FloatingRateBondFlowPlain> out;
    out.reserve(cashflows.size());
    for (const auto& cf : cashflows) {
        auto coupon = std::dynamic_pointer_cast<QuantLib::FloatingRateCoupon>(cf);
        if (coupon) {
            FloatingRateBondFlowPlain f;
            f.amount = coupon->amount();
            f.indexFixing = coupon->indexFixing();
            f.accrualStartDate = DateToIso(coupon->accrualStartDate());
            f.accrualEndDate = DateToIso(coupon->accrualEndDate());
            f.rate = coupon->rate();
            if (!coupon->hasOccurred(asOf)) {
                f.kind = FloatingRateBondFlowPlain::Kind::Interest;
                f.discount = discountCurve->discount(coupon->date());
                f.price = f.amount * f.discount;
            } else {
                // Past floating coupon: matches the legacy emit shape — no
                // discount and no price, only the realized rate/amount.
                f.kind = FloatingRateBondFlowPlain::Kind::PastInterest;
            }
            out.push_back(std::move(f));
            continue;
        }

        auto cash = std::dynamic_pointer_cast<QuantLib::CashFlow>(cf);
        if (cash && !cash->hasOccurred(asOf)) {
            FloatingRateBondFlowPlain f;
            f.kind = FloatingRateBondFlowPlain::Kind::Notional;
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

FloatingRateBondResult FloatingRateBondEvaluator::evaluate(
    const FloatingRateBondInputs& inputs,
    const PricingRegistry& reg,
    const PricingContext& ctx) const {

    FloatingRateBondResult result;
    result.bonds.reserve(inputs.trades.size());

    for (const auto& trade : inputs.trades) {
        ctx.budget.check();  // honor the per-request deadline before each trade
        auto discIt = reg.rates.curves.find(trade.discountingCurveId);
        if (discIt == reg.rates.curves.end()) {
            QUANTRA_NOT_FOUND("Discounting curve not found: " + trade.discountingCurveId);
        }
        auto fwdIt = reg.rates.curves.find(trade.forwardingCurveId);
        if (fwdIt == reg.rates.curves.end()) {
            QUANTRA_NOT_FOUND("Forwarding curve not found: " + trade.forwardingCurveId);
        }
        if (!reg.rates.indices.has(trade.indexId)) {
            QUANTRA_NOT_FOUND("Index not found: " + trade.indexId);
        }
        auto pricerIt = reg.rates.couponPricerDomains.find(trade.couponPricerId);
        if (pricerIt == reg.rates.couponPricerDomains.end()) {
            QUANTRA_NOT_FOUND("Coupon pricer not found: " + trade.couponPricerId);
        }
        const auto* blackSpec =
            std::get_if<BlackIborCouponPricerDomain>(&pricerIt->second.payload);
        if (blackSpec == nullptr) {
            QUANTRA_INVALID_ARGUMENT(
                "Coupon pricer '" + trade.couponPricerId +
                "' is not a BlackIborCouponPricer");
        }

        const auto& discountHandle = *discIt->second;
        auto discountCurve = discountHandle.currentLink();
        const auto& forwardingHandle = *fwdIt->second;
        auto iborIndex =
            reg.rates.indices.getIborWithCurve(trade.indexId, forwardingHandle);

        std::shared_ptr<QuantLib::Bond> bond;
        if (trade.notionals.empty()) {
            bond = std::make_shared<QuantLib::FloatingRateBond>(
                trade.settlement_days,
                trade.face_amount,
                trade.schedule,
                iborIndex,
                trade.accrual_day_counter,
                trade.payment_convention,
                trade.fixing_days,
                std::vector<QuantLib::Real>(1, 1.0),
                std::vector<QuantLib::Spread>(1, trade.spread),
                std::vector<QuantLib::Rate>(),
                std::vector<QuantLib::Rate>(),
                trade.in_arrears,
                trade.redemption,
                trade.issue_date);
        } else {
            // AmortizingFloatingRateBond carries the outstanding notional per
            // period; redemptions are derived from the notional decrements at
            // `redemption` percent, matching the constant bond's redemption.
            bond = std::make_shared<QuantLib::AmortizingFloatingRateBond>(
                trade.settlement_days,
                std::vector<QuantLib::Real>(trade.notionals.begin(), trade.notionals.end()),
                trade.schedule,
                iborIndex,
                trade.accrual_day_counter,
                trade.payment_convention,
                trade.fixing_days,
                std::vector<QuantLib::Real>(1, 1.0),
                std::vector<QuantLib::Spread>(1, trade.spread),
                std::vector<QuantLib::Rate>(),
                std::vector<QuantLib::Rate>(),
                trade.in_arrears,
                trade.issue_date,
                QuantLib::Period(),
                QuantLib::Calendar(),
                QuantLib::Unadjusted,
                false,
                std::vector<QuantLib::Real>(1, trade.redemption));
        }

        auto engine = std::make_shared<QuantLib::DiscountingBondEngine>(discountHandle);
        bond->setPricingEngine(engine);
        auto couponPricer = buildBlackIborCouponPricer(*blackSpec);
        QuantLib::setCouponPricer(bond->cashflows(), couponPricer);

        FloatingRateBondPerBond out;
        out.npv = bond->NPV();
        out.hasDetails = reg.options.bondPricingDetails;

        if (out.hasDetails) {
            out.yield = bond->yield(trade.yieldDc, trade.yieldComp, trade.yieldFreq);
            out.cleanPrice = bond->cleanPrice();
            out.dirtyPrice = bond->dirtyPrice();
            out.accruedAmount = bond->accruedAmount();
            out.accruedDays =
                static_cast<double>(QuantLib::BondFunctions::accruedDays(*bond));

            QuantLib::InterestRate interestRate(
                out.yield, trade.yieldDc, trade.yieldComp, trade.yieldFreq);

            out.modifiedDuration = QuantLib::BondFunctions::duration(
                *bond, interestRate, QuantLib::Duration::Modified, ctx.settlement);
            out.macaulayDuration = QuantLib::BondFunctions::duration(
                *bond, interestRate, QuantLib::Duration::Macaulay, ctx.settlement);
            out.convexity = QuantLib::BondFunctions::convexity(
                *bond, interestRate, ctx.settlement);
            out.bps = QuantLib::BondFunctions::bps(
                *bond, *discountCurve, ctx.settlement);
        }

        if (reg.options.bondPricingFlows) {
            out.flows = extractFlows(bond->cashflows(), discountCurve, ctx.asOf);
        }

        result.bonds.push_back(std::move(out));
    }

    return result;
}

} // namespace quantra
