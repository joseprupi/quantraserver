#include "cross_currency_swap_evaluator.h"

#include "date_convert.h"
#include "error.h"
#include "fx_forward_evaluator.h" // FxSpotDate / FxAsOfExchangeRate (shared FX algebra)

#include <ql/cashflows/cashflows.hpp>
#include <ql/cashflows/simplecashflow.hpp>
#include <ql/indexes/iborindex.hpp>
#include <ql/termstructures/yieldtermstructure.hpp>

#include <cmath>
#include <iostream>
#include <memory>

namespace quantra {

namespace {

constexpr double kBasisPoint = 1.0e-4;

std::string legName(std::size_t i) {
    return std::string("CrossCurrencySwap.leg") + (i == 0 ? "1" : "2");
}

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

/// A leg assembled for pricing: the coupon leg alone (annuity) and the full
/// leg with the principal exchanges (NPV), both in the leg's own frame.
struct BuiltLeg {
    QuantLib::Leg coupons;
    QuantLib::Leg full;
    const QuantLib::YieldTermStructure* discount = nullptr;
    /// NPV of `full` in the leg's own frame (receiver of the coupons).
    double npvOwn = 0.0;
    /// Annuity of the coupons for a unit rate / spread: bps / 1e-4.
    double annuity = 0.0;
};

/// The coupon leg of `leg` on its forwarding index. The index kind must match
/// the coupon kind: a compounded overnight coupon on an IBOR index (or an
/// IBOR coupon on an overnight index) is a request error naming the table
/// to use, never a silently different instrument.
QuantLib::Leg buildCoupons(
    const SwapLegData& leg, const std::string& name, const PricingRegistry& reg) {
    if (leg.kind == SwapLegData::Kind::Fixed) {
        return BuildFixedRateLeg(leg.fixed);
    }
    const auto& fwdHandle = curveOrThrow(reg, leg.forwardingCurveId, name + " forwarding curve");
    if (!reg.rates.indices.has(leg.indexId)) {
        QUANTRA_NOT_FOUND("Index not found: " + leg.indexId);
    }
    QuantLib::Handle<QuantLib::YieldTermStructure> fwd(fwdHandle.currentLink());
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

BuiltLeg buildLeg(
    const SwapLegData& leg,
    const std::string& name,
    const PricingRegistry& reg,
    const PricingContext& ctx) {
    BuiltLeg out;
    const auto& discHandle = curveOrThrow(reg, leg.discountingCurveId, name + " discounting curve");
    out.discount = discHandle.currentLink().get();
    out.coupons = buildCoupons(leg, name, reg);
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

    // Flows on the evaluation date are excluded, as DiscountingSwapEngine
    // excludes them for every other swap (includeSettlementDateFlows = false).
    out.npvOwn = QuantLib::CashFlows::npv(out.full, *out.discount, false, ctx.asOf, ctx.asOf);
    out.annuity = QuantLib::CashFlows::bps(out.coupons, *out.discount, false, ctx.asOf, ctx.asOf) /
                  kBasisPoint;
    return out;
}

CrossCurrencySwapPerSwap priceTrade(
    const CrossCurrencySwapTrade& trade,
    const PricingRegistry& reg,
    const PricingContext& ctx,
    bool includeFlows) {
    // Per-leg checks first (curves, index, index kind and currency), then
    // the spot: a request with several problems is reported leg by leg.
    std::vector<BuiltLeg> built;
    built.reserve(trade.legs.size());
    for (std::size_t i = 0; i < trade.legs.size(); ++i) {
        built.push_back(buildLeg(trade.legs[i], legName(i), reg, ctx));
    }
    const SwapLegData& l0 = trade.legs[0];
    const SwapLegData& l1 = trade.legs[1];

    const FxSpot& spot = reg.fx.spots.get(trade.fxSpotId); // QuantraNotFound when absent

    // The spot's pair must be the two leg currencies, in either order.
    const bool leg0IsBase = l0.currency == spot.baseCurrency && l1.currency == spot.quoteCurrency;
    const bool leg1IsBase = l1.currency == spot.baseCurrency && l0.currency == spot.quoteCurrency;
    if (!leg0IsBase && !leg1IsBase) {
        QUANTRA_INVALID_ARGUMENT(
            "PriceCrossCurrencySwap.fx_spot '" + trade.fxSpotId + "' (" + spot.baseCurrency + "/" +
            spot.quoteCurrency + ") does not quote the pair of the legs (" + l0.currency + ", " +
            l1.currency + ")");
    }

    // As-of exchange rate from the spot and the two legs' discount curves.
    const QuantLib::Date spotDate = FxSpotDate(spot, ctx.asOf);
    const double S = spot.rate->value();
    const QuantLib::YieldTermStructure& baseDisc =
        leg0IsBase ? *built[0].discount : *built[1].discount;
    const QuantLib::YieldTermStructure& quoteDisc =
        leg0IsBase ? *built[1].discount : *built[0].discount;
    const double X = FxAsOfExchangeRate(S, spotDate, baseDisc, quoteDisc);

    // Converts an amount between the two leg currencies (quote per base = X).
    auto convert = [&](double amount, const std::string& from, const std::string& to) {
        if (from == to) return amount;
        if (from == spot.baseCurrency) return amount * X;
        return amount / X;
    };

    CrossCurrencySwapPerSwap out;
    out.legs.resize(trade.legs.size());
    for (std::size_t i = 0; i < trade.legs.size(); ++i) {
        out.legs[i].npv = trade.legs[i].sign() * built[i].npvOwn;
        out.legs[i].npvReporting =
            convert(out.legs[i].npv, trade.legs[i].currency, trade.reportingCurrency);
    }
    out.npv = out.legs[0].npvReporting + out.legs[1].npvReporting;

    // Fair rate / spread: the NPV is linear in the fixed rate (spread) with
    // slope s_i * A_i in the leg's own currency.
    auto npvIn = [&](const std::string& ccy) {
        return convert(out.legs[0].npv, l0.currency, ccy) +
               convert(out.legs[1].npv, l1.currency, ccy);
    };
    const bool fixed0 = l0.kind == SwapLegData::Kind::Fixed;
    const bool fixed1 = l1.kind == SwapLegData::Kind::Fixed;
    if (fixed0 != fixed1) {
        const std::size_t f = fixed0 ? 0 : 1;
        const SwapLegData& leg = trade.legs[f];
        if (std::fabs(built[f].annuity) > 0.0) {
            out.fairRate = leg.level() - npvIn(leg.currency) / (leg.sign() * built[f].annuity);
        }
    } else if (!fixed0 && !fixed1) {
        if (std::fabs(built[1].annuity) > 0.0) {
            out.fairSpread = l1.level() - npvIn(l1.currency) / (l1.sign() * built[1].annuity);
        }
    }

    out.fxSpotUsed = S;
    out.fxAsOfRate = X;
    out.spotDate = DateToIso(spotDate);
    out.includeFlows = includeFlows;
    if (includeFlows) {
        for (std::size_t i = 0; i < trade.legs.size(); ++i) {
            ExtractSwapLegFlows(built[i].full, *built[i].discount, ctx.asOf, out.legs[i].flows);
        }
    }

    std::cout << "Cross-currency swap " << l0.currency << "/" << l1.currency << " NPV ("
              << trade.reportingCurrency << "): " << out.npv << std::endl;
    return out;
}

} // namespace

CrossCurrencySwapResult CrossCurrencySwapEvaluator::evaluate(
    const CrossCurrencySwapInputs& inputs,
    const PricingRegistry& reg,
    const PricingContext& ctx) const {
    CrossCurrencySwapResult result;
    result.swaps.reserve(inputs.trades.size());
    for (const auto& trade : inputs.trades) {
        ctx.budget.check();
        result.swaps.push_back(priceTrade(trade, reg, ctx, inputs.includeFlows));
    }
    return result;
}

} // namespace quantra
