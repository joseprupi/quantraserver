#include "fx_forward_evaluator.h"

#include "date_convert.h"
#include "error.h"

#include <ql/termstructures/yieldtermstructure.hpp>
#include <ql/time/calendar.hpp>

#include <cmath>

namespace quantra {

QuantLib::Date FxSpotDate(const FxSpot& spot, const QuantLib::Date& asOf) {
    const QuantLib::Calendar& cal = spot.settlementCalendar;
    return cal.advance(cal.adjust(asOf), spot.spotDays * QuantLib::Days);
}

double FxAsOfExchangeRate(
    double spotRate,
    const QuantLib::Date& spotDate,
    const QuantLib::YieldTermStructure& baseDiscount,
    const QuantLib::YieldTermStructure& quoteDiscount) {
    return spotRate * quoteDiscount.discount(spotDate) / baseDiscount.discount(spotDate);
}

namespace {

const QuantLib::YieldTermStructure& curveOrThrow(
    const PricingRegistry& reg, const std::string& id, const char* role) {
    auto it = reg.rates.curves.find(id);
    if (it == reg.rates.curves.end()) {
        QUANTRA_NOT_FOUND(std::string(role) + " not found: " + id);
    }
    const auto& handle = *it->second;
    if (handle.empty()) {
        QUANTRA_ERROR(std::string(role) + " '" + id + "' is not linked to a term structure");
    }
    return *handle.currentLink();
}

FxForwardPerTrade priceTrade(
    const FxForwardTrade& trade, const PricingRegistry& reg, const PricingContext& ctx) {
    // Market data by id; every unknown reference is a 404 like elsewhere.
    const FxSpot& spot = reg.fx.spots.get(trade.fxSpotId); // QuantraNotFound when absent
    const auto& baseCurve =
        curveOrThrow(reg, trade.baseDiscountingCurveId, "Base discounting curve");
    const auto& quoteCurve =
        curveOrThrow(reg, trade.quoteDiscountingCurveId, "Quote discounting curve");

    // The forward's pair must be the spot's pair in the same order: a spot
    // quoted the other way round is never inverted implicitly.
    if (trade.baseCurrency != spot.baseCurrency || trade.quoteCurrency != spot.quoteCurrency) {
        QUANTRA_INVALID_ARGUMENT(
            "FxForward.base_currency/quote_currency " + trade.baseCurrency + "/" +
            trade.quoteCurrency + " do not match the pair of PriceFxForward.fx_spot '" +
            trade.fxSpotId + "' (" + spot.baseCurrency + "/" + spot.quoteCurrency + ")");
    }

    // Dates: spot date from the (adjusted) evaluation date, maturity adjusted
    // with the spot's calendar and convention, as an FX swap's far leg is.
    const QuantLib::Date spotDate = FxSpotDate(spot, ctx.asOf);
    const QuantLib::Date maturity =
        spot.settlementCalendar.adjust(trade.maturityDate, spot.businessDayConvention);
    if (maturity <= spotDate) {
        QUANTRA_INVALID_ARGUMENT(
            "FxForward.maturity_date " + DateToIso(trade.maturityDate) + " (adjusted " +
            DateToIso(maturity) + ") must fall after the spot date " + DateToIso(spotDate) +
            " of PriceFxForward.fx_spot '" + trade.fxSpotId + "'");
    }

    const double S = spot.rate->value();
    const double N = trade.baseNotional;
    const double K = trade.forwardRate;
    const double sign = trade.direction == FxDirectionKind::BuyBase ? 1.0 : -1.0;

    const double dfBaseT = baseCurve.discount(maturity);
    const double dfQuoteT = quoteCurve.discount(maturity);

    // Leg present values, each in its own currency (see the header).
    const double baseLegPv = sign * N * dfBaseT;
    const double quoteLegPv = -sign * N * K * dfQuoteT;

    // As-of exchange rate and the fair rate derived from it: at K = fair,
    // baseLegPv * X + quoteLegPv == sign * N * (dfBaseT * X - X * dfBaseT) == 0.
    const double X = FxAsOfExchangeRate(S, spotDate, baseCurve, quoteCurve);
    const double fair = X * dfBaseT / dfQuoteT;

    FxForwardPerTrade out;
    if (trade.reportingCurrency == trade.quoteCurrency) {
        out.npv = baseLegPv * X + quoteLegPv;
    } else if (trade.reportingCurrency == trade.baseCurrency) {
        out.npv = baseLegPv + quoteLegPv / X;
    } else {
        // The mapper already rejects this; kept so the evaluator is safe on
        // its own.
        QUANTRA_INVALID_ARGUMENT(
            "PriceFxForward.reporting_currency '" + trade.reportingCurrency + "' must be " +
            trade.baseCurrency + " or " + trade.quoteCurrency);
    }
    out.fairForwardRate = fair;
    out.forwardPoints = fair - S;
    out.baseLegPv = baseLegPv;
    out.quoteLegPv = quoteLegPv;
    out.spotDate = DateToIso(spotDate);
    out.maturityDate = DateToIso(maturity);
    out.fxSpotUsed = S;
    return out;
}

} // namespace

FxForwardResult FxForwardEvaluator::evaluate(
    const FxForwardInputs& inputs, const PricingRegistry& reg, const PricingContext& ctx) const {
    FxForwardResult result;
    result.trades.reserve(inputs.trades.size());
    for (const auto& trade : inputs.trades) {
        ctx.budget.check();
        result.trades.push_back(priceTrade(trade, reg, ctx));
    }
    return result;
}

} // namespace quantra
