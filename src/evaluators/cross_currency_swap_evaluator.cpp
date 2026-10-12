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

BuiltLeg buildLeg(
    const XccyLegData& leg,
    const char* legName,
    const PricingRegistry& reg,
    const PricingContext& ctx) {
    BuiltLeg out;
    const auto& discHandle =
        curveOrThrow(reg, leg.discountingCurveId, std::string(legName) + " discounting curve");
    out.discount = discHandle.currentLink().get();

    if (leg.kind == XccyLegData::Kind::Fixed) {
        out.coupons = BuildFixedRateLeg(leg.fixed);
    } else {
        const auto& fwdHandle =
            curveOrThrow(reg, leg.forwardingCurveId, std::string(legName) + " forwarding curve");
        if (!reg.rates.indices.has(leg.indexId)) {
            QUANTRA_NOT_FOUND("Index not found: " + leg.indexId);
        }
        QuantLib::Handle<QuantLib::YieldTermStructure> fwd(fwdHandle.currentLink());
        auto base = reg.rates.indices.get(leg.indexId);
        // The leg currency is explicit on the wire and must be the index's
        // own: the engine never infers a currency from an index id.
        if (base->currency().code() != leg.currency) {
            QUANTRA_INVALID_ARGUMENT(
                std::string("XccyLeg.currency '") + leg.currency + "' of " + legName +
                " does not match the currency " + base->currency().code() + " of its index '" +
                leg.indexId + "' (IndexDef.currency)");
        }
        if (std::dynamic_pointer_cast<QuantLib::OvernightIndex>(base)) {
            if (leg.floating.inArrears) {
                QUANTRA_INVALID_ARGUMENT(
                    std::string("XccyLeg.floating_leg.in_arrears of ") + legName +
                    " must be false for an overnight index (index '" + leg.indexId +
                    "'): a compounded overnight coupon fixes daily");
            }
            out.coupons = BuildOvernightLeg(
                leg.floating, reg.rates.indices.getOvernightWithCurve(leg.indexId, fwd));
        } else {
            out.coupons =
                BuildIborLeg(leg.floating, reg.rates.indices.getIborWithCurve(leg.indexId, fwd));
        }
    }
    if (out.coupons.empty()) {
        QUANTRA_INVALID_ARGUMENT(
            std::string("XccyLeg schedule of ") + legName + " produces no coupon");
    }

    // Principal exchanges in the leg's own frame: the receiver of the coupons
    // pays the notional on the schedule's start date and gets it back with
    // the last coupon.
    out.full = out.coupons;
    const QuantLib::Schedule& schedule =
        leg.kind == XccyLegData::Kind::Fixed ? leg.fixed.schedule : leg.floating.schedule;
    if (leg.initialExchange) {
        out.full.push_back(
            std::make_shared<QuantLib::SimpleCashFlow>(-leg.notional, schedule.dates().front()));
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
    // Per-leg checks first (curves, index, index currency), then the spot:
    // a request with several problems is reported leg by leg.
    BuiltLeg leg1 = buildLeg(trade.leg1, "leg1", reg, ctx);
    BuiltLeg leg2 = buildLeg(trade.leg2, "leg2", reg, ctx);

    const FxSpot& spot = reg.fx.spots.get(trade.fxSpotId); // QuantraNotFound when absent

    // The spot's pair must be the two leg currencies, in either order.
    const bool leg1IsBase =
        trade.leg1.currency == spot.baseCurrency && trade.leg2.currency == spot.quoteCurrency;
    const bool leg2IsBase =
        trade.leg2.currency == spot.baseCurrency && trade.leg1.currency == spot.quoteCurrency;
    if (!leg1IsBase && !leg2IsBase) {
        QUANTRA_INVALID_ARGUMENT(
            "PriceCrossCurrencySwap.fx_spot '" + trade.fxSpotId + "' (" + spot.baseCurrency + "/" +
            spot.quoteCurrency + ") does not quote the pair of the legs (" + trade.leg1.currency +
            ", " + trade.leg2.currency + ")");
    }

    // As-of exchange rate from the spot and the two legs' discount curves.
    const QuantLib::Date spotDate = FxSpotDate(spot, ctx.asOf);
    const double S = spot.rate->value();
    const QuantLib::YieldTermStructure& baseDisc = leg1IsBase ? *leg1.discount : *leg2.discount;
    const QuantLib::YieldTermStructure& quoteDisc = leg1IsBase ? *leg2.discount : *leg1.discount;
    const double X = FxAsOfExchangeRate(S, spotDate, baseDisc, quoteDisc);

    // Converts an amount between the two leg currencies (quote per base = X).
    auto convert = [&](double amount, const std::string& from, const std::string& to) {
        if (from == to) return amount;
        if (from == spot.baseCurrency) return amount * X;
        return amount / X;
    };

    const double s1 = trade.swapType == QuantLib::VanillaSwap::Payer ? -1.0 : 1.0;
    const double s2 = -s1;

    CrossCurrencySwapPerSwap out;
    out.leg1Npv = s1 * leg1.npvOwn;
    out.leg2Npv = s2 * leg2.npvOwn;
    out.leg1NpvReporting = convert(out.leg1Npv, trade.leg1.currency, trade.reportingCurrency);
    out.leg2NpvReporting = convert(out.leg2Npv, trade.leg2.currency, trade.reportingCurrency);
    out.npv = out.leg1NpvReporting + out.leg2NpvReporting;

    // Fair rate / spread: the NPV is linear in the fixed rate (spread) with
    // slope s_i * A_i in the leg's own currency.
    const bool fixed1 = trade.leg1.kind == XccyLegData::Kind::Fixed;
    const bool fixed2 = trade.leg2.kind == XccyLegData::Kind::Fixed;
    auto npvIn = [&](const std::string& ccy) {
        return convert(out.leg1Npv, trade.leg1.currency, ccy) +
               convert(out.leg2Npv, trade.leg2.currency, ccy);
    };
    if (fixed1 != fixed2) {
        const XccyLegData& f = fixed1 ? trade.leg1 : trade.leg2;
        const BuiltLeg& built = fixed1 ? leg1 : leg2;
        const double sf = fixed1 ? s1 : s2;
        if (std::fabs(built.annuity) > 0.0) {
            out.fairRate = f.fixed.rate - npvIn(f.currency) / (sf * built.annuity);
        }
    } else if (!fixed1 && !fixed2) {
        if (std::fabs(leg2.annuity) > 0.0) {
            out.fairSpread =
                trade.leg2.floating.spread - npvIn(trade.leg2.currency) / (s2 * leg2.annuity);
        }
    }

    out.fxSpotUsed = S;
    out.fxAsOfRate = X;
    out.spotDate = DateToIso(spotDate);
    out.includeFlows = includeFlows;
    if (includeFlows) {
        ExtractSwapLegFlows(leg1.full, *leg1.discount, ctx.asOf, out.leg1Flows);
        ExtractSwapLegFlows(leg2.full, *leg2.discount, ctx.asOf, out.leg2Flows);
    }

    std::cout << "Cross-currency swap " << trade.leg1.currency << "/" << trade.leg2.currency
              << " NPV (" << trade.reportingCurrency << "): " << out.npv << std::endl;
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
