#include "cross_currency_swap_evaluator.h"

#include "date_convert.h"
#include "error.h"
#include "fx_forward_evaluator.h" // FxSpotDate / FxAsOfExchangeRate (shared FX algebra)

#include <ql/termstructures/yieldtermstructure.hpp>

#include <cmath>
#include <memory>

namespace quantra {

namespace {

constexpr double kBasisPoint = 1.0e-4;

std::string legName(std::size_t i) {
    return std::string("CrossCurrencySwap.leg") + (i == 0 ? "1" : "2");
}

/// Annuity of a built leg's coupons for a unit rate / spread: bps / 1e-4.
double annuityOf(const BuiltSwapLeg& built) {
    return built.bpsOwn / kBasisPoint;
}

CrossCurrencySwapPerSwap priceTrade(
    const CrossCurrencySwapTrade& trade,
    const PricingRegistry& reg,
    const PricingContext& ctx,
    bool includeFlows) {
    // Per-leg checks first (curves, index, index kind and currency), then
    // the spot: a request with several problems is reported leg by leg.
    std::vector<BuiltSwapLeg> built;
    built.reserve(trade.legs.size());
    for (std::size_t i = 0; i < trade.legs.size(); ++i) {
        built.push_back(BuildSwapLeg(trade.legs[i], legName(i), reg, ctx));
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
    // As on every swap endpoint: fair_rate iff exactly one leg is fixed,
    // fair_spread iff a leg is floating (on the floating leg of a fixed/float
    // swap, on legs[1] of a float/float one).
    const bool fixed0 = l0.kind == SwapLegData::Kind::Fixed;
    const bool fixed1 = l1.kind == SwapLegData::Kind::Fixed;
    if (fixed0 != fixed1) {
        const std::size_t f = fixed0 ? 0 : 1;
        const SwapLegData& leg = trade.legs[f];
        if (std::fabs(annuityOf(built[f])) > 0.0) {
            out.fairRate = leg.level() - npvIn(leg.currency) / (leg.sign() * annuityOf(built[f]));
        }
    }
    if (!fixed0 || !fixed1) {
        const std::size_t k = fixed1 ? 0 : 1;
        const SwapLegData& leg = trade.legs[k];
        if (std::fabs(annuityOf(built[k])) > 0.0) {
            out.fairSpread = leg.level() - npvIn(leg.currency) / (leg.sign() * annuityOf(built[k]));
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
