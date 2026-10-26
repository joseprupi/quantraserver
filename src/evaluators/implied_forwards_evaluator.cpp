#include "implied_forwards_evaluator.h"

#include "date_convert.h"
#include "error.h"

#include <ql/pricingengines/blackformula.hpp>
#include <ql/time/daycounters/actual365fixed.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <sstream>

namespace quantra {

namespace {

struct Pair {
    double strike;
    double y;      // call mid - put mid
    double weight; // 1 / (call spread + put spread), or 1.0
    std::optional<double> strikeKey;
};

struct LineFit {
    double a = 0.0; // intercept = DF * F
    double b = 0.0; // slope = -DF
};

/// Weighted least squares of y on K (see the header for the algebra).
/// Returns nullopt when the strikes do not span (all equal).
std::optional<LineFit> weightedLine(const std::vector<Pair>& pairs) {
    double W = 0.0, sK = 0.0, sY = 0.0;
    for (const auto& p : pairs) {
        W += p.weight;
        sK += p.weight * p.strike;
        sY += p.weight * p.y;
    }
    const double kBar = sK / W;
    const double yBar = sY / W;
    double sxy = 0.0, sxx = 0.0;
    for (const auto& p : pairs) {
        const double dk = p.strike - kBar;
        sxy += p.weight * dk * (p.y - yBar);
        sxx += p.weight * dk * dk;
    }
    if (!(sxx > 0.0)) return std::nullopt;
    LineFit f;
    f.b = sxy / sxx;
    f.a = yBar - f.b * kBar;
    return f;
}

std::string fmt(double v) {
    std::ostringstream os;
    os.precision(10);
    os << v;
    return os.str();
}

} // namespace

void validateQuoteFilter(const QuoteFilterData& flt) {
    if (!(flt.maxRelativeSpread > 0.0) || !std::isfinite(flt.maxRelativeSpread)) {
        QUANTRA_INVALID_ARGUMENT("filter.max_relative_spread must be finite and > 0");
    }
    if (flt.minQuotesPerExpiry < 2) {
        QUANTRA_INVALID_ARGUMENT(
            "filter.min_quotes_per_expiry must be >= 2 (a line needs two strikes)");
    }
    if (!(flt.moneynessWindow > 0.0) || !std::isfinite(flt.moneynessWindow)) {
        QUANTRA_INVALID_ARGUMENT("filter.moneyness_window must be finite and > 0");
    }
    if (flt.minOpenInterest && !(std::isfinite(*flt.minOpenInterest))) {
        QUANTRA_INVALID_ARGUMENT("filter.min_open_interest must be finite");
    }
}

std::vector<FilteredQuote> filterExpiryLines(
    const std::vector<const OptionQuoteLine*>& lines,
    const QuoteFilterData& flt,
    std::vector<DroppedQuoteData>& dropped) {
    std::vector<FilteredQuote> out;
    std::set<double> seenStrikes;
    auto dropLine = [&](const OptionQuoteLine& q, const std::string& reason) {
        dropped.push_back(DroppedQuoteData{q.expiryIso, q.strike, reason});
    };
    for (const auto* qp : lines) {
        const auto& q = *qp;
        if (!q.strike || !(*q.strike > 0.0) || !std::isfinite(*q.strike)) {
            dropLine(q, "strike missing or not > 0");
            continue;
        }
        if (!q.callBid || !q.callAsk || !q.putBid || !q.putAsk) {
            dropLine(q, "missing call or put side");
            continue;
        }
        const double cb = *q.callBid, ca = *q.callAsk, pb = *q.putBid, pa = *q.putAsk;
        if (!std::isfinite(cb) || !std::isfinite(ca) || !std::isfinite(pb) || !std::isfinite(pa)) {
            dropLine(q, "non-finite price");
            continue;
        }
        if (cb < 0.0 || pb < 0.0 || !(ca > 0.0) || !(pa > 0.0)) {
            dropLine(q, "negative bid or non-positive ask");
            continue;
        }
        if (cb > ca || pb > pa) {
            dropLine(q, "crossed quote (bid > ask)");
            continue;
        }
        if (flt.minOpenInterest) {
            if (!q.callOpenInterest || !q.putOpenInterest ||
                *q.callOpenInterest < *flt.minOpenInterest ||
                *q.putOpenInterest < *flt.minOpenInterest) {
                dropLine(q, "open interest below min_open_interest");
                continue;
            }
        }
        FilteredQuote f;
        f.strike = *q.strike;
        f.callBid = cb;
        f.callAsk = ca;
        f.putBid = pb;
        f.putAsk = pa;
        f.callMid = 0.5 * (cb + ca);
        f.putMid = 0.5 * (pb + pa);
        f.callSpread = ca - cb;
        f.putSpread = pa - pb;
        if (f.callSpread / f.callMid > flt.maxRelativeSpread ||
            f.putSpread / f.putMid > flt.maxRelativeSpread) {
            dropLine(q, "relative spread above max_relative_spread");
            continue;
        }
        if (!seenStrikes.insert(f.strike).second) {
            dropLine(q, "duplicate strike");
            continue;
        }
        out.push_back(f);
    }
    return out;
}

std::optional<double> impliedBlackVol(
    QuantLib::Option::Type type,
    double strike,
    double forward,
    double price,
    double discountFactor,
    double time) {
    try {
        const double sd = QuantLib::blackFormulaImpliedStdDev(
            type, strike, forward, price, discountFactor, 0.0, QuantLib::Null<QuantLib::Real>(),
            1e-12, 1000);
        const double v = sd / std::sqrt(time);
        if (!std::isfinite(v) || !(v > 0.0)) return std::nullopt;
        return v;
    } catch (...) {
        return std::nullopt;
    }
}

std::vector<OutOfTheMoneyQuote> selectOutOfTheMoneyQuotes(
    const std::vector<const OptionQuoteLine*>& lines,
    const QuoteFilterData& flt,
    const std::string& expiryIso,
    double spot,
    double forward,
    double discountFactor,
    double time,
    std::vector<DroppedQuoteData>& dropped) {
    const double F = forward, DF = discountFactor;
    std::vector<OutOfTheMoneyQuote> used;
    for (const FilteredQuote& q : filterExpiryLines(lines, flt, dropped)) {
        const bool isCall = q.strike >= F;
        const char* side = isCall ? "call" : "put";
        const double bid = isCall ? q.callBid : q.putBid;
        const double ask = isCall ? q.callAsk : q.putAsk;
        const double mid = isCall ? q.callMid : q.putMid;
        const double intrinsic = DF * std::max(isCall ? F - q.strike : q.strike - F, 0.0);
        if (!(mid > intrinsic)) {
            dropped.push_back(
                DroppedQuoteData{
                    expiryIso, q.strike,
                    std::string("out-of-the-money ") + side + " mid not above intrinsic value"});
            continue;
        }
        if (mid < kQuoteResolution * spot) {
            dropped.push_back(
                DroppedQuoteData{
                    expiryIso, q.strike,
                    std::string("out-of-the-money ") + side +
                        " mid below 1e-5 of the spot (quote resolution, no vol information)"});
            continue;
        }
        const auto vol = impliedBlackVol(
            isCall ? QuantLib::Option::Call : QuantLib::Option::Put, q.strike, F, mid, DF, time);
        if (!vol) {
            dropped.push_back(
                DroppedQuoteData{
                    expiryIso, q.strike,
                    std::string("implied vol of the out-of-the-money ") + side +
                        " mid does not solve (price outside the Black bounds)"});
            continue;
        }
        OutOfTheMoneyQuote u;
        u.strike = q.strike;
        u.isCall = isCall;
        u.bid = bid;
        u.ask = ask;
        u.mid = mid;
        u.vol = *vol;
        used.push_back(u);
    }
    std::sort(
        used.begin(), used.end(), [](const OutOfTheMoneyQuote& x, const OutOfTheMoneyQuote& y) {
            return x.strike < y.strike;
        });
    return used;
}

SheetMarket resolveSheetMarket(
    const PricingRegistry& reg, const std::string& underlyingId, const std::string& curveId) {
    auto uIt = reg.equity.equityUnderlyings.find(underlyingId);
    if (uIt == reg.equity.equityUnderlyings.end()) {
        QUANTRA_NOT_FOUND("Equity underlying not found: " + underlyingId);
    }
    if (uIt->second.spot.empty()) {
        QUANTRA_ERROR("Equity underlying '" + underlyingId + "' has no spot handle");
    }
    SheetMarket m;
    m.spot = uIt->second.spot->value();
    if (!(m.spot > 0.0) || !std::isfinite(m.spot)) {
        QUANTRA_INVALID_ARGUMENT(
            "Spot of equity underlying '" + underlyingId + "' must be finite and > 0");
    }
    auto cIt = reg.rates.curves.find(curveId);
    if (cIt == reg.rates.curves.end()) {
        QUANTRA_NOT_FOUND("Discounting curve not found: " + curveId);
    }
    m.curve = QuantLib::Handle<QuantLib::YieldTermStructure>(cIt->second->currentLink());
    if (m.curve.empty()) {
        QUANTRA_ERROR("Discounting curve '" + curveId + "' has an empty handle");
    }
    return m;
}

ImpliedForwardsResult ImpliedForwardsEvaluator::evaluate(
    const ImpliedForwardsInputs& in, const PricingRegistry& reg, const PricingContext& ctx) const {
    if (in.style != OptionSheetStyle::European) {
        QUANTRA_INVALID_ARGUMENT(
            "sheet.option_style must be European: put-call parity, which the implied-forward "
            "fit rests on, does not hold for American or Bermudan options (early-exercise "
            "premium)");
    }
    const QuoteFilterData& flt = in.filter;
    validateQuoteFilter(flt);
    if (in.quotes.empty()) {
        QUANTRA_INVALID_ARGUMENT("sheet.quotes must contain at least one quote");
    }
    const SheetMarket market = resolveSheetMarket(reg, in.underlyingId, in.discountingCurveId);
    const double spot = market.spot;
    const QuantLib::Handle<QuantLib::YieldTermStructure>& curve = market.curve;

    const QuantLib::Date asOf = ctx.asOf;
    const QuantLib::DayCounter dc = QuantLib::Actual365Fixed();

    // Group the sheet by expiry, date order.
    std::map<QuantLib::Date, std::vector<const OptionQuoteLine*>> byExpiry;
    for (const auto& q : in.quotes)
        byExpiry[q.expiry].push_back(&q);

    ImpliedForwardsResult out;
    out.sheetId = in.sheetId;
    out.underlyingId = in.underlyingId;
    out.spot = spot;
    out.filter = flt;
    out.asOfIso = DateToIso(asOf);
    out.dividendCurveId = in.dividendCurveId;

    for (const auto& [expiry, lines] : byExpiry) {
        ImpliedForwardExpiryData e;
        e.expiry = expiry;
        e.expiryIso = lines.front()->expiryIso;
        e.time = dc.yearFraction(asOf, expiry);

        auto dropLine = [&](const OptionQuoteLine& q, const std::string& reason) {
            e.dropped.push_back(DroppedQuoteData{q.expiryIso, q.strike, reason});
        };

        if (expiry <= asOf) {
            for (const auto* q : lines)
                dropLine(*q, "expiry not after as_of_date");
            e.droppedReason = "expiry not after as_of_date";
            out.expiries.push_back(std::move(e));
            continue;
        }

        // Pass 0: per-line quality filters (shared with the vol-surface fit).
        std::vector<Pair> pairs;
        bool zeroSpread = false;
        for (const FilteredQuote& q : filterExpiryLines(lines, flt, e.dropped)) {
            Pair p;
            p.strike = q.strike;
            p.y = q.callMid - q.putMid;
            const double total = q.callSpread + q.putSpread;
            if (!(total > 0.0)) zeroSpread = true;
            p.weight = total > 0.0 ? 1.0 / total : 1.0;
            pairs.push_back(p);
        }
        if (zeroSpread) {
            for (auto& p : pairs)
                p.weight = 1.0;
        }

        auto dropExpiry = [&](const std::string& reason) {
            e.droppedReason = reason;
            e.dropped.push_back(DroppedQuoteData{e.expiryIso, std::nullopt, reason});
            out.expiries.push_back(e);
        };

        if (pairs.size() < flt.minQuotesPerExpiry) {
            dropExpiry(
                "fewer than min_quotes_per_expiry (" + std::to_string(flt.minQuotesPerExpiry) +
                ") usable pairs after the quote filters: " + std::to_string(pairs.size()));
            continue;
        }

        // Pass 1: provisional forward over every surviving pair.
        auto fit1 = weightedLine(pairs);
        if (!fit1 || !(fit1->b < 0.0)) {
            dropExpiry(
                fit1 ? "first-pass slope not negative (implied discount factor not > 0)"
                     : "first-pass strikes do not span (all equal)");
            continue;
        }
        const double f1 = -fit1->a / fit1->b;
        if (!(f1 > 0.0) || !std::isfinite(f1)) {
            dropExpiry("first-pass forward not > 0");
            continue;
        }

        // Pass 2: restrict to the moneyness window around F1.
        const double lo = f1 * (1.0 - flt.moneynessWindow), hi = f1 * (1.0 + flt.moneynessWindow);
        std::vector<Pair> window;
        for (const auto& p : pairs) {
            if (p.strike < lo || p.strike > hi) {
                e.dropped.push_back(
                    DroppedQuoteData{
                        e.expiryIso, p.strike,
                        "outside moneyness window [" + fmt(lo) + ", " + fmt(hi) +
                            "] around first-pass forward " + fmt(f1)});
            } else {
                window.push_back(p);
            }
        }
        if (window.size() < flt.minQuotesPerExpiry) {
            dropExpiry(
                "fewer than min_quotes_per_expiry (" + std::to_string(flt.minQuotesPerExpiry) +
                ") pairs inside the moneyness window: " + std::to_string(window.size()));
            continue;
        }
        auto fit2 = weightedLine(window);
        if (!fit2 || !(fit2->b < 0.0)) {
            dropExpiry(
                fit2 ? "second-pass slope not negative (implied discount factor not > 0)"
                     : "second-pass strikes do not span (all equal)");
            continue;
        }
        const double dfImplied = -fit2->b;
        const double forward = fit2->a / dfImplied;
        if (!(forward > 0.0) || !std::isfinite(forward)) {
            dropExpiry("second-pass forward not > 0");
            continue;
        }

        double ss = 0.0, maxAbs = 0.0;
        for (const auto& p : window) {
            const double r = p.y - (fit2->a + fit2->b * p.strike);
            ss += r * r;
            maxAbs = std::max(maxAbs, std::fabs(r));
        }
        const double dfCurve = curve->discount(expiry);
        const double T = e.time;
        const double r = -std::log(dfCurve) / T;
        const double q = r - std::log(forward / spot) / T;

        e.usable = true;
        e.quotesUsed = static_cast<unsigned>(window.size());
        e.forward = forward;
        e.impliedDiscountFactor = dfImplied;
        e.curveDiscountFactor = dfCurve;
        e.impliedFundingBp = -std::log(dfImplied / dfCurve) / T * 1e4;
        e.impliedDividendYield = q;
        e.fitResidualRmse = std::sqrt(ss / static_cast<double>(window.size()));
        e.maxAbsResidual = maxAbs;
        out.expiries.push_back(std::move(e));
    }

    bool any = false;
    for (const auto& e : out.expiries)
        any = any || e.usable;
    if (!any) {
        std::ostringstream os;
        os << "No expiry of sheet '" << in.sheetId << "' survived the implied-forward fit:";
        for (const auto& e : out.expiries)
            os << " [" << e.expiryIso << ": " << e.droppedReason << "]";
        QUANTRA_INVALID_ARGUMENT(os.str());
    }

    // Dividend curve: as-of node (flat short end) + one node per fitted expiry.
    for (const auto& e : out.expiries) {
        if (!e.usable) continue;
        if (out.dividendCurve.empty()) {
            out.dividendCurve.push_back(DividendCurvePoint{out.asOfIso, *e.impliedDividendYield});
        }
        out.dividendCurve.push_back(DividendCurvePoint{e.expiryIso, *e.impliedDividendYield});
    }
    return out;
}

} // namespace quantra
